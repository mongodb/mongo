/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

/*
 * usage --
 *     Display a usage message for the turtle command.
 */
static int
usage(void)
{
    static const char *options[] = {"-l lsn",
      "dump the turtle page at this LSN instead of the at latest checkpoint metadata LSN", "-?",
      "show this message", NULL, NULL};

    util_usage("turtle [-l lsn]", "options:", options);
    return (1);
}

/*
 * parse_checkpoint_meta --
 *     Lenient parse of the disagg checkpoint metadata blob returned by pl_get_complete_checkpoint.
 */
static int
parse_checkpoint_meta(
  WT_SESSION_IMPL *session, const char *buf, size_t buf_len, WT_DISAGG_CHECKPOINT_META *metap)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    uint64_t hex_val;
    char *meta_str;

    meta_str = NULL;
    WT_CLEAR(*metap);

    WT_ERR(__wt_strndup(session, buf, buf_len, &meta_str));

    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, meta_str, "metadata_lsn", &cval), true);
    if (ret == 0 && cval.len != 0)
        metap->metadata_lsn = (uint64_t)cval.val;

    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, meta_str, "metadata_checksum", &cval), true);
    if (ret == 0 && cval.len != 0) {
        WT_ERR(__wt_conf_parse_hex(session, "metadata_checksum", &hex_val, &cval));
        if (hex_val > UINT32_MAX)
            WT_ERR_MSG(session, EINVAL, "metadata_checksum out of range: %" PRIx64, hex_val);
        metap->metadata_checksum = (uint32_t)hex_val;
        metap->has_metadata_checksum = true;
    }

    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, meta_str, "database_size", &cval), true);
    if (ret == 0 && cval.len != 0)
        metap->database_size = (uint64_t)cval.val;

    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, meta_str, "version", &cval), true);
    if (ret == 0 && cval.len != 0) {
        if (cval.val < 0 || (uint64_t)cval.val > UINT32_MAX)
            WT_ERR_MSG(session, EINVAL, "version out of range: %" PRId64, cval.val);
        metap->version = (uint32_t)cval.val;
    }

    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, meta_str, "compatible_version", &cval), true);
    if (ret == 0 && cval.len != 0) {
        if (cval.val < 0 || (uint64_t)cval.val > UINT32_MAX)
            WT_ERR_MSG(session, EINVAL, "compatible_version out of range: %" PRId64, cval.val);
        metap->compatible_version = (uint32_t)cval.val;
    }

    ret = 0;

err:
    __wt_free(session, meta_str);
    return (ret);
}

/*
 * print_blob --
 *     Write the bytes of an item to stdout, appending a newline if the last byte is not one.
 */
static void
print_blob(const WT_ITEM *item)
{
    const char *bytes;

    bytes = (const char *)item->data;
    fwrite(bytes, 1, item->size, stdout);
    if (item->size == 0 || bytes[item->size - 1] != '\n')
        fputc('\n', stdout);
}

/*
 * fetch_latest_checkpoint_meta --
 *     Ask the connection's page log for the latest complete checkpoint metadata. Returns the
 *     checkpoint LSN and the raw metadata blob (which points at the turtle page); blob ownership is
 *     transferred to the caller.
 */
static int
fetch_latest_checkpoint_meta(WT_SESSION_IMPL *session, uint64_t *lsnp, WT_ITEM *meta)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_PAGE_LOG *page_log;
    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS args;

    WT_CLEAR(args);

    conn = S2C(session);

    page_log = conn->disaggregated_storage.npage_log->page_log;
    if (page_log->pl_get_complete_checkpoint == NULL)
        WT_RET_MSG(session, ENOTSUP, "page log does not implement pl_get_complete_checkpoint");

    ret = page_log->pl_get_complete_checkpoint(page_log, &session->iface, &args);
    if (ret == 0) {
        *lsnp = args.checkpoint_lsn;
        *meta = args.checkpoint_metadata;
        WT_CLEAR(args.checkpoint_metadata);
    } else {
        if (ret == WT_NOTFOUND)
            printf("no complete checkpoint\n");
        __wt_buf_free(session, &args.checkpoint_metadata);
    }

    return (ret);
}

/*
 * fetch_turtle_page --
 *     plh_get the turtle page (page_id=WT_DISAGG_METADATA_MAIN_PAGE_ID, the page that mimics the
 *     ASC turtle file and points at the root of the metadata file) at the supplied LSN via the page
 *     log handle the connection already opened.
 */
