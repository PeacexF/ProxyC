#include "checker.h"
#include "db.h"

#include <curl/curl.h>
#include <pthread.h>
#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESPONSE_BUF_SIZE 4096

typedef struct {
    char   data[RESPONSE_BUF_SIZE];
    size_t len;
} ResponseBuf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    ResponseBuf *buf   = (ResponseBuf *)userdata;
    size_t       bytes = size * nmemb;
    size_t       space = RESPONSE_BUF_SIZE - buf->len - 1;

    if (bytes > space) bytes = space;
    memcpy(buf->data + buf->len, ptr, bytes);
    buf->len += bytes;
    buf->data[buf->len] = '\0';
    return size * nmemb;
}

typedef struct {
    char        address[256];
    ResponseBuf buf;
} HandleCtx;


typedef struct ResultNode {
    char   address[256];
    bool   success;
    char   error_type[64];
    long   latency_ms;
    struct ResultNode *next;
} ResultNode;

typedef struct {
    ResultNode     *head;
    ResultNode     *tail;
    int             count;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    bool            done;
} ResultQueue;

static void rq_init(ResultQueue *q) {
    q->head  = q->tail = NULL;
    q->count = 0;
    q->done  = false;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init (&q->cond, NULL);
}

static void rq_push(ResultQueue *q, const char *address, bool success, const char *error_type, long latency_ms) {
    ResultNode *n = malloc(sizeof(ResultNode));
    if (!n) return;

    strncpy(n->address, address, sizeof(n->address) - 1);
    n->address[sizeof(n->address) - 1] = '\0';
    n->success    = success;
    n->latency_ms = latency_ms;
    if (error_type)
        strncpy(n->error_type, error_type, sizeof(n->error_type) - 1);
    else
        n->error_type[0] = '\0';
    n->next = NULL;

    pthread_mutex_lock(&q->lock);
    if (q->tail) q->tail->next = n;
    else         q->head = n;
    q->tail = n;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}


typedef struct {
    const char  *db_path;
    ResultQueue *queue;
} DBWriterArg;

#define DB_WRITER_BATCH 200

static void writer_apply_pragmas(sqlite3 *h) {
    sqlite3_exec(h, "PRAGMA journal_mode = WAL;",       NULL, NULL, NULL);
    sqlite3_exec(h, "PRAGMA synchronous = NORMAL;",     NULL, NULL, NULL);
    sqlite3_exec(h, "PRAGMA cache_size = -4000;",       NULL, NULL, NULL);
    sqlite3_exec(h, "PRAGMA temp_store = MEMORY;",      NULL, NULL, NULL);
    sqlite3_busy_timeout(h, 30000);
}

static void *db_writer_thread(void *arg) {
    DBWriterArg *a = (DBWriterArg *)arg;
    ResultQueue *q = a->queue;

    sqlite3 *h = NULL;
    if (sqlite3_open(a->db_path, &h) != SQLITE_OK) {
        fprintf(stderr, "[db_writer] Cannot open db: %s\n",
                sqlite3_errmsg(h));
        sqlite3_close(h);
        return NULL;
    }
    writer_apply_pragmas(h);

    const char *sql =
        "UPDATE proxies SET"
        "  status       = ?,"
        "  error_type   = ?,"
        "  latency_ms   = ?,"
        "  last_checked = CURRENT_TIMESTAMP"
        " WHERE address = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[db_writer] prepare failed: %s\n",
                sqlite3_errmsg(h));
        sqlite3_close(h);
        return NULL;
    }

    int pending = 0;
    sqlite3_exec(h, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (q->count == 0 && !q->done)
            pthread_cond_wait(&q->cond, &q->lock);

        ResultNode *batch = q->head;
        q->head = q->tail = NULL;
        q->count = 0;
        bool finished = q->done;
        pthread_mutex_unlock(&q->lock);

        ResultNode *node = batch;
        while (node) {
            ResultNode *next = node->next;

            sqlite3_bind_text(stmt, 1,
                node->success ? "active" : "failed", -1, SQLITE_STATIC);
            if (node->error_type[0])
                sqlite3_bind_text(stmt, 2, node->error_type, -1, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null(stmt, 2);
            sqlite3_bind_int64(stmt, 3, (sqlite3_int64)node->latency_ms);
            sqlite3_bind_text (stmt, 4, node->address, -1, SQLITE_TRANSIENT);

            sqlite3_step(stmt);
            sqlite3_reset(stmt);

            free(node);
            node = next;
            pending++;

            if (pending >= DB_WRITER_BATCH) {
                sqlite3_exec(h, "COMMIT; BEGIN TRANSACTION;", NULL, NULL, NULL);
                pending = 0;
            }
        }

        if (finished) {
            sqlite3_exec(h, "COMMIT;", NULL, NULL, NULL);
            break;
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(h);
    return NULL;
}

static bool ip_matches_origin(const char *address, const char *body) {
    const char *host = address;

    const char *scheme_sep = strstr(address, "://");
    if (scheme_sep) host = scheme_sep + 3;

    const char *at = strchr(host, '@');
    if (at) host = at + 1;

    char ip[64] = {0};
    const char *colon = strrchr(host, ':');
    if (!colon) return false;

    size_t len = (size_t)(colon - host);
    if (len == 0 || len >= sizeof(ip)) return false;
    memcpy(ip, host, len);

    return strstr(body, ip) != NULL;
}


static CURL *make_easy_handle(const char *address, HandleCtx *ctx, const CheckerConfig *cfg) {
    CURL *easy = curl_easy_init();
    if (!easy) return NULL;

    char proxy_url[300];
    if (strstr(address, "://"))
        snprintf(proxy_url, sizeof(proxy_url), "%s", address);
    else
        snprintf(proxy_url, sizeof(proxy_url), "http://%s", address);

    strncpy(ctx->address, address, sizeof(ctx->address) - 1);
    ctx->address[sizeof(ctx->address) - 1] = '\0';
    ctx->buf.len     = 0;
    ctx->buf.data[0] = '\0';

    curl_easy_setopt(easy, CURLOPT_URL,            cfg->test_url);
    curl_easy_setopt(easy, CURLOPT_PROXY,          proxy_url);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT,        (long)cfg->timeout_sec);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, (long)cfg->connect_timeout_sec);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA,      &ctx->buf);
    curl_easy_setopt(easy, CURLOPT_PRIVATE,        ctx);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS,     1L);

    return easy;
}

