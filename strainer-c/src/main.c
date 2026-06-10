#include "db.h"
#include "checker.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>


static void print_usage(const char *prog) {
    printf(
        "Usage:\n"
        "  %s --import  <file>                Import proxies\n"
        "  %s --check                         Check unchecked proxies\n"
        "  %s --retry                         Retry failed proxies\n"
        "  %s --export  <file> [--scheme X]   Export active proxies\n"
        "  %s --stats                         Show statistics\n"
        "\nOptions:\n"
        "  --db      <path>   SQLite DB path     (default: data/proxies.db)\n"
        "  --threads <n>      Concurrent conns   (default: 512)\n"
        "  --timeout <n>      Timeout seconds    (default: 10)\n"
        "  --url     <url>    Test URL           (default: http://httpbin.org/get)\n"
        "  --scheme  <s>      Scheme filter for --export (e.g. socks5://)\n",
        prog, prog, prog, prog, prog
    );
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    enum {
        OPT_IMPORT = 1,
        OPT_CHECK,
        OPT_RETRY,
        OPT_EXPORT,
        OPT_STATS,
        OPT_DB,
        OPT_THREADS,
        OPT_TIMEOUT,
        OPT_URL,
        OPT_SCHEME,
        OPT_HELP,
    };

    static struct option long_opts[] = {
        { "import",  required_argument, NULL, OPT_IMPORT  },
        { "check",   no_argument,       NULL, OPT_CHECK   },
        { "retry",   no_argument,       NULL, OPT_RETRY   },
        { "export",  required_argument, NULL, OPT_EXPORT  },
        { "stats",   no_argument,       NULL, OPT_STATS   },
        { "db",      required_argument, NULL, OPT_DB      },
        { "threads", required_argument, NULL, OPT_THREADS },
        { "timeout", required_argument, NULL, OPT_TIMEOUT },
        { "url",     required_argument, NULL, OPT_URL     },
        { "scheme",  required_argument, NULL, OPT_SCHEME  },
        { "help",    no_argument,       NULL, OPT_HELP    },
        { NULL,      0,                 NULL, 0           },
    };

    const char *db_path      = "data/proxies.db";
    const char *import_file  = NULL;
    const char *export_file  = NULL;
    const char *scheme_filter = NULL;
    bool do_check  = false;
    bool do_retry  = false;
    bool do_stats  = false;

    CheckerConfig cfg;
    checker_config_default(&cfg);

    int opt;
    while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (opt) {
        case OPT_IMPORT:  import_file        = optarg;       break;
        case OPT_CHECK:   do_check           = true;         break;
        case OPT_RETRY:   do_retry           = true;         break;
        case OPT_EXPORT:  export_file        = optarg;       break;
        case OPT_STATS:   do_stats           = true;         break;
        case OPT_DB:      db_path            = optarg;       break;
        case OPT_THREADS: cfg.max_concurrent = atoi(optarg); break;
        case OPT_TIMEOUT: cfg.timeout_sec    = atoi(optarg); break;
        case OPT_URL:     cfg.test_url       = optarg;       break;
        case OPT_SCHEME:  scheme_filter      = optarg;       break;
        case OPT_HELP:
        default:
            print_usage(argv[0]);
            return opt == OPT_HELP ? 0 : 1;
        }
    }

    DB db;
    if (!db_open(&db, db_path)) {
        fprintf(stderr, "Failed to open database at '%s'\n", db_path);
        return 1;
    }

    int exit_code = 0;

    if (import_file) {
        printf("[strainer] Importing from '%s'...\n", import_file);
        int n = db_import_file(&db, import_file);
        if (n < 0) exit_code = 1;
    }

    if (do_check) {
        printf("[strainer] Checking unchecked proxies "
               "(concurrency=%d, timeout=%ds)\n",
               cfg.max_concurrent, cfg.timeout_sec);
        checker_run(&db, "unchecked", &cfg);
    }

    if (do_retry) {
        printf("[strainer] Resetting retryable failures to unchecked...\n");
        char *err = NULL;
        sqlite3_exec(db.handle,
            "UPDATE proxies SET status='unchecked', error_type=NULL, latency_ms=NULL"
            " WHERE error_type IN ('TIMEOUT','CONNECTION_ERROR','READ_TIMEOUT');",
            NULL, NULL, &err);
        if (err) {
            fprintf(stderr, "[strainer] SQL error: %s\n", err);
            sqlite3_free(err);
        }
        printf("[strainer] Re-checking...\n");
        checker_run(&db, "unchecked", &cfg);
    }

    if (export_file) {
        printf("[strainer] Exporting active proxies to '%s'", export_file);
        if (scheme_filter) printf(" (scheme: %s)", scheme_filter);
        printf("...\n");
        int n = db_export(&db, "active", scheme_filter, export_file);
        if (n < 0) exit_code = 1;
    }

    if (do_stats) {
        db_print_stats(&db);
    }

    if (!import_file && !do_check && !do_retry && !export_file && !do_stats) {
        print_usage(argv[0]);
        exit_code = 1;
    }

    db_close(&db);
    return exit_code;
}
