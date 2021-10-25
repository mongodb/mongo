/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

const char *home = "."; /* Home directory */
const char *progname;   /* Program name */
                        /* Global arguments */
const char *usage_prefix = "[-LmRrSVv] [-C config] [-E secretkey] [-h home]";
bool verbose = false; /* Verbose flag */

static const char *command; /* Command name */

/* Give users a hint in the help output for if they're trying to read MongoDB data files */
static const char *mongodb_config = "log=(enabled=true,path=journal,compressor=snappy)";

#define READONLY "readonly=true"
#define REC_ERROR "log=(recover=error)"
#define REC_LOGOFF "log=(enabled=false)"
#define REC_RECOVER "log=(recover=on)"
#define SALVAGE "salvage=true"

/*
 * wt_explicit_zero: clear a buffer, with precautions against being optimized away.
 */
static void
wt_explicit_zero(void *ptr, size_t len)
{
    /* Call through a volatile pointer to avoid removal even when it's a dead store. */
    static void *(*volatile memsetptr)(void *ptr, int ch, size_t len) = memset;
    (void)memsetptr(ptr, '\0', len);
}

static void
usage(void)
{
    static const char *options[] = {"-B", "maintain release 3.3 log file compatibility",
      "-C config", "wiredtiger_open configuration", "-E key", "secret encryption key", "-h home",
      "database directory", "-L", "turn logging off for debug-mode", "-m", "run verify on metadata",
      "-R", "run recovery (if recovery configured)", "-r",
      "access the database via a readonly connection", "-S",
      "run salvage recovery (if recovery configured)", "-V", "display library version and exit",
      "-v", "verbose", NULL, NULL};
    static const char *commands[] = {"alter", "alter an object", "backup", "database backup",
      "compact", "compact an object", "copyright", "display copyright information", "create",
      "create an object", "downgrade", "downgrade a database", "drop", "drop an object", "dump",
      "dump an object", "list", "list database objects", "load", "load an object", "loadtext",
      "load an object from a text file", "printlog", "display the database log", "read",
      "read values from an object", "rename", "rename an object", "salvage", "salvage a file",
      "stat", "display statistics for an object", "truncate",
      "truncate an object, removing all content", "upgrade", "upgrade an object", "verify",
      "verify an object", "write", "write values to an object", NULL, NULL};

    fprintf(stderr, "WiredTiger Data Engine (version %d.%d)\n", WIREDTIGER_VERSION_MAJOR,
      WIREDTIGER_VERSION_MINOR);
    fprintf(stderr, "MongoDB wiredtiger_open configuration: \"%s\"\n", mongodb_config);
    util_usage(NULL, "global_options:", options);
    util_usage(NULL, "commands:", commands);
}

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    size_t len;
    int ch, major_v, minor_v, tret, (*func)(WT_SESSION *, int, char *[]);
    char *p, *secretkey;
    const char *cmd_config, *config, *p1, *p2, *p3, *readonly_config, *rec_config, *salvage_config;
    bool backward_compatible, logoff, meta_verify, readonly, recover, salvage;

    conn = NULL;
    p = NULL;

    /* Get the program name. */
    if ((progname = strrchr(argv[0], '/')) == NULL)
        progname = argv[0];
    else
        ++progname;
    command = "";

    /* Check the version against the library build. */
    (void)wiredtiger_version(&major_v, &minor_v, NULL);
    if (major_v != WIREDTIGER_VERSION_MAJOR || minor_v != WIREDTIGER_VERSION_MINOR) {
        fprintf(stderr,
          "%s: program build version %d.%d does not match library build version %d.%d\n", progname,
          WIREDTIGER_VERSION_MAJOR, WIREDTIGER_VERSION_MINOR, major_v, minor_v);
        return (EXIT_FAILURE);
    }

    cmd_config = config = readonly_config = salvage_config = secretkey = NULL;
    /*
     * We default to returning an error if recovery needs to be run. Generally we expect this to be
     * run after a clean shutdown. The printlog command disables logging entirely. If recovery is
     * needed, the user can specify -R to run recovery.
     */
    rec_config = REC_ERROR;
    backward_compatible = logoff = meta_verify = readonly = recover = salvage = false;
    /* Check for standard options. */
    while ((ch = __wt_getopt(progname, argc, argv, "BC:E:h:LmRrSVv")) != EOF)
        switch (ch) {
        case 'B': /* backward compatibility */
            backward_compatible = true;
            break;
        case 'C': /* wiredtiger_open config */
            cmd_config = __wt_optarg;
            break;
        case 'E': /* secret key */
            if (secretkey != NULL)
                wt_explicit_zero(secretkey, strlen(secretkey));
            free(secretkey); /* lint: set more than once */
            if ((secretkey = strdup(__wt_optarg)) == NULL) {
                (void)util_err(NULL, errno, NULL);
                goto err;
            }
            wt_explicit_zero(__wt_optarg, strlen(__wt_optarg));
            break;
        case 'h': /* home directory */
            home = __wt_optarg;
            break;
        case 'L': /* no logging */
            rec_config = REC_LOGOFF;
            logoff = true;
            break;
        case 'm': /* verify metadata on connection open */
            cmd_config = "verify_metadata=true";
            meta_verify = true;
            break;
        case 'R': /* recovery */
            rec_config = REC_RECOVER;
            recover = true;
            break;
        case 'r':
            readonly_config = READONLY;
            readonly = true;
            break;
        case 'S': /* salvage */
            salvage_config = SALVAGE;
            salvage = true;
            break;
        case 'V': /* version */
            printf("%s\n", wiredtiger_version(NULL, NULL, NULL));
            goto done;
        case 'v': /* verbose */
            verbose = true;
            break;
        case '?':
        default:
            usage();
            goto err;
        }
    if ((logoff && recover) || (logoff && salvage) || (recover && salvage)) {
        fprintf(stderr, "Only one of -L, -R, and -S is allowed.\n");
        goto err;
    }
    if ((recover || salvage) && readonly) {
        fprintf(stderr, "-R and -S cannot be used with -r\n");
        goto err;
    }
    argc -= __wt_optind;
    argv += __wt_optind;

    func = NULL;
    /* The next argument is the command name. */
    if (argc < 1) {
        if (meta_verify)
            goto open;
        usage();
        goto err;
    }
    command = argv[0];

    /* Reset getopt. */
    __wt_optreset = __wt_optind = 1;
    switch (command[0]) {
    case 'a':
        if (strcmp(command, "alter") == 0)
            func = util_alter;
        break;
    case 'b':
        if (strcmp(command, "backup") == 0)
            func = util_backup;
        break;
    case 'c':
        if (strcmp(command, "compact") == 0)
            func = util_compact;
        else if (strcmp(command, "copyright") == 0) {
            util_copyright();
            goto done;
        } else if (strcmp(command, "create") == 0) {
            func = util_create;
            config = "create";
        }
        break;
    case 'd':
        if (strcmp(command, "downgrade") == 0)
            func = util_downgrade;
        else if (strcmp(command, "drop") == 0)
            func = util_drop;
        else if (strcmp(command, "dump") == 0)
            func = util_dump;
        break;
    case 'l':
        if (strcmp(command, "list") == 0)
            func = util_list;
        else if (strcmp(command, "load") == 0) {
            func = util_load;
            config = "create";
        } else if (strcmp(command, "loadtext") == 0) {
            func = util_loadtext;
            config = "create";
        }
        break;
    case 'p':
        if (strcmp(command, "printlog") == 0) {
            func = util_printlog;
            rec_config = REC_LOGOFF;
        }
        break;
    case 'r':
        if (strcmp(command, "read") == 0)
            func = util_read;
        else if (strcmp(command, "rename") == 0)
            func = util_rename;
        break;
    case 's':
        if (strcmp(command, "salvage") == 0)
            func = util_salvage;
        else if (strcmp(command, "stat") == 0) {
            func = util_stat;
            config = "statistics=(all)";
        }
        break;
    case 't':
        if (strcmp(command, "truncate") == 0)
            func = util_truncate;
        break;
    case 'u':
        if (strcmp(command, "upgrade") == 0)
            func = util_upgrade;
        break;
    case 'v':
        if (strcmp(command, "verify") == 0)
            func = util_verify;
        break;
    case 'w':
        if (strcmp(command, "write") == 0)
            func = util_write;
        break;
    default:
        break;
    }
    if (func == NULL) {
        usage();
        goto err;
    }

