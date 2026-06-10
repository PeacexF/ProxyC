#include "db.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static bool db_exec(DB *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db->handle, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[db] SQL error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return false;
    }
    return true;
}

static void db_apply_pragmas(DB *db) {
    db_exec(db, "PRAGMA journal_mode = WAL;");
    db_exec(db, "PRAGMA synchronous = NORMAL;");
    db_exec(db, "PRAGMA cache_size = -10000;");
    db_exec(db, "PRAGMA temp_store = MEMORY;");
}

static bool db_create_schema(DB *db) {
    const char *ddl =
        "CREATE TABLE IF NOT EXISTS proxies ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  address      TEXT UNIQUE NOT NULL,"
        "  status       TEXT    NOT NULL DEFAULT 'unchecked',"
        "  error_type   TEXT    DEFAULT NULL,"
        "  latency_ms   INTEGER DEFAULT NULL,"
        "  last_checked TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_status ON proxies(status);";

    return db_exec(db, ddl);
}

bool db_open(DB *db, const char *path) {
    int rc = sqlite3_open(path, &db->handle);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[db] Cannot open database '%s': %s\n",
                path, sqlite3_errmsg(db->handle));
        sqlite3_close(db->handle);
        db->handle = NULL;
        return false;
    }
    db_apply_pragmas(db);
    return db_create_schema(db);
}

void db_close(DB *db) {
    if (db->handle) {
        sqlite3_close(db->handle);
        db->handle = NULL;
    }
}

int db_import_file(DB *db, const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        perror("[db] fopen");
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    const char *insert_sql =
        "INSERT OR IGNORE INTO proxies (address) VALUES (?);";

    if (sqlite3_prepare_v2(db->handle, insert_sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[db] prepare failed: %s\n",
                sqlite3_errmsg(db->handle));
        fclose(f);
        return -1;
    }

    db_exec(db, "BEGIN TRANSACTION;");

    char line[512];
    int  count        = 0;
    int  skipped      = 0;
    int  batch_size   = 0;
    const int BATCH   = 5000;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0) continue;

        if (!is_valid_proxy_syntax(line)) {
            skipped++;
            continue;
        }

        sqlite3_bind_text(stmt, 1, line, (int)len, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            if (sqlite3_changes(db->handle) > 0) count++;
        }
        sqlite3_reset(stmt);

        if (++batch_size >= BATCH) {
            db_exec(db, "COMMIT; BEGIN TRANSACTION;");
            batch_size = 0;
        }
    }

    db_exec(db, "COMMIT;");
    sqlite3_finalize(stmt);
    fclose(f);

    printf("[db] Imported %d new proxies (skipped %d invalid lines)\n",
           count, skipped);
    return count;
}

int db_fetch_by_status(DB *db, const char *status, char ***out, int limit) {
    const char *sql =
        "SELECT address FROM proxies WHERE status = ? LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[db] prepare failed: %s\n",
                sqlite3_errmsg(db->handle));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 2, limit);

    char **arr = malloc((size_t)limit * sizeof(char *));
    if (!arr) {
        sqlite3_finalize(stmt);
        return -1;
    }

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < limit) {
        const char *addr = (const char *)sqlite3_column_text(stmt, 0);
        arr[n++] = strdup(addr);
    }

    sqlite3_finalize(stmt);
    *out = arr;
    return n;
}

void db_update_result(DB *db, const char *address, bool success, const char *error_type, long latency_ms) {
    const char *sql =
        "UPDATE proxies SET"
        "  status       = ?,"
        "  error_type   = ?,"
        "  latency_ms   = ?,"
        "  last_checked = CURRENT_TIMESTAMP"
        " WHERE address = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[db] prepare failed: %s\n",
                sqlite3_errmsg(db->handle));
        return;
    }

    sqlite3_bind_text(stmt, 1, success ? "active" : "failed", -1, SQLITE_STATIC);
    if (error_type)
        sqlite3_bind_text(stmt, 2, error_type, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)latency_ms);
    sqlite3_bind_text (stmt, 4, address, -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int db_export(DB *db, const char *status, const char *scheme_filter, const char *outfile) {
    const char *sql_all    = "SELECT address FROM proxies WHERE status = ?;";
    const char *sql_scheme =
        "SELECT address FROM proxies WHERE status = ? AND address LIKE ?;";

    sqlite3_stmt *stmt = NULL;
    int rc;

    if (scheme_filter) {
        rc = sqlite3_prepare_v2(db->handle, sql_scheme, -1, &stmt, NULL);
    } else {
        rc = sqlite3_prepare_v2(db->handle, sql_all, -1, &stmt, NULL);
    }

    if (rc != SQLITE_OK) {
        fprintf(stderr, "[db] prepare failed: %s\n",
                sqlite3_errmsg(db->handle));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
    if (scheme_filter) {
        char pattern[64];
        snprintf(pattern, sizeof(pattern), "%s%%", scheme_filter);
        sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);
    }

    FILE *f = fopen(outfile, "w");
    if (!f) {
        perror("[db] fopen export");
        sqlite3_finalize(stmt);
        return -1;
    }

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *addr = (const char *)sqlite3_column_text(stmt, 0);
        fprintf(f, "%s\n", addr);
        count++;
    }

    fclose(f);
    sqlite3_finalize(stmt);
    printf("[db] Exported %d proxies to '%s'\n", count, outfile);
    return count;
}

void db_print_stats(DB *db) {
    const char *sql =
        "SELECT status, COUNT(*) as n FROM proxies GROUP BY status ORDER BY n DESC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) return;

    printf("\n%-15s %s\n", "Status", "Count");
    printf("%-15s %s\n",   "------", "-----");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *status = (const char *)sqlite3_column_text(stmt, 0);
        int n = sqlite3_column_int(stmt, 1);
        printf("%-15s %d\n", status, n);
    }
    printf("\n");
    sqlite3_finalize(stmt);
}
