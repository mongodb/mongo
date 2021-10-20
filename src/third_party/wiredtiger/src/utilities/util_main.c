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
const char *usage_prefix = "[-LRSVv] [-C config] [-E secretkey] [-h home]";
bool verbose = false; /* Verbose flag */

static const char *command; /* Command name */

#define REC_ERROR "log=(recover=error)"
#define REC_LOGOFF "log=(enabled=false)"
#define REC_RECOVER "log=(recover=on)"
#define REC_SALVAGE "log=(recover=salvage)"

static void
usage(void)
{
    fprintf(stderr, "WiredTiger Data Engine (version %d.%d)\n", WIREDTIGER_VERSION_MAJOR,
      WIREDTIGER_VERSION_MINOR);
    fprintf(stderr,
      "global options:\n"
      "\t"
      "-C\t"
      "wiredtiger_open configuration\n"
      "\t"
      "-h\t"
      "database directory\n"
      "\t"
      "-L\t"
      "turn logging off for debug-mode\n"
      "\t"
      "-R\t"
      "run recovery if configured\n"
      "\t"
      "-V\t"
      "display library version and exit\n"
      "\t"
      "-v\t"
      "verbose\n");
    fprintf(stderr,
      "commands:\n"
      "\t"
      "alter\t  alter an object\n"
      "\t"
      "backup\t  database backup\n"
      "\t"
      "compact\t  compact an object\n"
      "\t"
      "copyright copyright information\n"
      "\t"
      "create\t  create an object\n"
      "\t"
      "downgrade downgrade a database\n"
      "\t"
      "drop\t  drop an object\n"
      "\t"
      "dump\t  dump an object\n"
      "\t"
      "list\t  list database objects\n"
      "\t"
      "load\t  load an object\n"
      "\t"
      "loadtext  load an object from a text file\n"
      "\t"
      "printlog  display the database log\n"
      "\t"
      "read\t  read values from an object\n"
      "\t"
      "rebalance rebalance an object\n"
      "\t"
      "rename\t  rename an object\n"
      "\t"
      "salvage\t  salvage a file\n"
      "\t"
      "stat\t  display statistics for an object\n"
      "\t"
      "truncate  truncate an object, removing all content\n"
      "\t"
      "upgrade\t  upgrade an object\n"
      "\t"
      "verify\t  verify an object\n"
      "\t"
      "write\t  write values to an object\n");
}

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    size_t len;
    int (*cfunc)(WT_SESSION *, WT_CONNECTION *, int, char *[]);
    int ch, major_v, minor_v, tret, (*func)(WT_SESSION *, int, char *[]);
    char *p, *secretkey;
    const char *cmd_config, *config, *p1, *p2, *p3, *rec_config;
    bool logoff, needconn, recover, salvage;

    conn = NULL;
    p = NULL;

    /* Get the program name. */
    if ((progname = strrchr(argv[0], '/')) == NULL)
        progname = argv[0];
    else
        ++progname;
    command = "";

    needconn = false;

    /* Check the version against the library build. */
    (void)wiredtiger_version(&major_v, &minor_v, NULL);
    if (major_v != WIREDTIGER_VERSION_MAJOR || minor_v != WIREDTIGER_VERSION_MINOR) {
        fprintf(stderr,
          "%s: program build version %d.%d does not match "
          "library build version %d.%d\n",
          progname, WIREDTIGER_VERSION_MAJOR, WIREDTIGER_VERSION_MINOR, major_v, minor_v);
        return (EXIT_FAILURE);
    }

    cmd_config = config = secretkey = NULL;
    /*
     * We default to returning an error if recovery needs to be run. Generally we expect this to be
     * run after a clean shutdown. The printlog command disables logging entirely. If recovery is
     * needed, the user can specify -R to run recovery.
     */
    rec_config = REC_ERROR;
    logoff = recover = salvage = false;
    /* Check for standard options. */
    while ((ch = __wt_getopt(progname, argc, argv, "C:E:h:LRSVv")) != EOF)
        switch (ch) {
        case 'C': /* wiredtiger_open config */
            cmd_config = __wt_optarg;
            break;
        case 'E':            /* secret key */
            free(secretkey); /* lint: set more than once */
            if ((secretkey = strdup(__wt_optarg)) == NULL) {
                (void)util_err(NULL, errno, NULL);
                goto err;
            }
            memset(__wt_optarg, 0, strlen(__wt_optarg));
            break;
        case 'h': /* home directory */
            home = __wt_optarg;
            break;
        case 'L': /* no logging */
            rec_config = REC_LOGOFF;
            logoff = true;
            break;
        case 'R': /* recovery */
            rec_config = REC_RECOVER;
            recover = true;
            break;
        case 'S': /* salvage */
            rec_config = REC_SALVAGE;
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
    argc -= __wt_optind;
    argv += __wt_optind;

    /* The next argument is the command name. */
    if (argc < 1) {
        usage();
        goto err;
    }
    command = argv[0];

    /* Reset getopt. */
    __wt_optreset = __wt_optind = 1;

    func = NULL;
    cfunc = NULL;
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
        if (strcmp(command, "downgrade") == 0) {
            cfunc = util_downgrade;
            needconn = true;
        } else if (strcmp(command, "drop") == 0)
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
        else if (strcmp(command, "rebalance") == 0)
            func = util_rebalance;
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
    if (func == NULL && cfunc == NULL) {
        usage();
        goto err;
    }

    /* Build the configuration string. */
    len = 10; /* some slop */
    p1 = p2 = p3 = "";
    if (config != NULL)
        len += strlen(config);
    if (cmd_config != NULL)
        len += strlen(cmd_config);
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
    if ((ret = __wt_snprintf(p, len, "%s,%s,%s%s%s%s", config == NULL ? "" : config,
           cmd_config == NULL ? "" : cmd_config, rec_config, p1, p2, p3)) != 0) {
        (void)util_err(NULL, ret, NULL);
        goto err;
    }
    config = p;

    /* Open the database and a session. */
    if ((ret = wiredtiger_open(home, verbose ? verbose_handler : NULL, config, &conn)) != 0) {
        (void)util_err(NULL, ret, NULL);
        goto err;
    }
    if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
        (void)util_err(NULL, ret, NULL);
        goto err;
    }

    /* Call the function. */
    if (needconn)
        ret = cfunc(session, conn, argc, argv);
    else
        ret = func(session, argc, argv);

    if (0) {
err:
        ret = 1;
    }
done:

    /* Close the database. */
    if (conn != NULL && (tret = conn->close(conn, NULL)) != 0 && ret == 0)
        ret = tret;

    free(p);
    free(secretkey);

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
