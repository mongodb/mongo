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
 *     Display a usage message for the stat command.
 */
static int
usage(void)
{
    static const char *options[] = {"-f", "include only \"fast\" statistics in the output", "-?",
      "show this message", NULL, NULL};

    util_usage("stat [-f] [uri]", "options:", options);
    return (1);
}

/*
 * util_stat --
 *     The stat command.
 */
int
util_stat(WT_SESSION *session, int argc, char *argv[])
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    size_t urilen;
    int ch;
    char *objname, *uri;
    const char *config, *desc, *pval;
    bool objname_free;

    objname_free = false;
    objname = uri = NULL;
    config = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "af?")) != EOF)
        switch (ch) {
        case 'a':
            /*
             * Historically, the -a option meant include all of the statistics; because we are
             * opening the database with statistics=(all), that is now the default, allow the option
             * for compatibility.
             */
            config = NULL;
            break;
        case 'f':
            config = "statistics=(fast)";
            break;
        case '?':
            usage();
            return (0);
        default:
            return (usage());
        }
    argc -= __wt_optind;
    argv += __wt_optind;

    /*
     * If there are no arguments, the statistics cursor operates on the connection, otherwise, the
     * optional remaining argument is a file or LSM name.
     */
    switch (argc) {
    case 0:
        objname = (char *)"";
        break;
    case 1:
        if ((objname = util_uri(session, *argv, "table")) == NULL)
            return (1);
        objname_free = true;
        break;
    default:
        return (usage());
    }

    urilen = strlen("statistics:") + strlen(objname) + 1;
    if ((uri = calloc(urilen, 1)) == NULL) {
        fprintf(stderr, "%s: %s\n", progname, strerror(errno));
        goto err;
    }
    if ((ret = __wt_snprintf(uri, urilen, "statistics:%s", objname)) != 0) {
        fprintf(stderr, "%s: %s\n", progname, strerror(ret));
        goto err;
    }

    if ((ret = session->open_cursor(session, uri, NULL, config, &cursor)) != 0) {
        fprintf(stderr, "%s: cursor open(%s) failed: %s\n", progname, uri,
          session->strerror(session, ret));
        goto err;
    }

    /* List the statistics. */
    while ((ret = cursor->next(cursor)) == 0 &&
      (ret = cursor->get_value(cursor, &desc, &pval, NULL)) == 0)
        if (printf("%s=%s\n", desc, pval) < 0) {
            (void)util_err(session, errno, "printf");
            goto err;
        }
    if (ret == WT_NOTFOUND)
        ret = 0;

    if (ret != 0) {
        fprintf(stderr, "%s: cursor get(%s) failed: %s\n", progname, objname,
          session->strerror(session, ret));
        goto err;
    }

    if (0) {
err:
        ret = 1;
    }
    if (objname_free)
        free(objname);
    free(uri);

    return (ret);
}
