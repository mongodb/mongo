/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"
#include "util_load.h"

static int config_read(WT_SESSION *, char ***, bool *);
static int config_rename(WT_SESSION *, char **, const char *);
static int format(WT_SESSION *);
static int insert(WT_CURSOR *, const char *);
static int load_dump(WT_SESSION *);

static bool append = false;       /* -a append (ignore number keys) */
static char *cmdname;             /* -r rename */
static char **cmdconfig;          /* configuration pairs */
static bool json = false;         /* -j input is JSON format */
static bool no_overwrite = false; /* -n don't overwrite existing data */

/*
 * usage --
 *     Display a usage message for the load command.
 */
static int
usage(void)
{
    static const char *options[] = {"-a",
      "ignore record number keys in the input and assign new record number keys", "-f input",
      "read from the specified file (by default records are read from stdin)", "-j",
      "read in JSON format", "-n", "fail at any attempt to overwrite existing data", "-r name",
      "use the argument as the table name, ignoring any name in the source", "-?",
      "show this message", NULL, NULL};

    util_usage(
      "load [-ajn] [-f input-file] [-r name] [object configuration ...]", "options:", options);
    return (1);
}

/*
 * util_load --
 *     The load command.
 */
int
util_load(WT_SESSION *session, int argc, char *argv[])
{
    uint32_t flags;
    int ch;
    const char *filename;

    flags = 0;

    filename = "<stdin>";
    while ((ch = __wt_getopt(progname, argc, argv, "af:jnr:?")) != EOF)
        switch (ch) {
        case 'a': /* append (ignore record number keys) */
            append = true;
            break;
        case 'f': /* input file */
            if (freopen(__wt_optarg, "r", stdin) == NULL)
                return (util_err(session, errno, "%s: reopen", __wt_optarg));
            else
                filename = __wt_optarg;
            break;
        case 'j': /* input is JSON */
            json = true;
            break;
        case 'n': /* don't overwrite existing data */
            no_overwrite = true;
            break;
        case 'r': /* rename */
            cmdname = __wt_optarg;
            break;
        case '?':
            usage();
            return (0);
        default:
            return (usage());
        }
    argc -= __wt_optind;
    argv += __wt_optind;

    /* -a and -o are mutually exclusive. */
    if (append && no_overwrite)
        return (util_err(
          session, EINVAL, "the -a (append) and -n (no-overwrite) flags are mutually exclusive"));

    /* The remaining arguments are configuration uri/string pairs. */
    if (argc != 0) {
        if (argc % 2 != 0)
            return (usage());
        cmdconfig = argv;
    }

    if (json) {
        if (append)
            flags |= LOAD_JSON_APPEND;
        if (no_overwrite)
            flags |= LOAD_JSON_NO_OVERWRITE;
        return (util_load_json(session, filename, flags));
    }
    return (load_dump(session));
}

/*
 * load_dump --
 *     Load from the WiredTiger dump format.
 */
static int
load_dump(WT_SESSION *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    int tret;
    char **list, **tlist, *uri, config[64];
    bool hex;

    cursor = NULL;
    list = NULL; /* -Wuninitialized */
    hex = false; /* -Wuninitialized */
    uri = NULL;

    /* Read the metadata file. */
    if ((ret = config_read(session, &list, &hex)) != 0)
        return (ret);

    /* Reorder and check the list. */
    if ((ret = config_reorder(session, list)) != 0)
        goto err;

    /* Update the config based on any command-line configuration. */
    if ((ret = config_update(session, list)) != 0)
        goto err;

    uri = list[0];
    /* Create the items in the list. */
    if ((ret = config_exec(session, list)) != 0)
        goto err;

    /* Open the insert cursor. */
    if ((ret = __wt_snprintf(config, sizeof(config), "dump=%s%s%s", hex ? "hex" : "print",
           append ? ",append" : "", no_overwrite ? ",overwrite=false" : "")) != 0) {
        ret = util_err(session, ret, NULL);
        goto err;
    }
    if ((ret = session->open_cursor(session, uri, NULL, config, &cursor)) != 0) {
        ret = util_err(session, ret, "%s: session.open_cursor", uri);
        goto err;
    }

    /*
     * Check the append flag (it only applies to objects where the primary key is a record number).
     */
    if (append && !WT_STREQ(cursor->key_format, "r")) {
        fprintf(stderr, "%s: %s: -a option illegal unless the primary key is a record number\n",
          progname, uri);
        ret = 1;
    } else
        ret = insert(cursor, uri);

err:
    /*
     * Technically, we don't have to close the cursor because the session handle will do it for us,
     * but I'd like to see the flush to disk and the close succeed, it's better to fail early when
     * loading files.
     */
    if (cursor != NULL && (tret = cursor->close(cursor)) != 0) {
        tret = util_err(session, tret, "%s: cursor.close", uri);
        if (ret == 0)
            ret = tret;
    }
    if (ret == 0)
        ret = util_flush(session, uri);

    for (tlist = list; *tlist != NULL; ++tlist)
        free(*tlist);
    free(list);

    return (ret == 0 ? 0 : 1);
}

