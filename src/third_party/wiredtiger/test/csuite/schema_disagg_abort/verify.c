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
    uint64_t schema_ts; /* publish epoch, or the legacy operation's test timestamp */
    uint64_t commit_ts; /* DATA_COMMIT_TS_NONE when the create has no durable insert record */
    uint32_t key_min;
    uint32_t key_max;
    bool is_create;
    bool valid;
    bool newer_schema_op; /* legacy schema record above the checkpoint timestamp */
    char uri[64];         /* filled when valid, so the checks need not rebuild it */
} SLOT_STATE;

/* What the checks asserted, so a run that verifies nothing is visible in the log. */
typedef struct {
    uint32_t present; /* slots asserted present */
    uint32_t absent;  /* slots asserted absent */
    uint32_t rows;    /* slots whose rows were compared */
    uint32_t skipped_slots;
    uint32_t skipped_rows;
} VERIFY_STATS;

/*
 * parse_record_uri --
 *     Parse and filter one record's URI: true when it belongs to the given node and thread and its
 *     slot is inside the pool, reporting the slot.
 */
static bool
parse_record_uri(
  const char *rec_uri, uint32_t node, uint32_t t, uint32_t pool_size, uint32_t *slotp)
{
    uint32_t g, n2, s, t2;

    if (sscanf(rec_uri, SCHEMA_TABLE_FMT, &n2, &t2, &s, &g) != 4 || n2 != node || t2 != t ||
      s >= pool_size)
        return (false);
    *slotp = s;
    return (true);
}

/*
 * parse_schema_records --
 *     Read a record file and accumulate the last durable operation per slot. Epoch-mode records
 *     above the cutoff are not durable; legacy schema records there have an unknown outcome.
 *
 * This function is called for both leader's and follower's record files to recreate the last
 *     durable state of the database after recovery.
 */
static void
parse_schema_records(const char *fname, uint32_t node, uint32_t t, uint64_t durable_ts,
  bool epoch_less, SLOT_STATE states[MAX_POOL_SIZE], uint32_t pool_size)
{
    FILE *fp;
    testutil_assert_errno((fp = fopen(fname, "r")) != NULL);

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        /* A worker killed mid-write leaves a partial line, which can only be the file's last. */
        const bool torn = strchr(line, '\n') == NULL;

        char op[16];

        if (sscanf(line, "%15s", op) != 1) {
            testutil_assertfmt(torn, "%s: unreadable record \"%s\"", fname, line);
            continue;
        }

        char rec_uri[128];
        uint64_t op_ts; /* publish epoch, legacy schema timestamp, or insert commit timestamp */
        uint32_t key_max = 0, key_min = 0, s;
        bool parsed = false;

        /* Every record is "<op> <op_ts> [<key range>] <uri>". */
        if (strcmp(op, "CREATE") == 0 || strcmp(op, "DROP") == 0)
            parsed = sscanf(line, "%*s %" SCNu64 " %127s", &op_ts, rec_uri) == 2;
        else if (strcmp(op, "INSERT") == 0)
            parsed = sscanf(line, "%*s %" SCNu64 " %" SCNu32 " %" SCNu32 " %127s", &op_ts, &key_min,
                       &key_max, rec_uri) == 4;
        else
            testutil_assertfmt(false, "%s: unknown record op \"%s\"", fname, op);

        if (!parsed) {
            testutil_assertfmt(torn, "%s: malformed record \"%s\"", fname, line);
            continue;
        }

        if (!parse_record_uri(rec_uri, node, t, pool_size, &s))
            continue;
        /* Legacy schema entries above the checkpoint timestamp may or may not have been drained. */
        if (op_ts > durable_ts) {
            if (epoch_less && (strcmp(op, "CREATE") == 0 || strcmp(op, "DROP") == 0))
                states[s].newer_schema_op = true;
            continue;
        }

        if (strcmp(op, "INSERT") == 0) {
            /*
             * Each CREATE may be followed by several rounds of INSERT's, keep only the latest one
             * because earlier values are overwritten.
             */
            const bool latest_insert = states[s].valid && states[s].is_create &&
              op_ts > states[s].schema_ts && op_ts > states[s].commit_ts;

            if (latest_insert) {
                states[s].commit_ts = op_ts;
                states[s].key_min = key_min;
                states[s].key_max = key_max;
            }
        } else if (op_ts > states[s].schema_ts) { /* most recent CREATE or DROP */
            states[s].schema_ts = op_ts;
            states[s].commit_ts = DATA_COMMIT_TS_NONE;
            states[s].is_create = strcmp(op, "CREATE") == 0;
            states[s].valid = true;
            testutil_snprintf(states[s].uri, sizeof(states[s].uri), "%s", rec_uri);
        }
    }
    (void)fclose(fp);
}

