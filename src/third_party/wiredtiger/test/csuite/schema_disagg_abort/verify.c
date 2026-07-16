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

#include "schema_disagg_abort.h"

/* No durable insert was recorded for the slot's last create. */
#define DATA_COMMIT_TS_NONE UINT64_MAX

/* The last durable create or drop recorded for one URI slot, and its inserted data if any. */
typedef struct {
    uint64_t epoch;
    uint64_t commit_ts; /* DATA_COMMIT_TS_NONE when the create has no durable insert record */
    uint32_t key_min;
    uint32_t key_max;
    bool is_create;
    bool valid;
} SLOT_STATE;

/*
 * parse_schema_records --
 *     Record thread t's last durable operation per slot, plus the data inserted for its last
 *     create. durable_epoch is the highest schema epoch that survived recovery; records above it
 *     never reached a checkpoint before the crash and are ignored, as are records for other
 *     threads.
 */
static void
parse_schema_records(const char *fname, uint32_t t, uint64_t durable_epoch,
  SLOT_STATE states[MAX_POOL_SIZE], uint32_t pool_size)
{
    FILE *fp;
    uint64_t commit_ts, entry_epoch;
    uint32_t key_max, key_min, s, t2;
    char line[256], op[16], rec_uri[128];

    for (s = 0; s < pool_size; s++) {
        states[s].epoch = 0;
        states[s].commit_ts = DATA_COMMIT_TS_NONE;
        states[s].key_min = states[s].key_max = 0;
        states[s].is_create = false;
        states[s].valid = false;
    }

    if ((fp = fopen(fname, "r")) == NULL)
        return;

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "%15s", op) != 1)
            continue;

        if (strcmp(op, "INSERT") == 0) {
            if (sscanf(line, "%*s %" SCNu64 " %" SCNu64 " %" SCNu32 " %" SCNu32 " %127s",
                  &entry_epoch, &commit_ts, &key_min, &key_max, rec_uri) != 5)
                continue;
            if (entry_epoch > durable_epoch)
                continue;
            if (sscanf(rec_uri, SCHEMA_TABLE_FMT, &t2, &s) != 2 || t2 != t || s >= pool_size)
                continue;
            /* The insert belongs to the slot's current create. */
            if (states[s].valid && states[s].is_create && states[s].epoch == entry_epoch) {
                states[s].commit_ts = commit_ts;
                states[s].key_min = key_min;
                states[s].key_max = key_max;
            }
            continue;
        }

        if (sscanf(line, "%*s %" SCNu64 " %127s", &entry_epoch, rec_uri) != 2)
            continue;
        if (entry_epoch > durable_epoch)
            continue;
        if (sscanf(rec_uri, SCHEMA_TABLE_FMT, &t2, &s) != 2 || t2 != t || s >= pool_size)
            continue;
        if (entry_epoch > states[s].epoch) {
            states[s].epoch = entry_epoch;
            states[s].commit_ts = DATA_COMMIT_TS_NONE;
            states[s].is_create = (strcmp(op, "CREATE") == 0);
            states[s].valid = true;
        }
    }
    (void)fclose(fp);
}

/*
 * check_schema_presence --
 *     For each slot with a durable record, assert that a table whose last operation was a create
 *     still exists and one whose last operation was a drop is absent.
 */
static void
check_schema_presence(
  WT_SESSION *session, uint32_t t, const SLOT_STATE states[MAX_POOL_SIZE], uint32_t pool_size)
{
    WT_CURSOR *md_cursor;
    WT_DECL_RET;
    uint32_t s;
    char uri[64];

    /* Validate presence against the metadata entry rather than instantiating each table. */
    testutil_check(session->open_cursor(session, "metadata:", NULL, NULL, &md_cursor));

    for (s = 0; s < pool_size; s++) {
        if (!states[s].valid)
            continue;

        testutil_snprintf(uri, sizeof(uri), SCHEMA_TABLE_FMT, t, s);
        md_cursor->set_key(md_cursor, uri);
        ret = md_cursor->search(md_cursor);
        testutil_assert(ret == 0 || ret == WT_NOTFOUND);

        if (states[s].is_create)
            testutil_assertfmt(ret == 0, "%s missing after recovery (CREATE at epoch %" PRIu64 ")",
              uri, states[s].epoch);
        else
            testutil_assertfmt(
              ret == WT_NOTFOUND, "%s present after recovery (last op was DROP)", uri);
    }

    testutil_check(md_cursor->close(md_cursor));
}

