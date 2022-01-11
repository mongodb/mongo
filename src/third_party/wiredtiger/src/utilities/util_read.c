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
 *     TODO: Add a comment describing this function.
 */
static int
usage(void)
{
    util_usage("read uri key ...", NULL, NULL);
    return (1);
}

/*
 * util_read --
 *     TODO: Add a comment describing this function.
 */
int
util_read(WT_SESSION *session, int argc, char *argv[])
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint64_t recno;
    int ch;
    char *uri, *value;
    bool rkey, rval;

    uri = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "")) != EOF)
        switch (ch) {
        case '?':
        default:
            return (usage());
        }
    argc -= __wt_optind;
    argv += __wt_optind;

    /* The remaining arguments are a uri followed by a list of keys. */
    if (argc < 2)
        return (usage());
    if ((uri = util_uri(session, *argv, "table")) == NULL)
        return (1);

    /*
     * Open the object; free allocated memory immediately to simplify future error handling.
     */
    if ((ret = session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0)
        (void)util_err(session, ret, "%s: session.open_cursor", uri);
    free(uri);
    if (ret != 0)
        return (ret);

    /*
     * A simple search only makes sense if the key format is a string or a record number, and the
     * value format is a single string.
     */
    if (!WT_STREQ(cursor->key_format, "r") && !WT_STREQ(cursor->key_format, "S")) {
        fprintf(stderr,
          "%s: read command only possible when the key format is a record number or string\n",
          progname);
        return (1);
    }
    rkey = WT_STREQ(cursor->key_format, "r");
    if (!WT_STREQ(cursor->value_format, "S")) {
        fprintf(
          stderr, "%s: read command only possible when the value format is a string\n", progname);
        return (1);
    }

    /*
     * Run through the keys, returning non-zero on error or if any requested key isn't found.
     */
    for (rval = false; *++argv != NULL;) {
        if (rkey) {
            if (util_str2num(session, *argv, true, &recno))
                return (1);
            cursor->set_key(cursor, recno);
        } else
            cursor->set_key(cursor, *argv);

        switch (ret = cursor->search(cursor)) {
        case 0:
            if ((ret = cursor->get_value(cursor, &value)) != 0)
                return (util_cerr(cursor, "get_value", ret));
            if (printf("%s\n", value) < 0)
                return (util_err(session, EIO, NULL));
            break;
        case WT_NOTFOUND:
            (void)util_err(session, 0, "%s: not found", *argv);
            rval = true;
            break;
        default:
            return (util_cerr(cursor, "search", ret));
        }
    }

    return (rval ? 1 : 0);
}
