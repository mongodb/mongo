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
    static const char *options[] = {"-d config",
      "display underlying information during verification", "-s",
      "verify against the specified timestamp", "-t", "do not clear txn ids during verification",
      NULL, NULL};

    util_usage(
      "verify [-s] [-t] [-d dump_address | dump_blocks | dump_layout | dump_offsets=#,# | "
      "dump_pages] "
      "[uri]",
      "options:", options);

    return (1);
}

/*
 * util_verify --
 *     The verify command.
 */
int
util_verify(WT_SESSION *session, int argc, char *argv[])
{
    WT_DECL_RET;
    size_t size;
    int ch;
    char *config, *dump_offsets, *uri;
    bool do_not_clear_txn_id, dump_address, dump_blocks, dump_layout, dump_pages, stable_timestamp;

    do_not_clear_txn_id = dump_address = dump_blocks = dump_layout = dump_pages = stable_timestamp =
      false;
    config = dump_offsets = uri = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "d:st")) != EOF)
        switch (ch) {
        case 'd':
            if (strcmp(__wt_optarg, "dump_address") == 0)
                dump_address = true;
            else if (strcmp(__wt_optarg, "dump_blocks") == 0)
                dump_blocks = true;
            else if (strcmp(__wt_optarg, "dump_layout") == 0)
                dump_layout = true;
            else if (WT_PREFIX_MATCH(__wt_optarg, "dump_offsets=")) {
                if (dump_offsets != NULL) {
                    fprintf(
                      stderr, "%s: only a single 'dump_offsets' argument supported\n", progname);
                    return (usage());
                }
                dump_offsets = __wt_optarg + strlen("dump_offsets=");
            } else if (strcmp(__wt_optarg, "dump_pages") == 0)
                dump_pages = true;
            else
                return (usage());
            break;
        case 's':
            stable_timestamp = true;
            break;
        case 't':
            do_not_clear_txn_id = true;
            break;
        case '?':
        default:
            return (usage());
        }
    argc -= __wt_optind;
    argv += __wt_optind;

    /*
     * The remaining argument is the table name. If we are verifying the history store we do not
     * accept a URI. Otherwise, we need a URI to operate on.
     */
    if (argc != 1)
        return (usage());
    if ((uri = util_uri(session, *argv, "table")) == NULL)
        return (1);

    if (do_not_clear_txn_id || dump_address || dump_blocks || dump_layout || dump_offsets != NULL ||
      dump_pages || stable_timestamp) {
        size = strlen("do_not_clear_txn_id,") + strlen("dump_address,") + strlen("dump_blocks,") +
          strlen("dump_layout,") + strlen("dump_pages,") + strlen("dump_offsets[],") +
          (dump_offsets == NULL ? 0 : strlen(dump_offsets)) + strlen("history_store") +
          strlen("stable_timestamp,") + 20;
        if ((config = malloc(size)) == NULL) {
            ret = util_err(session, errno, NULL);
            goto err;
        }
        if ((ret = __wt_snprintf(config, size, "%s%s%s%s%s%s%s%s%s",
               do_not_clear_txn_id ? "do_not_clear_txn_id," : "",
               dump_address ? "dump_address," : "", dump_blocks ? "dump_blocks," : "",
               dump_layout ? "dump_layout," : "", dump_offsets != NULL ? "dump_offsets=[" : "",
               dump_offsets != NULL ? dump_offsets : "", dump_offsets != NULL ? "]," : "",
               dump_pages ? "dump_pages," : "", stable_timestamp ? "stable_timestamp," : "")) !=
          0) {
            (void)util_err(session, ret, NULL);
            goto err;
        }
    }
    if ((ret = session->verify(session, uri, config)) != 0)
        (void)util_err(session, ret, "session.verify: %s", uri);
    else {
        /*
         * Verbose configures a progress counter, move to the next line.
         */
        if (verbose)
            printf("\n");
    }

err:
    free(config);
    free(uri);
    return (ret);
}
