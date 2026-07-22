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

#include "format.h"

/* Ascending pushed key timestamps, with key bytes KEY_PREFIX + timestamp. */
#define KEY_PREFIX "abcdefghijklmnopqrstuvwxyz"

/*
 * disagg_key_history_clear --
 *     Clear the recorded push history on step-down.
 */
void
disagg_key_history_clear(void)
{
    memset(g.key_push_history, 0, sizeof(g.key_push_history));
    g.key_push_count = 0;
}

/*
 * key_push_history_append --
 *     Record a successfully pushed timestamp.
 */
static void
key_push_history_append(wt_timestamp_t ts)
{
    testutil_assertfmt(g.key_push_count < KEY_PUSH_HISTORY_MAX,
      "key rotation history full at %d entries, cannot push %" PRIu64, KEY_PUSH_HISTORY_MAX,
      (uint64_t)ts);
    g.key_push_history[g.key_push_count++] = ts;
}

/*
 * disagg_key_push --
 *     Push a new key one timestamp above the given floor, recording it on success. Returns EINVAL
 *     without recording when stable advanced past the chosen timestamp, leaving the caller to
 *     retry.
 */
static int
disagg_key_push(
  WT_SESSION *session, WT_KEY_PROVIDER *kp, wt_timestamp_t floor_ts, wt_timestamp_t *push_tsp)
{
    WT_DECL_RET;

    wt_timestamp_t push_ts = floor_ts + 1;

    WT_CRYPT_KEYS crypt;
    WT_CLEAR(crypt);
    char key_buf[64];
    testutil_snprintf(key_buf, sizeof(key_buf), "%s%" PRIu64, KEY_PREFIX, (uint64_t)push_ts);
    crypt.keys.data = key_buf;
    crypt.keys.size = strlen(key_buf);
    crypt.timestamp = push_ts;

    if ((ret = kp->set_key(kp, session, &crypt)) == EINVAL)
        return (EINVAL);
    testutil_check(ret);

    key_push_history_append(push_ts);
    *push_tsp = push_ts;
    return (0);
}

/*
 * disagg_key_push_initial --
 *     Push an initial key so the first checkpoint has a persisted KEK page. Called at create time
 *     for the leader and before step-up for the follower.
 */
void
disagg_key_push_initial(WT_CONNECTION *conn, bool advance_stable)
{
    if (GV(DISAGG_KEY_PROVIDER) != DISAGG_KEY_PROVIDER_PUSH)
        return;

    WT_SESSION *session;
    WT_KEY_PROVIDER *kp;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(conn->get_key_provider(conn, &kp));

    wt_timestamp_t push_ts;
    testutil_check(disagg_key_push(session, kp, g.stable_timestamp, &push_ts));

    /*
     * The leader seeds before its create-time close checkpoint, which must drain the key now, so it
     * advances stable.
     */
    if (advance_stable) {
        char ts_buf[WT_TS_HEX_STRING_SIZE + 24];
        testutil_snprintf(ts_buf, sizeof(ts_buf), "stable_timestamp=%" PRIx64, (uint64_t)push_ts);
        testutil_check(conn->set_timestamp(conn, ts_buf));
        g.stable_timestamp = push_ts;
    }

    testutil_check(session->close(session, NULL));
}

/*
 * expected_kek_ts --
 *     The expected persisted timestamp for a checkpoint, the latest pushed timestamp at or below
 *     the checkpoint timestamp, or WT_TS_NONE if none was pushed.
 */
static wt_timestamp_t
expected_kek_ts(wt_timestamp_t checkpoint_ts)
{
    wt_timestamp_t expected_ts = WT_TS_NONE;

    for (size_t i = 0; i < g.key_push_count; ++i) {
        if (g.key_push_history[i] > checkpoint_ts)
            break;
        expected_ts = g.key_push_history[i];
    }
    return (expected_ts);
}

/*
 * disagg_validate_kek_page --
 *     Assert a persisted KEK page's timestamp and key bytes match the expected key.
 */
static void
disagg_validate_kek_page(
  const WT_ITEM *page, wt_timestamp_t expected_ts, wt_timestamp_t checkpoint_ts)
{
    WT_CRYPT_HEADER hdr;

    /* Byte-swap the leading crypt header before reading its fields. */
    testutil_assert(page->size >= sizeof(WT_CRYPT_HEADER));
    memcpy(&hdr, page->data, sizeof(hdr));
    __wt_crypt_header_byteswap(&hdr);
    testutil_assert(hdr.signature == WT_CRYPT_HEADER_SIGNATURE);

    wt_timestamp_t persisted_ts = hdr.timestamp;
    testutil_assert(hdr.header_size + hdr.crypt_size <= page->size);
    const uint8_t *key_data = (const uint8_t *)page->data + hdr.header_size;
    size_t key_size = hdr.crypt_size;

    testutil_assertfmt(persisted_ts == expected_ts,
      "persisted KEK timestamp %" PRIu64 " but expected %" PRIu64
      " for checkpoint timestamp %" PRIu64,
      persisted_ts, (uint64_t)expected_ts, (uint64_t)checkpoint_ts);

    char buf[64];
    testutil_snprintf(buf, sizeof(buf), "%s%" PRIu64, KEY_PREFIX, (uint64_t)expected_ts);
    testutil_assertfmt(key_size == strlen(buf) && memcmp(key_data, buf, key_size) == 0,
      "persisted KEK bytes do not match the expected key for timestamp %" PRIu64,
      (uint64_t)expected_ts);
}

/*
 * disagg_read_kek_page --
 *     Read the KEK page referenced by a checkpoint's metadata.
 */