open:
    /* Build the configuration string. */
    len = 10; /* some slop */
    p1 = p2 = p3 = "";
    len += strlen("error_prefix=wt");
    if (config != NULL)
        len += strlen(config);
    if (cmd_config != NULL)
        len += strlen(cmd_config);
    if (readonly_config != NULL)
        len += strlen(readonly_config);
    if (salvage_config != NULL)
        len += strlen(salvage_config);
    if (secretkey != NULL) {
        len += strlen(secretkey) + 30;
        p1 = ",encryption=(secretkey=";
        p2 = secretkey;
        p3 = ")";
    }
    len += strlen(rec_config);
    if ((p = malloc(len)) == NULL) {
        (void)util_err(NULL, errno, NULL);
        goto err;
    }
    if ((ret = __wt_snprintf(p, len, "error_prefix=wt,%s,%s,%s,%s,%s%s%s%s",
           config == NULL ? "" : config, cmd_config == NULL ? "" : cmd_config,
           readonly_config == NULL ? "" : readonly_config, rec_config,
           salvage_config == NULL ? "" : salvage_config, p1, p2, p3)) != 0) {
        (void)util_err(NULL, ret, NULL);
        goto err;
    }
    config = p;

    if ((ret = wiredtiger_open(home, verbose ? verbose_handler : NULL, config, &conn)) != 0) {
        (void)util_err(NULL, ret, NULL);
        goto err;
    }

    if (secretkey != NULL) {
        /* p contains a copy of secretkey, so zero both before freeing */
        wt_explicit_zero(p, strlen(p));
        wt_explicit_zero(secretkey, strlen(secretkey));
    }
    free(p);
    free(secretkey);
    secretkey = p = NULL;

    /* If we only want to verify the metadata, that is done in wiredtiger_open. We're done. */
    if (func == NULL && meta_verify)
        goto done;

    if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
        (void)util_err(NULL, ret, NULL);
        goto err;
    }

    /* Call the function after opening the database and session. */
    ret = func(session, argc, argv);

    if (0) {
err:
        ret = 1;
    }
