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
 *     Display a usage message for the downgrade command.
 */
static int
usage(void)
{
    static const char *options[] = {"-V",
      "a required option, the version to which the database is downgraded", "-?",
      "show this message", NULL, NULL};

    util_usage("downgrade -V release", "options:", options);
    return (1);
}

/*
 * util_downgrade --
 *     The downgrade command.
 */
int
util_downgrade(WT_SESSION *session, int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    int ch;
    char config_str[128], *release;

    release = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "V:?")) != EOF)
        switch (ch) {
        case 'V':
            release = __wt_optarg;
            break;
        case '?':
            usage();
            return (0);
        default:
            return (usage());
        }
    argc -= __wt_optind;

    /*
     * The release argument is required. There should not be any more arguments.
     */
    if (argc != 0 || release == NULL)
        return (usage());

    if ((ret = __wt_snprintf(
           config_str, sizeof(config_str), "compatibility=(release=%s)", release)) != 0)
        return (util_err(session, ret, NULL));
    conn = session->connection;
    if ((ret = conn->reconfigure(conn, config_str)) != 0)
        return (util_err(session, ret, "WT_CONNECTION.downgrade"));

    return (0);
}
