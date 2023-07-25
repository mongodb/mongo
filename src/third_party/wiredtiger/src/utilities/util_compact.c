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
 *     Display a usage message for the compact command.
 */
static int
usage(void)
{
    static const char *options[] = {"-c config",
      "a configuration string to be passed to WT_SESSION.compact", "-?", "show this message", NULL,
      NULL};

    util_usage("compact [-c configuration] uri", "options:", options);
    return (1);
}

/*
 * util_compact --
 *     The compact command.
 */
int
util_compact(WT_SESSION *session, int argc, char *argv[])
{
    WT_DECL_RET;
    int ch;
    char *config, *uri;

    config = uri = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "c:?")) != EOF)
        switch (ch) {
        case 'c': /* command-line configuration */
            config = __wt_optarg;
            break;
        case '?':
            usage();
            return (0);
        default:
            return (usage());
        }
    argc -= __wt_optind;
    argv += __wt_optind;

    /* The remaining argument is the table name. */
    if (argc != 1)
        return (usage());
    if ((uri = util_uri(session, *argv, "table")) == NULL)
        return (1);

    if ((ret = session->compact(session, uri, config)) != 0)
        (void)util_err(session, ret, "session.compact: %s", uri);

    util_free(uri);
    return (ret);
}
