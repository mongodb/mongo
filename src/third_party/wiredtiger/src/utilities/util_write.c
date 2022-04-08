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
 *     Display a usage message for the write command.
 */
static int
usage(void)
{
    static const char *options[] = {"-a", "append each value as a new record in the data source",
      "-o", "allow overwrite of previously existing records", NULL, NULL};

    util_usage("write [-ao] uri key ...", "options:", options);
    return (1);
}

/*
 * util_write --
 *     The write command.
 */
int
util_write(WT_SESSION *session, int argc, char *argv[])
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint64_t recno;
    int ch;
    char *uri, config[100];
    bool append, overwrite, rkey;

    append = overwrite = false;
    uri = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "ao")) != EOF)
        switch (ch) {
        case 'a':
            append = true;
            break;
        case 'o':
            overwrite = true;
            break;
        case '?':
        default:
            return (usage());
        }
    argc -= __wt_optind;
    argv += __wt_optind;

    /*
     * The remaining arguments are a uri followed by a list of values (if append is set), or
     * key/value pairs (if append is not set).
     */
    if (append) {
        if (argc < 2)
            return (usage());
    } else if (argc < 3 || ((argc - 1) % 2 != 0))
        return (usage());
    if ((uri = util_uri(session, *argv, "table")) == NULL)
        return (1);

    /*
     * Open the object; free allocated memory immediately to simplify future error handling.
     */
    if ((ret = __wt_snprintf(config, sizeof(config), "%s,%s", append ? "append=true" : "",
           overwrite ? "overwrite=true" : "")) != 0) {
        free(uri);
        return (util_err(session, ret, NULL));
    }
    if ((ret = session->open_cursor(session, uri, NULL, config, &cursor)) != 0)
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
          "%s: write command only possible when the key format is a record number or string\n",
          progname);
        return (1);
    }
    rkey = WT_STREQ(cursor->key_format, "r");
    if (!WT_STREQ(cursor->value_format, "S")) {
        fprintf(
          stderr, "%s: write command only possible when the value format is a string\n", progname);
        return (1);
    }

    /* Run through the values or key/value pairs. */
    while (*++argv != NULL) {
        if (!append) {
            if (rkey) {
                if (util_str2num(session, *argv, true, &recno))
                    return (1);
                cursor->set_key(cursor, recno);
            } else
                cursor->set_key(cursor, *argv);
            ++argv;
        }
        cursor->set_value(cursor, *argv);

        if ((ret = cursor->insert(cursor)) != 0)
            return (util_cerr(cursor, "search", ret));
    }

    return (0);
}
