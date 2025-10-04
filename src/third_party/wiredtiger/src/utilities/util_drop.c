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
 *     Display a usage message for the drop command.
 */
static int
usage(void)
{
    static const char *options[] = {"-?", "show this message", NULL, NULL};

    util_usage("drop uri", "options:", options);
    return (1);
}

/*
 * util_drop --
 *     The drop command.
 */
int
util_drop(WT_SESSION *session, int argc, char *argv[])
{
    WT_DECL_RET;
    int ch;
    char *uri;

    uri = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "?")) != EOF)
        switch (ch) {
        case '?':
            usage();
            return (0);
        default:
            return (usage());
        }

    argc -= __wt_optind;
    argv += __wt_optind;

    /* The remaining argument is the uri. */
    if (argc != 1)
        return (usage());
    if ((uri = util_uri(session, *argv, "table")) == NULL)
        return (1);

    if ((ret = session->drop(session, uri, "force")) != 0)
        (void)util_err(session, ret, "session.drop: %s", uri);

    util_free(uri);
    return (ret);
}