/*
 * check_data_rows --
 *     For each slot whose last checkpointed operation was a CREATE and whose data commit timestamp
 *     is at or below the last checkpoint timestamp, confirm the recorded key range is present.
 *     Slots with no durable insert record, or whose data commit timestamp exceeds last_ckpt_ts, are
 *     skipped.
 */
static void
check_data_rows(WT_SESSION *session, uint32_t t, const SLOT_STATE states[MAX_POOL_SIZE],
  uint32_t pool_size, uint64_t last_ckpt_ts)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint32_t r, s;
    char expected_val[32], key_buf[16], uri[64];
    const char *actual_val;

    for (s = 0; s < pool_size; s++) {
        if (!states[s].valid || !states[s].is_create)
            continue;
        if (states[s].commit_ts == DATA_COMMIT_TS_NONE)
            continue;
        if (last_ckpt_ts > 0 && states[s].commit_ts > last_ckpt_ts)
            continue;

        testutil_snprintf(uri, sizeof(uri), SCHEMA_TABLE_FMT, t, s);
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

        testutil_snprintf(expected_val, sizeof(expected_val), "%" PRIu64, states[s].epoch);
        for (r = states[s].key_min; r <= states[s].key_max; r++) {
            testutil_snprintf(key_buf, sizeof(key_buf), "%" PRIu32, r);
            cursor->set_key(cursor, key_buf);
            ret = cursor->search(cursor);
            testutil_assertfmt(ret == 0, "%s key %s: %s (epoch %" PRIu64 ")", uri, key_buf,
              ret == WT_NOTFOUND ? "missing" : wiredtiger_strerror(ret), states[s].epoch);
            testutil_check(cursor->get_value(cursor, &actual_val));
            testutil_assertfmt(strcmp(actual_val, expected_val) == 0, "%s key %s: got %s want %s",
              uri, key_buf, actual_val, expected_val);
        }
        testutil_check(cursor->close(cursor));
    }
}

/*
 * verify_schema_state --
 *     Verify schema and data state after recovery.
 *
 * Reads the per-thread schema record files and takes last_disaggregated_schema_epoch as the highest
 *     durable schema epoch. Asserts that every table whose last durable operation was a create
 *     exists and holds the right rows, and every one last dropped is gone. Aborts on the first
 *     mismatch.
 */
void
verify_schema_state(WT_CONNECTION *conn, TEST_CONFIG *cfg)
{
    SLOT_STATE states[MAX_POOL_SIZE];
    WT_SESSION *session;
    uint64_t durable_epoch, last_ckpt_ts;
    char fname[128], ts_buf[64];
    uint32_t t;

    durable_epoch = 0;
    testutil_check(conn->query_timestamp(conn, ts_buf, "get=last_disaggregated_schema_epoch"));
    (void)sscanf(ts_buf, "%" SCNx64, &durable_epoch);
    printf("Schema verify: last_disaggregated_schema_epoch = %" PRIu64 "\n", durable_epoch);
    if (durable_epoch == 0)
        testutil_die(EINVAL, "last_disaggregated_schema_epoch is 0 after recovery");

    last_ckpt_ts = 0;
    (void)conn->query_timestamp(conn, ts_buf, "get=last_checkpoint");
    (void)sscanf(ts_buf, "%" SCNx64, &last_ckpt_ts);
    printf("Schema verify: last_checkpoint_timestamp = %" PRIu64 "\n", last_ckpt_ts);

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    for (t = 0; t < cfg->nth; t++) {
        testutil_snprintf(fname, sizeof(fname), SCHEMA_RECORDS_FILE, t);
        parse_schema_records(fname, t, durable_epoch, states, cfg->pool_size);
        check_schema_presence(session, t, states, cfg->pool_size);
        check_data_rows(session, t, states, cfg->pool_size, last_ckpt_ts);
    }

    testutil_check(session->close(session, NULL));
}
