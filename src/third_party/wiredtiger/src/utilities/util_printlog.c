/*-
 * Copyright (c) 2014-2019 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int usage(void);

int
util_printlog(WT_SESSION *session, int argc, char *argv[])
{
    WT_DECL_RET;
    uint32_t flags;
    int ch;
    char *ofile;

    flags = 0;
    ofile = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "f:x")) != EOF)
        switch (ch) {
        case 'f': /* output file */
            ofile = __wt_optarg;
            break;
        case 'x': /* hex output */
            LF_SET(WT_TXN_PRINTLOG_HEX);
            break;
        case '?':
        default:
            return (usage());
        }
    argc -= __wt_optind;

    /* There should not be any more arguments. */
    if (argc != 0)
        return (usage());

    if ((ret = __wt_txn_printlog(session, ofile, flags)) != 0)
        (void)util_err(session, ret, "printlog");

    return (ret);
}

static int
usage(void)
{
    (void)fprintf(stderr,
      "usage: %s %s "
      "printlog [-x] [-f output-file]\n",
      progname, usage_prefix);
    return (1);
}
