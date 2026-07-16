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

#pragma once

#include "test_util.h"

#include <pthread.h>
#include <sys/wait.h>
#include <signal.h>

/* Tunables. */
#define MAX_CKPT_INVL 4
#define MAX_POOL_SIZE 64
#define MAX_STARTUP 60
#define MAX_TH 12
#define MAX_TIME 40
#define MIN_POOL_SIZE 2
#define MIN_TH 2
#define MIN_TIME 10

/* URI / file name patterns. */
#define DATA_KEY_MIN 0
#define DATA_KEY_MAX 9
#define READY_FILE "child_ready"
#define SCHEMA_TABLE_FMT "table:schema_%u_%u"
#define SCHEMA_RECORDS_FILE RECORDS_DIR DIR_DELIM_STR "schema-%" PRIu32

/* Connection config. */
#define ENV_CONFIG_DEF "create,statistics=(all),statistics_log=(json,on_close,wait=1)"

/* Test-wide configuration passed from parent to child and to the verifier. */
typedef struct {
    TEST_OPTS *opts;
    char home[1024];
    char page_log_home[PATH_MAX];
    uint32_t nth;
    uint32_t pool_size;
} TEST_CONFIG;

/* Global state shared by all workload threads. */
typedef struct {
    volatile bool stable_set; /* set once the stable timestamp is first advanced */
    uint64_t schema_op_epoch; /* next schema epoch to assign */
    /* Read: a schema thread's create/drop and publish. Write: the checkpoint. */
    pthread_rwlock_t lock;
} WORKLOAD_STATE;

/* Per-thread argument. */
typedef struct {
    TEST_CONFIG *cfg;
    WT_CONNECTION *conn;
    WORKLOAD_STATE *state;
    uint32_t info;
    WT_RAND_STATE rnd;
} THREAD_DATA;

/* workload.c */
void run_workload(TEST_CONFIG *) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/* verify.c */
void verify_schema_state(WT_CONNECTION *conn, TEST_CONFIG *cfg);