done:

    /* may get here via either err or done before the free above happens */
    if (p != NULL) {
        /* p may contain a copy of secretkey, so zero before freeing */
        wt_explicit_zero(p, strlen(p));
        free(p);
    }
    if (secretkey != NULL) {
        wt_explicit_zero(secretkey, strlen(secretkey));
        free(secretkey);
    }

    if (conn != NULL) {
        /* Maintain backward compatibility. */
        if (backward_compatible &&
          (tret = conn->reconfigure(conn, "compatibility=(release=3.3)")) != 0 && ret == 0)
            ret = tret;

        /* Close the database. */
        if ((tret = conn->close(conn, NULL)) != 0 && ret == 0)
            ret = tret;
    }

    return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/*
 * util_uri --
 *     Build a name.
 */
char *
util_uri(WT_SESSION *session, const char *s, const char *type)
{
    WT_DECL_RET;
    size_t len;
    char *name;

    if (WT_PREFIX_MATCH(s, "backup:") || WT_PREFIX_MATCH(s, "config:") ||
      WT_PREFIX_MATCH(s, "statistics:")) {
        fprintf(stderr, "%s: %s: unsupported object type: %s\n", progname, command, s);
        return (NULL);
    }

    len = strlen(type) + strlen(s) + 2;
    if ((name = calloc(len, 1)) == NULL) {
        (void)util_err(session, errno, NULL);
        return (NULL);
    }

    /*
     * If the string has a URI prefix, use it verbatim, otherwise prepend the default type for the
     * operation.
     */
    if (strchr(s, ':') != NULL)
        WT_ERR(__wt_snprintf(name, len, "%s", s));
    else
        WT_ERR(__wt_snprintf(name, len, "%s:%s", type, s));
    return (name);

err:
    free(name);
    (void)util_err(session, ret, NULL);
    return (NULL);
}
