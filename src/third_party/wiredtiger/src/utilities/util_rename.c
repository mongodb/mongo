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
 *     Display a usage message for the rename command.
 */
static int
usage(void)
{
    util_usage("rename uri newuri", NULL, NULL);
    return (1);
}

/*
 * util_rename --
 *     The rename command.
 */
int
util_rename(WT_SESSION *session, int argc, char *argv[])
{
    WT_DECL_RET;
    int ch;
    char *uri, *newuri;

    uri = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "")) != EOF)
        switch (ch) {
        case '?':
        default:
            return (usage());
        }
    argc -= __wt_optind;
    argv += __wt_optind;

    /* The remaining arguments are the object uri and new name. */
    if (argc != 2)
        return (usage());
    if ((uri = util_uri(session, *argv, "table")) == NULL)
        return (1);
    newuri = argv[1];

    if ((ret = session->rename(session, uri, newuri, NULL)) != 0)
        (void)util_err(session, ret, "session.rename: %s, %s", uri, newuri);

    free(uri);
    return (ret);
}