static void process_done(CURL *easy, CURLcode res, ResultQueue *q) {
    HandleCtx *ctx = NULL;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &ctx);

    bool success = false;
    char error_type[64] = {0};
    long latency_ms = 0;

    double total_time = 0.0;
    curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME, &total_time);
    latency_ms = (long)(total_time * 1000.0);

    if (res != CURLE_OK) {
        switch (res) {
        case CURLE_OPERATION_TIMEDOUT:
            strncpy(error_type, "TIMEOUT",          sizeof(error_type) - 1); break;
        case CURLE_COULDNT_CONNECT:
            strncpy(error_type, "CONNECTION_ERROR", sizeof(error_type) - 1); break;
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST:
            strncpy(error_type, "RESOLVE_ERROR",    sizeof(error_type) - 1); break;
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
            strncpy(error_type, "READ_TIMEOUT",     sizeof(error_type) - 1); break;
        default:
            snprintf(error_type, sizeof(error_type), "CURL_%d", (int)res);   break;
        }
    } else {
        long http_code = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code != 200) {
            snprintf(error_type, sizeof(error_type), "HTTP_ERROR_%ld", http_code);
        } else if (!ip_matches_origin(ctx->address, ctx->buf.data)) {
            strncpy(error_type, "IP_MISMATCH", sizeof(error_type) - 1);
        } else {
            success = true;
        }
    }

    rq_push(q, ctx->address, success,
            success ? NULL : error_type,
            latency_ms);

    free(ctx);
}

void checker_config_default(CheckerConfig *cfg) {
    cfg->max_concurrent      = 512;
    cfg->batch_size          = 2000;
    cfg->timeout_sec         = 10;
    cfg->connect_timeout_sec = 5;
    cfg->test_url            = "http://httpbin.org/get";
}

void checker_run(DB *db, const char *status, const CheckerConfig *cfg) {
    curl_global_init(CURL_GLOBAL_ALL);

    ResultQueue  queue;
    rq_init(&queue);

    DBWriterArg writer_arg = { .db_path = db->path, .queue = &queue };
    pthread_t   writer_tid;
    pthread_create(&writer_tid, NULL, db_writer_thread, &writer_arg);

    CURLM *multi = curl_multi_init();
    curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS, (long)cfg->max_concurrent);

    int active  = 0;
    int total   = 0;
    int fetched = 0;
    int idx     = 0;

    char **addrs = NULL;

    sqlite3_busy_timeout(db->handle, 30000);

    fetched = db_fetch_by_status(db, status, &addrs, cfg->batch_size);
    printf("[checker] Starting — %d proxies in first batch\n", fetched);

    while (fetched > 0 || active > 0) {

        while (active < cfg->max_concurrent && idx < fetched) {
            HandleCtx *ctx = calloc(1, sizeof(HandleCtx));
            if (!ctx) break;

            CURL *easy = make_easy_handle(addrs[idx], ctx, cfg);
            if (!easy) { free(ctx); idx++; continue; }

            curl_multi_add_handle(multi, easy);
            active++;
            idx++;
        }

        int running = 0;
        curl_multi_perform(multi, &running);

        if (running) {
            int numfds = 0;
            curl_multi_wait(multi, NULL, 0, 100, &numfds);
            curl_multi_perform(multi, &running);
        }

        CURLMsg *msg;
        int msgs_left = 0;
        while ((msg = curl_multi_info_read(multi, &msgs_left)) != NULL) {
            if (msg->msg == CURLMSG_DONE) {
                CURL    *easy = msg->easy_handle;
                CURLcode res  = msg->data.result;

                curl_multi_remove_handle(multi, easy);
                process_done(easy, res, &queue);
                curl_easy_cleanup(easy);

                active--;
                total++;

                if (total % 500 == 0)
                    printf("[checker] Processed %d proxies...\n", total);
            }
        }

        if (idx >= fetched && active < cfg->max_concurrent) {
            for (int i = 0; i < fetched; i++) free(addrs[i]);
            free(addrs);
            addrs = NULL;

            fetched = db_fetch_by_status(db, status, &addrs, cfg->batch_size);
            idx = 0;

            if (fetched > 0)
                printf("[checker] Fetching next batch of %d proxies\n", fetched);
        }
    }

    for (int i = 0; i < fetched; i++) free(addrs[i]);
    free(addrs);

    curl_multi_cleanup(multi);
    curl_global_cleanup();

    pthread_mutex_lock(&queue.lock);
    queue.done = true;
    pthread_cond_signal(&queue.cond);
    pthread_mutex_unlock(&queue.lock);

    pthread_join(writer_tid, NULL);

    pthread_mutex_destroy(&queue.lock);
    pthread_cond_destroy (&queue.cond);

    printf("[checker] Done. Total processed: %d\n", total);
}