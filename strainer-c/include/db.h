#ifndef DB_H
#define DB_H

#include <sqlite3.h>
#include <stdbool.h>


typedef struct {
    sqlite3    *handle;
    char        path[512];
} DB;

bool db_open(DB *db, const char *path);

void db_close(DB *db);

int db_import_file(DB *db, const char *filepath);

int db_fetch_by_status(DB *db, const char *status, char ***out, int limit);

void db_update_result(DB *db, const char *address, bool success, const char *error_type, long latency_ms);

int db_export(DB *db, const char *status, const char *scheme_filter, const char *outfile);

void db_print_stats(DB *db);

#endif