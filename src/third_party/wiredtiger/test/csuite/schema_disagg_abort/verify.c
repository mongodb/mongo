/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Verification: run by the parent after recovery. Replays the record files against the recovered
 * database state.
 */

#include "schema_disagg_abort.h"

/*
 * No durable insert was recorded for the slot's last create.
 */
#define DATA_COMMIT_TS_NONE WT_TS_NONE

/* The last durable create or drop recorded for one URI slot, and its inserted data if any. */
typedef struct {
    uint64_t epoch;
    uint64_t commit_ts; /* DATA_COMMIT_TS_NONE when the create has no durable insert record */
    uint32_t key_min;
    uint32_t key_max;
    bool is_create;
    bool valid;
    char uri[64]; /* filled when valid, so the checks need not rebuild it */
} SLOT_STATE;

/*
 * parse_record_uri --
 *     Parse and filter one record's URI: true when it belongs to the given node and thread and its
 *     slot is inside the pool, reporting the slot.
 */
static bool
parse_record_uri(
  const char *rec_uri, uint32_t node, uint32_t t, uint32_t pool_size, uint32_t *slotp)
{
    uint32_t n2, s, t2;

    if (sscanf(rec_uri, SCHEMA_TABLE_FMT, &n2, &t2, &s) != 3 || n2 != node || t2 != t ||
      s >= pool_size)
        return (false);
    *slotp = s;
    return (true);
}

/*
 * parse_schema_records --
 *     Record thread t's last durable operation per slot, plus the data inserted for its last
 *     create. durable_epoch is the highest schema epoch that survived recovery; records above it
 *     never reached a checkpoint before the crash and are ignored, as are records for other
 *     threads.
 */