/*
 * check_schema_presence --
 *     Check table presence where legacy checkpoint and role-switch semantics make it knowable.
 */
static void
check_schema_presence(WT_SESSION *session, const TEST_CONFIG *cfg,
  const SLOT_STATE states[MAX_POOL_SIZE], uint32_t pool_size, VERIFY_STATS *stats)
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

        const bool uncertain_tail = cfg->epoch_less && states[s].newer_schema_op;
        /* A legacy step-up rebuilds shared metadata from local metadata, and enqueue creates
         * only, so a drop applied while following is resurrected. */
        const bool switched_legacy_drop =
          cfg->epoch_less && cfg->switch_interval != 0 && !states[s].is_create;

        if (states[s].is_create && !uncertain_tail) {
            testutil_assertfmt(ret == 0, "%s missing after recovery (CREATE at %" PRIu64 ")",
              states[s].uri, states[s].schema_ts);
            ++stats->present;
        } else if (!states[s].is_create && !uncertain_tail && !switched_legacy_drop) {
            testutil_assertfmt(
              ret == WT_NOTFOUND, "%s present after recovery (last op was DROP)", states[s].uri);
            ++stats->absent;
        } else
            ++stats->skipped_slots;
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
  uint64_t last_ckpt_ts, VERIFY_STATS *stats)
{
    for (uint32_t s = 0; s < pool_size; s++) {
        if (!states[s].valid || !states[s].is_create)
            continue;
        if (states[s].commit_ts == DATA_COMMIT_TS_NONE)
            continue;
        if (last_ckpt_ts > 0 && states[s].commit_ts > last_ckpt_ts)
            continue;
        if (states[s].newer_schema_op) {
            ++stats->skipped_rows;
            continue;
        }
        ++stats->rows;

        WT_CURSOR *cursor;
        testutil_check(session->open_cursor(session, states[s].uri, NULL, NULL, &cursor));

        /* Rows hold their commit timestamp; a mismatch means another generation's
         * data. */
        char expected_val[32];
        testutil_snprintf(expected_val, sizeof(expected_val), "%" PRIu64, states[s].commit_ts);
        for (uint32_t r = states[s].key_min; r <= states[s].key_max; r++) {
            char key_buf[16];
            testutil_snprintf(key_buf, sizeof(key_buf), "%" PRIu32, r);
            cursor->set_key(cursor, key_buf);
            const int ret = cursor->search(cursor);
            testutil_assertfmt(ret == 0, "%s key %s: %s (schema op %" PRIu64 ")", states[s].uri,
              key_buf, ret == WT_NOTFOUND ? "missing" : wiredtiger_strerror(ret),
              states[s].schema_ts);

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
 * Every node's records are checked against the recovered database, since the shared page log makes
 *     all of them visible to any node. Epoch mode uses the checkpoint's schema epoch as the cutoff;
 *     legacy mode uses its stable timestamp.
 */
void
verify_schema_state(WT_CONNECTION *conn, const TEST_CONFIG *cfg)
{
    const uint64_t last_ckpt_ts = query_ts(conn, TS_LAST_CHECKPOINT);
    const uint64_t durable_epoch = query_ts(conn, TS_LAST_SCHEMA_EPOCH);
    /* An epoch-less database has no schema epoch; a -v rerun that forgot -e would check nothing. */
    testutil_assertfmt(!cfg->epoch_less || durable_epoch == 0,
      "-e verify against a database with schema epoch %" PRIu64, durable_epoch);

    const uint64_t durable_ts = cfg->epoch_less ? last_ckpt_ts : durable_epoch;
    println("Schema verify: %s = %" PRIu64,
      cfg->epoch_less ? "last_checkpoint_timestamp" : "last_disaggregated_schema_epoch",
      durable_ts);
    /* Nothing durable means no record is below the cutoff, so there is nothing to check. */
    if (durable_ts == 0) {
        println("Schema verify: nothing durable, no expectations to check");
        return;
    }

    if (!cfg->epoch_less)
        println("Schema verify: last_checkpoint_timestamp = %" PRIu64, last_ckpt_ts);

    WT_SESSION *session;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    VERIFY_STATS stats = {0};
    for (uint32_t n = 0; n < MAX_NODES; n++)
        for (uint32_t t = 0; t < cfg->thread_count; t++) {
            char lname[128], mname[128];
            testutil_snprintf(lname, sizeof(lname), LEADER_RECORDS_FILE, n, t);
            /* The peer's follower file mirrors what node n originated on this thread. */
            testutil_snprintf(mname, sizeof(mname), FOLLOWER_RECORDS_FILE, 1 - n, t);

            /*
             * Union the node's own records with the peer's mirror of them: a node relays before it
             * records, so a SIGKILL can leave the last operation recorded only on the peer - the
             * survivor whose checkpoints can then make that operation durable. Either file may be
             * missing; they are created lazily.
             */
            const bool have_own = testutil_exists(NULL, lname);
            const bool have_mirror = testutil_exists(NULL, mname);
            if (!have_own && !have_mirror)
                continue;

            /* Zero state is fully valid: invalid slot, no durable insert (DATA_COMMIT_TS_NONE). */
            SLOT_STATE states[MAX_POOL_SIZE];
            memset(states, 0, sizeof(SLOT_STATE) * cfg->pool_size);
            if (have_own)
                parse_schema_records(
                  lname, n, t, durable_ts, cfg->epoch_less, states, cfg->pool_size);
            if (have_mirror)
                parse_schema_records(
                  mname, n, t, durable_ts, cfg->epoch_less, states, cfg->pool_size);
            check_schema_presence(session, cfg, states, cfg->pool_size, &stats);
            check_data_rows(session, states, cfg->pool_size, last_ckpt_ts, &stats);
        }

    println("Schema verify: asserted %" PRIu32 " present, %" PRIu32 " absent, %" PRIu32
            " with rows; skipped %" PRIu32 " slots, %" PRIu32 " row checks",
      stats.present, stats.absent, stats.rows, stats.skipped_slots, stats.skipped_rows);

    testutil_check(session->close(session, NULL));
}

/*
 * verify_relay_pair --
 *     Check one follower record file against the peer's leader file for the same thread: the
 *     follower's lines must match the leader's, line for line.
 *
 * A SIGKILL can truncate either side's final line, and can cost the leader's file the one event in
 *     flight between the relay and the record. Both show up as a mismatch on what must then be the
 *     last line; anything past that is a relay bug.
 */
static void
verify_relay_pair(const char *follower_fname, const char *leader_fname)
{
    FILE *ffp, *sfp;
    testutil_assert_errno((ffp = fopen(follower_fname, "r")) != NULL);
    testutil_assert_errno((sfp = fopen(leader_fname, "r")) != NULL);

    char fline[256], sline[256];
    uint32_t lineno = 0;
    while (fgets(fline, sizeof(fline), ffp) != NULL) {
        ++lineno;
        /* The leader may lack this line: killed after relaying the event, before recording it. */
        if (fgets(sline, sizeof(sline), sfp) == NULL)
            break;
        if (strcmp(fline, sline) == 0)
            continue;
        /*
         * Only a truncated final line may differ, so the shorter must prefix the longer: two
         * complete lines are prefixes of each other only when they are equal.
         */
        testutil_assertfmt(strncmp(fline, sline, WT_MIN(strlen(fline), strlen(sline))) == 0,
          "%s diverges from %s at line %" PRIu32 ": \"%s\" vs \"%s\"", follower_fname, leader_fname,
          lineno, fline, sline);
        break;
    }
    /* Whatever ended the comparison was an end-of-file effect, so nothing may follow it. */
    testutil_assertfmt(fgets(fline, sizeof(fline), ffp) == NULL,
      "%s runs past %s after line %" PRIu32, follower_fname, leader_fname, lineno);

    (void)fclose(ffp);
    (void)fclose(sfp);
}

/*
 * verify_relay_prefix --
 *     Check the integrity of the event relay: everything a node recorded while following must match
 *     what its peer recorded while leading, per thread.
 *
 * The leader relays each event before writing its own record, so a record on disk implies the peer
 *     holds the event, and the pipe preserves order: any divergence or reordering is a relay bug.
 */
void
verify_relay_prefix(const TEST_CONFIG *cfg)
{
    uint32_t checked = 0;
    for (uint32_t n = 0; n < MAX_NODES; n++)
        for (uint32_t t = 0; t < cfg->thread_count; t++) {
            char follower_fname[128], leader_fname[128];
            testutil_snprintf(follower_fname, sizeof(follower_fname), FOLLOWER_RECORDS_FILE, n, t);
            testutil_snprintf(leader_fname, sizeof(leader_fname), LEADER_RECORDS_FILE, 1 - n, t);

            /* No relay file means no events were relayed for this thread; nothing to check. */
            if (!testutil_exists(NULL, follower_fname))
                continue;
            testutil_assertfmt(testutil_exists(NULL, leader_fname),
              "%s exists but the peer's %s does not", follower_fname, leader_fname);

            verify_relay_pair(follower_fname, leader_fname);
            ++checked;
        }

    println("Relay verify: %" PRIu32
            " follower record files are prefixes of the peer's leader records",
      checked);
}
