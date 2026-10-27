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
 *     Display a usage message for the verify command.
 */
static int
usage(void)
{
    static const char *options[] = {"-a", "abort on error during verification of all tables", "-c",
      "continue to the next page after encountering error during verification", "-d config",
      "display underlying information during verification", "-s",
      "verify against the specified timestamp", "-t", "do not clear txn ids during verification",
      "-k",
      "display only the keys in the application data with configuration dump_blocks or dump_pages",
      "-u",
      "display all the application data when dumping with configuration dump_blocks or dump_pages",
      "-?", "show this message", NULL, NULL};

    util_usage(
      "verify [-ackSstu] [-d dump_address | dump_blocks | dump_layout | dump_tree_shape | "
      "dump_offsets=#,# "
      "| dump_pages] [uri]",
      "options:", options);

    return (1);
}

#define WT_USE_ERR()   \
    do {               \
        ret = usage(); \
        goto err;      \
    } while (0)

/*
 * verify_one --
 *     Verify the file specified by the URI.
 */
static int
verify_one(WT_SESSION *session, char *config, char *uri)
{
    WT_DECL_RET;

    if ((ret = session->verify(session, uri, config)) != 0)
        ret = util_err(session, ret, "session.verify: %s", uri);
    else if (verbose) {
        /* Verbose configures a progress counter, move to the next line. */
        printf("\n");
    }
    return (ret);
}

/*
 * util_verify --
 *     The verify command.
 */
int
util_verify(WT_SESSION *session, int argc, char *argv[])
{
    WT_DECL_ITEM(config);
    WT_DECL_RET;
    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    int ch;
    char *dump_offsets, *uri;
    bool dump_all_data, dump_key_data;

    dump_all_data = dump_key_data = false;
    dump_offsets = uri = NULL;

    WT_RET(__wt_scr_alloc(session_impl, 0, &config));

    while ((ch = __wt_getopt(progname, argc, argv, "acd:kSstu?")) != EOF)
        switch (ch) {
        case 'a':
            WT_ERR(__wt_buf_catfmt(session_impl, config, "abort_on_error,"));
            break;
        case 'c':
            WT_ERR(__wt_buf_catfmt(session_impl, config, "read_corrupt,"));
            break;
        case 'd':
            if (strcmp(__wt_optarg, "dump_address") == 0)
                WT_ERR(__wt_buf_catfmt(session_impl, config, "dump_address,"));
            else if (strcmp(__wt_optarg, "dump_blocks") == 0)
                WT_ERR(__wt_buf_catfmt(session_impl, config, "dump_blocks,"));
            else if (strcmp(__wt_optarg, "dump_layout") == 0)
                WT_ERR(__wt_buf_catfmt(session_impl, config, "dump_layout,"));
            else if (strcmp(__wt_optarg, "dump_tree_shape") == 0)
                WT_ERR(__wt_buf_catfmt(session_impl, config, "dump_tree_shape,"));
            else if (WT_PREFIX_MATCH(__wt_optarg, "dump_offsets=")) {
                if (dump_offsets != NULL) {
                    fprintf(
                      stderr, "%s: only a single 'dump_offsets' argument supported\n", progname);
                    WT_USE_ERR();
                }
                dump_offsets = __wt_optarg + strlen("dump_offsets=");
                WT_ERR(__wt_buf_catfmt(session_impl, config, "dump_offsets=[%s],", dump_offsets));
            } else if (strcmp(__wt_optarg, "dump_pages") == 0)
                WT_ERR(__wt_buf_catfmt(session_impl, config, "dump_pages,"));
            else
                WT_USE_ERR();
            break;
        case 'k':
            dump_key_data = true;
            WT_ERR(__wt_buf_catfmt(session_impl, config, "dump_key_data,"));
            break;
        case 'S':
            WT_ERR(__wt_buf_catfmt(session_impl, config, "strict,"));
            break;
        case 's':
            WT_ERR(__wt_buf_catfmt(session_impl, config, "stable_timestamp,"));
            break;
        case 't':
            WT_ERR(__wt_buf_catfmt(session_impl, config, "do_not_clear_txn_id,"));
            break;
        case 'u':
            dump_all_data = true;
            WT_ERR(__wt_buf_catfmt(session_impl, config, "dump_all_data,"));
            break;
        case '?':
            usage();
            return (0);
        default:
            WT_USE_ERR();
        }

    if (dump_all_data && dump_key_data) {
        fprintf(stderr, "%s: -u and -k are mutually exclusive\n", progname);
        WT_USE_ERR();
    }

    argc -= __wt_optind;
    argv += __wt_optind;

    /* Verify all the tables if no particular URI is specified. */
    if (argc < 1) {
        /* A NULL URI asks the library to verify every table in the database. */
        if ((ret = session->verify(session, NULL, (char *)config->data)) != 0)
            ret = util_err(session, ret, "session.verify: all tables");
        else if (verbose)
            /* Verbose configures a progress counter, move to the next line. */
            printf("\n");
    } else {
        if ((uri = util_uri(session, *argv, "table")) == NULL)
            goto err;

        ret = verify_one(session, (char *)config->data, uri);
    }

err:
    __wt_scr_free(session_impl, &config);
    util_free(uri);
    return (ret);
}
