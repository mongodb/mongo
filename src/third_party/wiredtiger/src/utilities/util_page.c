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
      "required: numeric LSN (decimal or 0x-prefixed hex)", "-?", "show this message", NULL, NULL};

    util_usage("page -p page_id -l lsn uri", "options:", options);
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
    uint64_t lsn, page_id;
    int ch;
    char *uri;
    bool have_lsn, have_page_id;

    session_impl = (WT_SESSION_IMPL *)session;
    have_lsn = false;
    have_page_id = false;
    lsn = 0;
    page_id = 0;
    uri = NULL;

    while ((ch = __wt_getopt(progname, argc, argv, "l:p:?")) != EOF)
        switch (ch) {
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
    if (argc != 1)
        return (usage());
    if ((uri = util_uri(session, *argv, "file")) == NULL)
        return (1);

    WT_ERR(__wt_session_get_dhandle(session_impl, uri, NULL, NULL, 0));
#ifdef HAVE_DIAGNOSTIC
    ret = __wt_debug_disagg_page_id(session_impl, page_id, lsn, NULL);
#else
    fprintf(stderr,
      "%s: page: this subcommand requires a diagnostic build "
      "(rebuild WiredTiger with -DHAVE_DIAGNOSTIC=1)\n",
      progname);
    ret = ENOTSUP;
#endif
    WT_TRET(__wt_session_release_dhandle(session_impl));

err:
    util_free(uri);
    if (ret != 0)
        (void)util_err(session, ret, "page");
    return (ret == 0 ? 0 : 1);
}
