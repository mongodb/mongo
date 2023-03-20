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
      "-o", "allow overwrite of previously existing records", "-r", "remove an existing record",
      "-?", "show this message", NULL, NULL};

    util_usage("write [-aor] uri key value ...", "options:", options);
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
    bool append, overwrite, remove, rkey;

    append = overwrite = remove = false;
    uri = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "aor?")) != EOF)
        switch (ch) {
        case 'a':
            append = true;
            break;
        case 'o':
            overwrite = true;
            break;
        case 'r':
            remove = true;
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
     * The remaining arguments are
     *   - a uri followed by a list of values (if append is set), or
     *   - a uri followed by a key (if remove is set), or
     *   - a uri followed by key/value pairs.
     */
    if (append) {
        if (argc < 2)
            return (usage());
    } else if (remove) {
        if (argc != 2)
            return (usage());
    } else {
        if (argc < 3 || ((argc - 1) % 2 != 0))
            return (usage());
    }

    if ((uri = util_uri(session, *argv, "table")) == NULL)
        return (1);

    /*
     * Open the object; free allocated memory immediately to simplify future error handling.
     */
    if ((ret = __wt_snprintf(config, sizeof(config), "append=%s,overwrite=%s",
           append ? "true" : "false", overwrite ? "true" : "false")) != 0) {
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

    /* Run through the values or a single key or key/value pairs. */
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
        if (remove) {
            if ((ret = cursor->remove(cursor)) != 0)
                return (util_cerr(cursor, "remove", ret));
            break;
        }
        cursor->set_value(cursor, *argv);

        if ((ret = cursor->insert(cursor)) != 0)
            return (util_cerr(cursor, "insert", ret));
    }

    return (0);
}