static void
disagg_read_kek_page(
  WT_SESSION *session, WT_PAGE_LOG *page_log, const WT_DISAGG_METADATA *metadata, WT_ITEM *page)
{
    WT_CLEAR(*page);

    /* Parse the KEK page LSN from the key_provider config. */
    char *kp_str = NULL;
    WT_CONFIG_ITEM page_cval, lsn_cval;
    testutil_check(__wt_strndup(
      (WT_SESSION_IMPL *)session, metadata->key_provider, metadata->key_provider_len, &kp_str));
    testutil_check(__wt_config_getones((WT_SESSION_IMPL *)session, kp_str, "page.1", &page_cval));
    testutil_check(__wt_config_subgets((WT_SESSION_IMPL *)session, &page_cval, "lsn", &lsn_cval));
    uint64_t key_provider_lsn = (uint64_t)lsn_cval.val;

    WT_PAGE_LOG_HANDLE *plh;
    WT_PAGE_LOG_GET_ARGS get_args;
    testutil_check(
      page_log->pl_open_handle(page_log, session, WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID, &plh));
    WT_CLEAR(get_args);
    get_args.lsn = key_provider_lsn;

    for (u_int retry = 0;; ++retry) {
        uint32_t count = 1;
        testutil_check(plh->plh_get(
          plh, session, WT_DISAGG_KEY_PROVIDER_MAIN_PAGE_ID, 0, &get_args, page, &count));
        if (count == 1)
            break;
        testutil_assert(retry < 100);
        __wt_sleep(0, 10000);
    }

    testutil_check(plh->plh_close(plh, session));
    __wt_free((WT_SESSION_IMPL *)session, kp_str);
}

/*
 * disagg_key_validate_persisted --
 *     Read the KEK page the given checkpoint selected and assert it matches the push history.
 */
static void
disagg_key_validate_persisted(WT_SESSION *session, WT_PAGE_LOG *page_log,
  const WT_DISAGG_METADATA *metadata, wt_timestamp_t checkpoint_ts)
{
    WT_ITEM page;
    wt_timestamp_t expected_ts = expected_kek_ts(checkpoint_ts);
    if (expected_ts == WT_TS_NONE)
        return;

    disagg_read_kek_page(session, page_log, metadata, &page);
    disagg_validate_kek_page(&page, expected_ts, checkpoint_ts);
    free(page.mem);
}

/*
 * disagg_key_validate_after_checkpoint --
 *     After a leader checkpoint, read the latest complete checkpoint's persisted KEK page and
 *     verify it is the latest key pushed at or below the checkpoint timestamp.
 */
void
disagg_key_validate_after_checkpoint(WT_SESSION *session)
{
    WT_DECL_RET;

    /* Only push mode persists rotated keys, and the read-back is PALite-specific. */
    if (GV(DISAGG_KEY_PROVIDER) != DISAGG_KEY_PROVIDER_PUSH ||
      strcmp(GVS(DISAGG_PAGE_LOG), "palite") != 0)
        return;

    WT_CONNECTION *conn = session->connection;
    WT_PAGE_LOG *page_log;
    testutil_check(conn->get_page_log(conn, GVS(DISAGG_PAGE_LOG), &page_log));

    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS args;
    memset(&args, 0, sizeof(args));
    ret = page_log->pl_get_complete_checkpoint(page_log, session, &args);
    testutil_check_error_ok(ret, WT_NOTFOUND);
    if (ret != WT_NOTFOUND) {
        WT_ITEM full_metadata;
        testutil_check(follower_fetch_full_metadata(
          session, page_log, &args.checkpoint_metadata, &full_metadata));
        WT_DISAGG_METADATA metadata;
        testutil_check(
          __wt_disagg_parse_meta((WT_SESSION_IMPL *)session, &full_metadata, &metadata));
        disagg_key_validate_persisted(session, page_log, &metadata, args.checkpoint_timestamp);
        free(full_metadata.mem);
    }

    free(args.checkpoint_metadata.mem);
    testutil_check(page_log->terminate(page_log, NULL));
}

/*
 * disagg_key_rotation --
 *     On the leader, periodically push a new key at a timestamp above both stable and the previous
 *     push so the next checkpoint can persist it.
 */
WT_THREAD_RET
disagg_key_rotation(void *arg)
{
    wt_timestamp_t last_push_ts = 0;
    u_int counter = 0;

    (void)arg;

    WT_KEY_PROVIDER *kp;
    testutil_check(g.wts_conn->get_key_provider(g.wts_conn, &kp));
    SAP sap;
    memset(&sap, 0, sizeof(sap));
    WT_SESSION *session;
    wt_wrap_open_session(g.wts_conn, &sap, NULL, NULL, &session);

    for (u_int secs = mmrand(&g.extra_rnd, 1, 5); !g.workers_finished;) {
        if (secs > 0) {
            __wt_sleep(1, 0);
            --secs;
            continue;
        }
        secs = mmrand(&g.extra_rnd, 1, 5);

        wt_timestamp_t stable_ts = g.stable_timestamp;

        /* Stable can pass push_ts between the read and the call, so a benign EINVAL just retries.
         */
        wt_timestamp_t push_ts;
        if (disagg_key_push(session, kp, WT_MAX(stable_ts, last_push_ts), &push_ts) == EINVAL)
            continue;
        last_push_ts = push_ts;
        trace_msg(session, "key rotation #%u pushed timestamp %" PRIu64, ++counter, push_ts);

        /* Validate pushed key list against latest checkpoint. */
        if (g.disagg_leader)
            disagg_key_validate_after_checkpoint(session);
    }

    wt_wrap_close_session(session);

    return (WT_THREAD_RET_VALUE);
}
