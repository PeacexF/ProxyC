#ifndef CHECKER_H
#define CHECKER_H

#include "db.h"
#include <stddef.h>


typedef struct {
    int  max_concurrent;
    int  batch_size; 
    int  timeout_sec;   
    int  connect_timeout_sec; 
    const char *test_url;   
} CheckerConfig;

void checker_config_default(CheckerConfig *cfg);

void checker_run(DB *db, const char *status, const CheckerConfig *cfg);

#endif