/*
 * config_exec --
 *     Create the tables/indices/colgroups implied by the list.
 */
int
config_exec(WT_SESSION *session, char **list)
{
    WT_DECL_RET;

    for (; *list != NULL; list += 2)
        if ((ret = session->create(session, list[0], list[1])) != 0)
            return (util_err(session, ret, "%s: session.create", list[0]));
    return (0);
}

/*
 * config_list_add --
 *     Add a value to the config list.
 */
int
config_list_add(WT_SESSION *session, CONFIG_LIST *clp, char *val)
{
    if (clp->entry + 1 >= clp->max_entry)
        if ((clp->list = realloc(clp->list, (size_t)(clp->max_entry += 100) * sizeof(char *))) ==
          NULL)
            /* List already freed by realloc. */
            return (util_err(session, errno, NULL));

    clp->list[clp->entry++] = val;
    clp->list[clp->entry] = NULL;
    return (0);
}

/*
 * config_list_free --
 *     Free the list and any of its entries.
 */
void
config_list_free(CONFIG_LIST *clp)
{
    char **entry;

    if (clp->list != NULL)
        for (entry = &clp->list[0]; *entry != NULL; entry++)
            free(*entry);
    free(clp->list);
    clp->list = NULL;
    clp->entry = 0;
    clp->max_entry = 0;
}

/*
 * config_read --
 *     Read the config lines and do some basic validation.
 */
static int
config_read(WT_SESSION *session, char ***listp, bool *hexp)
{
    ULINE l;
    WT_DECL_RET;
    int entry, max_entry;
    bool eof;
    const char *s;
    char **list, **tlist;

    list = NULL;
    memset(&l, 0, sizeof(l));

    /* Header line #1: "WiredTiger Dump" and a WiredTiger version. */
    if ((ret = util_read_line(session, &l, false, &eof)) != 0)
        goto err;
    s = "WiredTiger Dump ";
    if (strncmp(l.mem, s, strlen(s)) != 0) {
        ret = format(session);
        goto err;
    }

    /* Header line #2: "Format={hex,print}". */
    if ((ret = util_read_line(session, &l, false, &eof)) != 0)
        goto err;
    if (strcmp(l.mem, "Format=print") == 0)
        *hexp = false;
    else if (strcmp(l.mem, "Format=hex") == 0)
        *hexp = true;
    else {
        ret = format(session);
        goto err;
    }

    /* Header line #3: "Header". */
    if ((ret = util_read_line(session, &l, false, &eof)) != 0)
        goto err;
    if (strcmp(l.mem, "Header") != 0) {
        ret = format(session);
        goto err;
    }

    /* Now, read in lines until we get to the end of the headers. */
    for (entry = max_entry = 0, list = NULL;; ++entry) {
        if ((ret = util_read_line(session, &l, false, &eof)) != 0)
            goto err;
        if (strcmp(l.mem, "Data") == 0)
            break;

        /*
         * Grow the array of header lines as necessary -- we need an extra slot for NULL
         * termination.
         */
        if (entry + 1 >= max_entry) {
            if ((tlist = realloc(list, (size_t)(max_entry += 100) * sizeof(char *))) == NULL) {
                ret = util_err(session, errno, NULL);

                /*
                 * List already freed by realloc, still use err label for consistency.
                 */
                list = NULL;
                goto err;
            }
            list = tlist;
        }
        if ((list[entry] = strdup(l.mem)) == NULL) {
            ret = util_err(session, errno, NULL);
            goto err;
        }
        list[entry + 1] = NULL;
    }

    /* Headers are required, and they're supposed to be in pairs. */
    if (list == NULL || entry % 2 != 0) {
        ret = format(session);
        goto err;
    }
    *listp = list;

    free(l.mem);
    return (0);

err:
    if (list != NULL) {
        for (tlist = list; *tlist != NULL; ++tlist)
            free(*tlist);
        free(list);
    }
    free(l.mem);
    return (ret);
}

