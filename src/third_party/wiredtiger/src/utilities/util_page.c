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
 *     Display a usage message for the page command.
 */
static int
usage(void)
{
    static const char *options[] = {"-p page_id",
      "required: numeric page id (decimal or 0x-prefixed hex)", "-l lsn",
      "required: numeric LSN (decimal or 0x-prefixed hex)", "-t table_id",
      "numeric table id to read directly off the page server without opening the table (use when "
      "the checkpoint is unreadable)",
      "-k",
      "display only the keys in the application data when dumping the page (not supported "
      "with -t)",
      "-u", "unredact all application data when dumping the page (not supported with -k)", "-?",
      "show this message", NULL, NULL};

    util_usage("page [-k | -u] -p page_id -l lsn [-t table_id] [uri]", "options:", options);
    return (1);
}

/*
 * util_page --
 *     The page command: read a single page through WT_PAGE_LOG.
 */
int
util_page(WT_SESSION *session, int argc, char *argv[])
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session_impl;
    uint64_t lsn, page_id, table_id;
    int ch;
    char *uri;
    bool dump_all_data, dump_key_data, have_lsn, have_page_id, have_table_id;

    session_impl = (WT_SESSION_IMPL *)session;
    dump_all_data = false;
    dump_key_data = false;
    have_lsn = false;
    have_page_id = false;
    have_table_id = false;
    lsn = 0;
    page_id = 0;
    table_id = 0;
    uri = NULL;

    while ((ch = __wt_getopt(progname, argc, argv, "kl:p:t:u?")) != EOF)
        switch (ch) {
        case 'k':
            dump_key_data = true;
            break;
        case 'l':
            if (util_str2num(session, __wt_optarg, true, &lsn) != 0)
                return (usage());
            have_lsn = true;
            break;
        case 'p':
            if (util_str2num(session, __wt_optarg, true, &page_id) != 0)
                return (usage());
            have_page_id = true;
            break;
        case 't':
            if (util_str2num(session, __wt_optarg, true, &table_id) != 0)
                return (usage());
            have_table_id = true;
            break;
        case 'u':
            dump_all_data = true;
            break;
        case '?':
            usage();
            return (0);
        default:
            return (usage());
        }
    argc -= __wt_optind;
    argv += __wt_optind;

    if (!have_page_id) {
        fprintf(stderr, "%s: page: -p page_id is required\n", progname);
        return (usage());
    }
    if (!have_lsn) {
        fprintf(stderr, "%s: page: -l lsn is required\n", progname);
        return (usage());
    }
    if (dump_all_data && dump_key_data) {
        fprintf(stderr, "%s: page: -u and -k are mutually exclusive\n", progname);
        return (usage());
    }
    if (dump_key_data && have_table_id) {
        fprintf(stderr,
          "%s: page: -k is not supported with -t: a raw table id read has no decoded page "
          "format to redact selectively\n",
          progname);
        return (usage());
    }

    /*
     * An explicit table id reads directly off the connection page log without opening the table,
     * which works even when the checkpoint cannot be picked up. Otherwise resolve the URI to a
     * dhandle and read through it.
     */
#ifdef HAVE_DIAGNOSTIC
    if (have_table_id) {
        ret = __wt_debug_disagg_page_id_raw(session_impl, table_id, page_id, lsn, dump_all_data);
    } else {
        if (argc != 1)
            return (usage());
        if ((uri = util_uri(session, *argv, "file")) == NULL)
            return (1);
        if ((ret = __wt_session_get_dhandle(session_impl, uri, NULL, NULL, 0)) == 0) {
            ret = __wt_debug_disagg_page_id(
              session_impl, page_id, lsn, NULL, dump_all_data, dump_key_data);
            WT_TRET(__wt_session_release_dhandle(session_impl));
        }
    }
#else
    WT_UNUSED(have_table_id);
    WT_UNUSED(session_impl);
    WT_UNUSED(dump_all_data);
    WT_UNUSED(dump_key_data);
    fprintf(stderr,
      "%s: page: this subcommand requires a diagnostic build "
      "(rebuild WiredTiger with -DHAVE_DIAGNOSTIC=1)\n",
      progname);
    ret = ENOTSUP;
#endif

    util_free(uri);
    if (ret != 0)
        (void)util_err(session, ret, "page");
    return (ret == 0 ? 0 : 1);
}