static int
fetch_turtle_page(WT_SESSION_IMPL *session, uint64_t lsn, WT_ITEM *item)
{
    WT_CONNECTION_IMPL *conn;
    WT_PAGE_LOG_GET_ARGS get_args;
    WT_PAGE_LOG_HANDLE *plh;
    uint32_t count;

    conn = S2C(session);
    plh = conn->disaggregated_storage.page_log_meta;

    WT_CLEAR(get_args);
    get_args.lsn = lsn;
    count = 1;
    WT_RET(plh->plh_get(
      plh, &session->iface, WT_DISAGG_METADATA_MAIN_PAGE_ID, 0, &get_args, item, &count));
    if (count == 0)
        return (WT_NOTFOUND);
    return (0);
}

/*
 * print_turtle_page --
 *     Print a turtle page banner and its bytes. If a checksum is available from the checkpoint
 *     metadata, verify and report rather than abort on mismatch. Returns true on checksum mismatch.
 */
static bool
print_turtle_page(
  uint64_t lsn, const WT_ITEM *page, bool have_expected_cksum, uint32_t expected_cksum)
{
    uint32_t actual;
    bool mismatch;

    mismatch = false;
    printf("\n=== turtle page (table_id=2, page_id=1, requested_lsn=%" PRIu64 ") ===\n", lsn);
    if (have_expected_cksum) {
        actual = __wt_checksum(page->data, page->size);
        if (actual != expected_cksum)
            mismatch = true;
        printf("checksum=0x%08" PRIx32 " (expected=0x%08" PRIx32 ")\n", actual, expected_cksum);
    }
    print_blob(page);
    return (mismatch);
}

/*
 * util_turtle --
 *     The turtle command. Requires a disaggregated-storage connection. With no flags, fetch the
 *     latest checkpoint metadata and dump its raw blob bytes, then chase its metadata_lsn to fetch
 *     and dump the turtle page at that LSN. With -l <lsn>, skip the checkpoint metadata and dump
 *     the turtle page at that LSN as-is.
 */
int
util_turtle(WT_SESSION *session, int argc, char *argv[])
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGG_CHECKPOINT_META ckpt_meta;
    WT_ITEM ckpt_meta_blob, turtle_page;
    WT_SESSION_IMPL *session_impl;
    uint64_t lsn, lsn_arg;
    uint32_t expected_cksum;
    int ch;
    bool have_lsn_arg, have_expected_cksum;

    session_impl = (WT_SESSION_IMPL *)session;
    conn = S2C(session_impl);
    WT_CLEAR(ckpt_meta);
    WT_CLEAR(ckpt_meta_blob);
    WT_CLEAR(turtle_page);
    lsn = 0;
    lsn_arg = 0;
    expected_cksum = 0;
    have_lsn_arg = false;
    have_expected_cksum = false;

    while ((ch = __wt_getopt(progname, argc, argv, "l:?")) != EOF)
        switch (ch) {
        case 'l':
            if (util_str2num(session, __wt_optarg, true, &lsn_arg) != 0)
                return (usage());
            if (lsn_arg == 0) {
                printf("lsn must be greater than 0\n");
                return (usage());
            }
            have_lsn_arg = true;
            break;
        case '?':
            usage();
            return (0);
        default:
            return (usage());
        }
    argc -= __wt_optind;

    if (argc != 0)
        return (usage());

    if (conn->disaggregated_storage.npage_log == NULL ||
      conn->disaggregated_storage.page_log_meta == NULL)
        WT_RET_MSG(session_impl, ENOTSUP, "wt turtle requires a disaggregated-storage connection");

    if (!have_lsn_arg) {
        WT_ERR(fetch_latest_checkpoint_meta(session_impl, &lsn, &ckpt_meta_blob));
        print_blob(&ckpt_meta_blob);
        WT_ERR(parse_checkpoint_meta(
          session_impl, ckpt_meta_blob.data, ckpt_meta_blob.size, &ckpt_meta));
        lsn_arg = ckpt_meta.metadata_lsn;
        have_expected_cksum = ckpt_meta.has_metadata_checksum;
        expected_cksum = ckpt_meta.metadata_checksum;
    }

    ret = fetch_turtle_page(session_impl, lsn_arg, &turtle_page);
    if (ret == WT_NOTFOUND)
        printf("turtle page not found at requested_lsn=%" PRIu64 "\n", lsn_arg);
    else if (ret == 0) {
        if (print_turtle_page(lsn_arg, &turtle_page, have_expected_cksum, expected_cksum))
            ret = 1;
    }

err:
    __wt_buf_free(session_impl, &ckpt_meta_blob);
    __wt_buf_free(session_impl, &turtle_page);
    if (ret != 0)
        (void)util_err(session, ret, "turtle");
    return (ret == 0 ? 0 : 1);
}
