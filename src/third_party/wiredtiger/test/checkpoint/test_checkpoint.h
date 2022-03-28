/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "test_util.h"

#include <signal.h>

#define URI_BASE "table:__wt" /* File name */

#define ERR_KEY_MISMATCH 0x200001
#define ERR_DATA_MISMATCH 0x200002

/* Magic value to store in FLCS if we have lost track of the corresponding string value. */
#define FLCS_UNKNOWN 255

/*
 * There are three different table types in the test, and a 'special' type of mixed (i.e a mixture
 * of the other three types.
 */
#define MAX_TABLE_TYPE 4
typedef enum { MIX = 0, COL, FIX, LSM, ROW } table_type; /* File type */

/*
 * Per-table cookie structure.
 */
typedef struct {
    int id;
    table_type type; /* Type for table. */
    char uri[128];
} COOKIE;

typedef struct {
    char *home;                           /* Home directory */
    const char *checkpoint_name;          /* Checkpoint name */
    WT_CONNECTION *conn;                  /* WiredTiger connection */
    bool debug_mode;                      /* History store stress test */
    u_int nkeys;                          /* Keys to load */
    u_int nops;                           /* Operations per thread */
    FILE *logfp;                          /* Message log file. */
    int nworkers;                         /* Number workers configured */
    int ntables;                          /* Number tables configured */
    int ntables_created;                  /* Number tables opened */
    volatile int running;                 /* Whether to stop */
    int status;                           /* Exit status */
    bool sweep_stress;                    /* Sweep stress test */
    bool failpoint_hs_delete_key_from_ts; /* Failpoint for hs key deletion. */
    bool hs_checkpoint_timing_stress;     /* History store checkpoint timing stress */
    bool reserved_txnid_timing_stress;    /* Reserved transaction id timing stress */
    bool checkpoint_slow_timing_stress;   /* Checkpoint slow timing stress */
    uint64_t ts_oldest;                   /* Current oldest timestamp */
    uint64_t ts_stable;                   /* Current stable timestamp */
    bool mixed_mode_deletes;              /* Run with mixed mode deletes */
    bool use_timestamps;                  /* Use txn timestamps */
    bool race_timestamps;                 /* Async update to oldest timestamp */
    bool prepare;                         /* Use prepare transactions */
    COOKIE *cookies;                      /* Per-thread info */
    WT_RWLOCK clock_lock;                 /* Clock synchronization */
    wt_thread_t checkpoint_thread;        /* Checkpoint thread */
    wt_thread_t clock_thread;             /* Clock thread */
} GLOBAL;
extern GLOBAL g;

#define log_print_err(m, e, fatal) log_print_err_worker(__func__, __LINE__, m, e, fatal)

void end_checkpoints(void);
uint8_t flcs_encode(const char *);
uint8_t flcs_modify(WT_MODIFY *, int, uint8_t);
int log_print_err_worker(const char *, int, const char *, int, int);
void start_checkpoints(void);
int start_workers(void);
const char *type_to_string(table_type);
int verify_consistency(WT_SESSION *, wt_timestamp_t);
