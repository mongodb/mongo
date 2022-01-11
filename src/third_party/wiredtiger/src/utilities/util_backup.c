/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int copy(WT_SESSION *, const char *, const char *);

/*
 * usage --
 *     TODO: Add a comment describing this function.
 */
static int
usage(void)
{
    static const char *options[] = {"-t uri",
      "backup the named data sources (by default the entire database is backed up)", NULL, NULL};

    util_usage("backup [-t uri] directory", "options:", options);
    return (1);
}

/*
 * util_backup --
 *     TODO: Add a comment describing this function.
 */
int
util_backup(WT_SESSION *session, int argc, char *argv[])
{
    WT_CURSOR *cursor;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_SESSION_IMPL *session_impl;
    int ch;
    const char *directory, *name;
    bool target;

    session_impl = (WT_SESSION_IMPL *)session;

    target = false;
    while ((ch = __wt_getopt(progname, argc, argv, "t:")) != EOF)
        switch (ch) {
        case 't':
            if (!target) {
                WT_ERR(__wt_scr_alloc(session_impl, 0, &tmp));
                WT_ERR(__wt_buf_fmt(session_impl, tmp, "%s", "target=("));
            }
            WT_ERR(__wt_buf_catfmt(session_impl, tmp, "%s\"%s\"", target ? "," : "", __wt_optarg));
            target = true;
            break;
        case '?':
        default:
            WT_ERR(usage());
        }
    argc -= __wt_optind;
    argv += __wt_optind;

    if (argc != 1) {
        (void)usage();
        goto err;
    }
    directory = *argv;

    /* Terminate any target. */
    if (target)
        WT_ERR(__wt_buf_catfmt(session_impl, tmp, "%s", ")"));

    if ((ret = session->open_cursor(
           session, "backup:", NULL, target ? (char *)tmp->data : NULL, &cursor)) != 0) {
        fprintf(stderr, "%s: cursor open(backup:) failed: %s\n", progname,
          session->strerror(session, ret));
        goto err;
    }

    /* Copy the files. */
    while ((ret = cursor->next(cursor)) == 0 && (ret = cursor->get_key(cursor, &name)) == 0)
        if ((ret = copy(session, directory, name)) != 0)
            goto err;
    if (ret == WT_NOTFOUND)
        ret = 0;

    if (ret != 0) {
        fprintf(stderr, "%s: cursor next(backup:) failed: %s\n", progname,
          session->strerror(session, ret));
        goto err;
    }

err:
    __wt_scr_free(session_impl, &tmp);
    return (ret);
}

/*
 * copy --
 *     TODO: Add a comment describing this function.
 */
static int
copy(WT_SESSION *session, const char *directory, const char *name)
{
    WT_DECL_RET;
    size_t len;
    char *to;

    to = NULL;

    /* Build the target pathname. */
    len = strlen(directory) + strlen(name) + 2;
    if ((to = malloc(len)) == NULL) {
        fprintf(stderr, "%s: %s\n", progname, strerror(errno));
        return (1);
    }
    if ((ret = __wt_snprintf(to, len, "%s/%s", directory, name)) != 0) {
        fprintf(stderr, "%s: %s\n", progname, strerror(ret));
        goto err;
    }

    if (verbose && printf("Backing up %s/%s to %s\n", home, name, to) < 0) {
        fprintf(stderr, "%s: %s\n", progname, strerror(EIO));
        goto err;
    }

    /*
     * Use WiredTiger to copy the file: ensuring stability of the copied file on disk requires care,
     * and WiredTiger knows how to do it.
     */
    if ((ret = __wt_copy_and_sync(session, name, to)) != 0)
        fprintf(stderr, "%s/%s to %s: backup copy: %s\n", home, name, to,
          session->strerror(session, ret));

err:
    free(to);
    return (ret);
}