static void
parse_schema_records(const char *fname, uint32_t node, uint32_t t, uint64_t durable_epoch,
  SLOT_STATE states[MAX_POOL_SIZE], uint32_t pool_size)
{
    FILE *fp;
    testutil_assert_errno((fp = fopen(fname, "r")) != NULL);

    /* Zero state is fully valid: invalid slot, no durable insert (DATA_COMMIT_TS_NONE). */
    memset(states, 0, sizeof(SLOT_STATE) * pool_size);
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char op[16];
        if (sscanf(line, "%15s", op) != 1)
            continue;

        uint64_t entry_epoch;
        uint32_t s;
        char rec_uri[128];

        if (strcmp(op, "INSERT") == 0) {
            uint64_t commit_ts;
            uint32_t key_max, key_min;
            if (sscanf(line, "%*s %" SCNu64 " %" SCNu32 " %" SCNu32 " %127s", &commit_ts, &key_min,
                  &key_max, rec_uri) != 4)
                continue;
            if (commit_ts > durable_epoch)
                continue;
            if (!parse_record_uri(rec_uri, node, t, pool_size, &s))
                continue;
            /*
             * The insert belongs to the slot's current create: the per-thread records are in apply
             * order and an insert immediately follows its create. A cut-off create cannot capture a
             * stray insert - a commit's value always exceeds its table's create epoch, so the
             * durability cutoff above already dropped the insert too.
             */
            if (states[s].valid && states[s].is_create) {
                states[s].commit_ts = commit_ts;
                states[s].key_min = key_min;
                states[s].key_max = key_max;
            }
            continue;
        }

        if (sscanf(line, "%*s %" SCNu64 " %127s", &entry_epoch, rec_uri) != 2)
            continue;
        if (!parse_record_uri(rec_uri, node, t, pool_size, &s))
            continue;
        if (entry_epoch > durable_epoch)
            continue;
        if (entry_epoch > states[s].epoch) {
            states[s].epoch = entry_epoch;
            states[s].commit_ts = DATA_COMMIT_TS_NONE;
            states[s].is_create = (strcmp(op, "CREATE") == 0);
            states[s].valid = true;
            testutil_snprintf(states[s].uri, sizeof(states[s].uri), "%s", rec_uri);
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
  WT_SESSION *session, const SLOT_STATE states[MAX_POOL_SIZE], uint32_t pool_size)
{
    /* Validate presence against the metadata entry rather than instantiating each table. */
    WT_CURSOR *md_cursor;
    testutil_check(session->open_cursor(session, "metadata:", NULL, NULL, &md_cursor));

    for (uint32_t s = 0; s < pool_size; s++) {
        if (!states[s].valid)
            continue;

        md_cursor->set_key(md_cursor, states[s].uri);
        const int ret = md_cursor->search(md_cursor);
        testutil_assert(ret == 0 || ret == WT_NOTFOUND);

        if (states[s].is_create)
            testutil_assertfmt(ret == 0, "%s missing after recovery (CREATE at epoch %" PRIu64 ")",
              states[s].uri, states[s].epoch);
        else
            testutil_assertfmt(
              ret == WT_NOTFOUND, "%s present after recovery (last op was DROP)", states[s].uri);
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
check_data_rows(WT_SESSION *session, const SLOT_STATE states[MAX_POOL_SIZE], uint32_t pool_size,
  uint64_t last_ckpt_ts)
{
    for (uint32_t s = 0; s < pool_size; s++) {
        if (!states[s].valid || !states[s].is_create)
            continue;
        if (states[s].commit_ts == DATA_COMMIT_TS_NONE)
            continue;
        if (last_ckpt_ts > 0 && states[s].commit_ts > last_ckpt_ts)
            continue;

        WT_CURSOR *cursor;
        testutil_check(session->open_cursor(session, states[s].uri, NULL, NULL, &cursor));

        /* Rows are valued with their commit timestamp; a mismatch means another generation's
         * data. */
        char expected_val[32];
        testutil_snprintf(expected_val, sizeof(expected_val), "%" PRIu64, states[s].commit_ts);
        for (uint32_t r = states[s].key_min; r <= states[s].key_max; r++) {
            char key_buf[16];
            testutil_snprintf(key_buf, sizeof(key_buf), "%" PRIu32, r);
            cursor->set_key(cursor, key_buf);
            const int ret = cursor->search(cursor);
            testutil_assertfmt(ret == 0, "%s key %s: %s (epoch %" PRIu64 ")", states[s].uri,
              key_buf, ret == WT_NOTFOUND ? "missing" : wiredtiger_strerror(ret), states[s].epoch);

            const char *actual_val;
            testutil_check(cursor->get_value(cursor, &actual_val));
            testutil_assertfmt(strcmp(actual_val, expected_val) == 0, "%s key %s: got %s want %s",
              states[s].uri, key_buf, actual_val, expected_val);
        }
        testutil_check(cursor->close(cursor));
    }
}

/*
 * verify_schema_state --
 *     Verify schema and data state after recovery.
 *
 * Reads every node's per-thread leader record files (each node logs only its own operations while
 *     it leads; the shared page log makes all of them visible to any recovered node) and takes
 *     last_disaggregated_schema_epoch as the highest durable schema epoch. Asserts that every table
 *     whose last durable operation was a create exists and holds the right rows, and every one last
 *     dropped is gone. Aborts on the first mismatch.
 */
void
verify_schema_state(WT_CONNECTION *conn, const TEST_CONFIG *cfg)
{
    const uint64_t durable_epoch = query_ts(conn, "last_disaggregated_schema_epoch");
    println("Schema verify: last_disaggregated_schema_epoch = %" PRIu64, durable_epoch);
    /* Nothing durable means no record is below the cutoff, so there is nothing to check. */
    if (durable_epoch == 0) {
        println("Schema verify: nothing durable, no expectations to check");
        return;
    }

    const uint64_t last_ckpt_ts = query_ts(conn, "last_checkpoint");
    println("Schema verify: last_checkpoint_timestamp = %" PRIu64, last_ckpt_ts);

    WT_SESSION *session;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    for (uint32_t n = 0; n < MAX_NODES; n++)
        for (uint32_t t = 0; t < cfg->nth; t++) {
            char fname[128];
            testutil_snprintf(fname, sizeof(fname), LEADER_RECORDS_FILE, n, t);

            /*
             * A missing record file means there are no expectations to verify for this thread:
             * record files are created lazily, so a thread that never got to operate (or a node
             * that never led) leaves none.
             */
            if (!testutil_exists(NULL, fname))
                continue;

            SLOT_STATE states[MAX_POOL_SIZE];
            parse_schema_records(fname, n, t, durable_epoch, states, cfg->pool_size);
            check_schema_presence(session, states, cfg->pool_size);
            check_data_rows(session, states, cfg->pool_size, last_ckpt_ts);
        }

    testutil_check(session->close(session, NULL));
}

/*
 * verify_relay_prefix --
 *     Check the integrity of the event relay: everything a node recorded while following must be an
 *     exact prefix of what its peer recorded while leading, per thread.
 *
 * The leader writes its own record before relaying the event, and the pipe preserves order, so any
 *     divergence or reordering is a relay bug. A SIGKILL can truncate the recorder's final line, so
 *     a partial trailing line is accepted if it is a prefix of the peer's line.
 */
void
verify_relay_prefix(const TEST_CONFIG *cfg)
{
    uint32_t checked = 0;
    for (uint32_t n = 0; n < MAX_NODES; n++)
        for (uint32_t t = 0; t < cfg->nth; t++) {
            char follower_fname[128], leader_fname[128];
            testutil_snprintf(follower_fname, sizeof(follower_fname), FOLLOWER_RECORDS_FILE, n, t);
            testutil_snprintf(leader_fname, sizeof(leader_fname), LEADER_RECORDS_FILE, 1 - n, t);

            /* No relay file means no events were relayed for this thread; nothing to check. */
            if (!testutil_exists(NULL, follower_fname))
                continue;
            testutil_assertfmt(testutil_exists(NULL, leader_fname),
              "%s exists but the peer's %s does not", follower_fname, leader_fname);

            FILE *ffp, *sfp;
            testutil_assert_errno((ffp = fopen(follower_fname, "r")) != NULL);
            testutil_assert_errno((sfp = fopen(leader_fname, "r")) != NULL);

            char fline[256], sline[256];
            uint32_t lineno = 0;
            while (fgets(fline, sizeof(fline), ffp) != NULL) {
                ++lineno;
                testutil_assertfmt(fgets(sline, sizeof(sline), sfp) != NULL,
                  "%s line %" PRIu32 " has no counterpart in %s", follower_fname, lineno,
                  leader_fname);

                const size_t flen = strlen(fline);
                if (flen > 0 && fline[flen - 1] == '\n')
                    testutil_assertfmt(strcmp(fline, sline) == 0,
                      "%s diverges from %s at line %" PRIu32 ": \"%s\" vs \"%s\"", follower_fname,
                      leader_fname, lineno, fline, sline);
                else
                    /* Partial trailing line: the recorder was killed mid-write. */
                    testutil_assertfmt(strncmp(sline, fline, flen) == 0,
                      "%s truncated line %" PRIu32 " is not a prefix of %s: \"%s\" vs \"%s\"",
                      follower_fname, lineno, leader_fname, fline, sline);
            }

            (void)fclose(ffp);
            (void)fclose(sfp);
            ++checked;
        }

    println("Relay verify: %" PRIu32
            " follower record files are prefixes of the peer's leader records",
      checked);
}