/*
 * config_reorder --
 *     For table dumps, reorder the list so tables are first. For other dumps, make any needed
 *     checks.
 */
int
config_reorder(WT_SESSION *session, char **list)
{
    char **entry, *p;

    /*
     * Search for a table name -- if we find one, then it's table dump, otherwise, it's a single
     * file dump.
     */
    for (entry = list; *entry != NULL; ++entry)
        if (WT_PREFIX_MATCH(*entry, "table:"))
            break;
    if (*entry == NULL) {
        /*
         * Single file dumps can only have two lines, the file name and the configuration
         * information.
         */
        if ((list[0] == NULL || list[1] == NULL || list[2] != NULL) ||
          (!WT_PREFIX_MATCH(list[0], "file:") && !WT_PREFIX_MATCH(list[0], "lsm:") &&
            !WT_PREFIX_MATCH(list[0], "tiered:")))
            return (format(session));

        entry = list;
    }

    /*
     * Make sure the table key/value pair comes first, then we can just run through the array in
     * order. (We already checked that we had a multiple of 2 entries, so this is safe.)
     */
    if (entry != list) {
        p = list[0];
        list[0] = entry[0];
        entry[0] = p;
        p = list[1];
        list[1] = entry[1];
        entry[1] = p;
    }
    return (0);
}

/*
 * config_update --
 *     Reconcile and update the command line configuration against the config we found.
 */
int
config_update(WT_SESSION *session, char **list)
{
    WT_DECL_RET;
    size_t cnt;
    int found;
    char **configp, **listp;
    const char *p, **cfg;

    /*
     * If the object has been renamed, replace all of the column group, index, file and table names
     * with the new name.
     */
    if (cmdname != NULL) {
        for (listp = list; *listp != NULL; listp += 2)
            if (WT_PREFIX_MATCH(*listp, "colgroup:") || WT_PREFIX_MATCH(*listp, "file:") ||
              WT_PREFIX_MATCH(*listp, "index:") || WT_PREFIX_MATCH(*listp, "lsm:") ||
              WT_PREFIX_MATCH(*listp, "table:") || WT_PREFIX_MATCH(*listp, "tiered:"))
                if (config_rename(session, listp, cmdname))
                    return (1);

        /*
         * If the object was renamed, and there are configuration pairs, rename the configuration
         * pairs as well, because we don't know if the user used the old or new names for the pair's
         * URI.
         */
        for (configp = cmdconfig; cmdconfig != NULL && *configp != NULL; configp += 2)
            if (config_rename(session, configp, cmdname))
                return (1);
    }

    /*
     * Updating the key/value formats seems like an easy mistake to make. If there were command-line
     * configuration pairs, walk the list of command-line configuration strings and check.
     */
    for (configp = cmdconfig; configp != NULL && *configp != NULL; configp += 2)
        if (strstr(configp[1], "key_format=") || strstr(configp[1], "value_format="))
            return (util_err(session, 0, "an object's key or value format may not be modified"));

    /*
     * If there were command-line configuration pairs, walk the list of command-line URIs and find a
     * matching dump URI. It is an error if a command-line URI doesn't find a single, exact match,
     * that's likely a mistake.
     */
    for (configp = cmdconfig; configp != NULL && *configp != NULL; configp += 2) {
        for (found = 0, listp = list; *listp != NULL; listp += 2)
            if (strncmp(*configp, listp[0], strlen(*configp)) == 0)
                ++found;
        switch (found) {
        case 0:
            return (util_err(session, 0,
              "the command line object name %s was not matched by any loaded object name",
              *configp));
        case 1:
            break;
        default:
            return (util_err(session, 0,
              "the command line object name %s was not unique, matching more than a single loaded "
              "object name",
              *configp));
        }
    }

    /*
     * Allocate a big enough configuration stack to hold all of the command line arguments, a list
     * of configuration values to remove, and the base configuration values, plus some slop.
     */
    for (cnt = 0, configp = cmdconfig; cmdconfig != NULL && *configp != NULL; configp += 2)
        ++cnt;
    if ((cfg = calloc(cnt + 10, sizeof(cfg[0]))) == NULL)
        return (util_err(session, errno, NULL));

    /*
     * For each match, rewrite the dump configuration as described by any command-line configuration
     * arguments.
     *
     * New filenames will be chosen as part of the table load, remove all "filename=", "source=" and
     * other configurations that foil loading from the values; we call an unpublished API to do the
     * work.
     */
    for (listp = list; *listp != NULL; listp += 2) {
        cnt = 0;
        cfg[cnt++] = listp[1];
        for (configp = cmdconfig; cmdconfig != NULL && *configp != NULL; configp += 2)
            if (strncmp(*configp, listp[0], strlen(*configp)) == 0)
                cfg[cnt++] = configp[1];
        cfg[cnt++] = NULL;

        if ((ret = __wt_config_merge((WT_SESSION_IMPL *)session, cfg,
               "filename=,id=,checkpoint=,checkpoint_backup_info=,checkpoint_lsn=,version=,source="
               ",",
               &p)) != 0)
            break;

        free(listp[1]);
        listp[1] = (char *)p;
    }
    free(cfg);
    return (ret);
}

/*
 * config_rename --
 *     Update the URI name.
 */
static int
config_rename(WT_SESSION *session, char **urip, const char *name)
{
    WT_DECL_RET;
    size_t len;
    char *buf, *p;

    /* Allocate room. */
    len = strlen(*urip) + strlen(name) + 10;
    if ((buf = malloc(len)) == NULL)
        return (util_err(session, errno, NULL));

    /*
     * Find the separating colon characters, but note the trailing one may not be there.
     */
    if ((p = strchr(*urip, ':')) == NULL) {
        free(buf);
        return (format(session));
    }
    *p = '\0';
    p = strchr(p + 1, ':');
    if ((ret = __wt_snprintf(buf, len, "%s:%s%s", *urip, name, p == NULL ? "" : p)) != 0) {
        free(buf);
        return (util_err(session, ret, NULL));
    }
    *urip = buf;

    return (0);
}

/*
 * format --
 *     The input doesn't match the dump format.
 */
static int
format(WT_SESSION *session)
{
    return (util_err(session, 0, "input does not match WiredTiger dump format"));
}

/*
 * insert --
 *     Read and insert data.
 */
static int
insert(WT_CURSOR *cursor, const char *name)
{
    ULINE key, value;
    WT_DECL_RET;
    WT_SESSION *session;
    uint64_t insert_count;
    bool eof;

    session = cursor->session;

    memset(&key, 0, sizeof(key));
    memset(&value, 0, sizeof(value));

    /* Read key/value pairs and insert them into the file. */
    for (insert_count = 0;;) {
        /*
         * Three modes: in row-store, we always read a key and use it, in column-store, we might
         * read it (a dump), we might read and ignore it (a dump with "append" set), or not read it
         * at all (flat-text load).
         */
        if ((ret = util_read_line(session, &key, true, &eof)) != 0)
            goto err;
        if (eof)
            break;
        if (!append)
            cursor->set_key(cursor, key.mem);

        if ((ret = util_read_line(session, &value, false, &eof)) != 0)
            goto err;
        cursor->set_value(cursor, value.mem);

        if ((ret = cursor->insert(cursor)) != 0) {
            ret = util_err(session, ret, "%s: cursor.insert", name);
            goto err;
        }

        /* Report on progress every 100 inserts. */
        if (verbose && ++insert_count % 100 == 0) {
            printf("\r\t%s: %" PRIu64, name, insert_count);
            fflush(stdout);
        }
    }

    if (verbose)
        printf("\r\t%s: %" PRIu64 "\n", name, insert_count);

err:
    free(key.mem);
    free(value.mem);

    return (ret);
}
