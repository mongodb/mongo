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
const char *usage_prefix = "[-BLmpqRrSVv] [-C config] [-E secretkey] [-h home]";
bool verbose = false;      /* Verbose flag */
bool read_corrupt = false; /* -q: continue past corrupt pages for read-oriented wt commands */

static const char *command; /* Command name */

/* Give users a hint in the help output for if they're trying to read MongoDB data files */
static const char *mongodb_config = "log=(enabled=true,path=journal,compressor=snappy)";

#define READONLY "readonly=true"
#define REC_ERROR "log=(recover=error)"
#define REC_LOGOFF "log=(enabled=false)"
#define REC_RECOVER "log=(recover=on)"
#define SALVAGE "salvage=true"
#define VERIFY_METADATA "verify_metadata=true"

/*
 * wt_explicit_zero --
 *     Clear a buffer, with precautions against being optimized away.
 */
static void
wt_explicit_zero(void *ptr, size_t len)
{
    /* Call through a volatile pointer to avoid removal even when it's a dead store. */
    static void *(*volatile memsetptr)(void *ptr, int ch, size_t len) = memset;
    (void)memsetptr(ptr, '\0', len);
}

/*
 * util_disagg_pick_up_latest_checkpoint --
 *     If the connection is configured for disaggregated storage, fetch the latest complete
 *     checkpoint metadata from the page log and install it via reconfigure.
 *
 * Returns 0 if disagg is not configured, if no checkpoint is available yet (WT_NOTFOUND from the
 *     extension), or after a successful install. Returns the WT error code on failure.
 */
static int
util_disagg_pick_up_latest_checkpoint(WT_CONNECTION *conn, WT_SESSION *session)
{
    WT_CONNECTION_IMPL *conn_impl;
    WT_DECL_RET;
    WT_PAGE_LOG *page_log;
    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS args;
    WT_SESSION_IMPL *session_impl;
    size_t reconfig_len;
    char *reconfig;
    const char *page_log_name;

    session_impl = (WT_SESSION_IMPL *)session;
    conn_impl = S2C(session_impl);
    page_log = NULL;
    reconfig = NULL;
    WT_CLEAR(args);

    /*
     * Leader-mode pickup is handled inside wiredtiger_open by __wti_disagg_conn_config. Connections
     * without a page log don't need pickup.
     */
    if (conn_impl->layered_table_manager.leader ||
      conn_impl->disaggregated_storage.npage_log == NULL)
        return (0);

    page_log_name = conn_impl->disaggregated_storage.page_log;
    WT_ASSERT(session_impl, page_log_name != NULL);

    WT_RET(conn->get_page_log(conn, page_log_name, &page_log));

    /* The wt utility tolerates page logs that do not implement pl_get_complete_checkpoint. */
    if (page_log->pl_get_complete_checkpoint == NULL) {
        fprintf(stderr,
          "%s: disaggregated page log %s does not implement pl_get_complete_checkpoint; "
          "proceeding with empty metadata\n",
          progname, page_log_name);
        goto done;
    }

    ret = page_log->pl_get_complete_checkpoint(page_log, session, &args);
    if (ret == WT_NOTFOUND) {
        fprintf(stderr,
          "%s: no complete checkpoint found in disaggregated page log; proceeding with empty "
          "metadata\n",
          progname);
        ret = 0;
        goto done;
    }
    WT_ERR(ret);

    /*
     * Format the metadata as a disaggregated.checkpoint_meta config string and install via
     * reconfigure.
     */
    reconfig_len =
      strlen("disaggregated=(checkpoint_meta=\"\")") + args.checkpoint_metadata.size + 1;
    if ((reconfig = util_malloc(reconfig_len)) == NULL) {
        ret = errno;
        goto err;
    }
    WT_ERR(__wt_snprintf(reconfig, reconfig_len, "disaggregated=(checkpoint_meta=\"%.*s\")",
      (int)args.checkpoint_metadata.size, (const char *)args.checkpoint_metadata.data));
    /*
     * A failed pickup is non-fatal in the utility: the checkpoint may be corrupt or unreadable, but
     * individual pages can still be read directly off the page log (wt page -t). Warn and proceed
     * with empty metadata.
     */
    if ((ret = conn->reconfigure(conn, reconfig)) != 0) {
        fprintf(stderr,
          "%s: failed to pick up the latest checkpoint (%s); proceeding with empty metadata\n",
          progname, wiredtiger_strerror(ret));
        ret = 0;
    }

err:
done:
    __wt_buf_free(session_impl, &args.checkpoint_metadata);
    util_free(reconfig);
    if (page_log != NULL)
        WT_TRET(page_log->terminate(page_log, session));

    return (ret);
}

/*
 * util_func_supports_read_corrupt --
 *     Whether a wt subcommand accepts the -q (read-corrupt) flag. Only supported for read-oriented
 *     commands. Verify has it's own read_corrupt flag. List, printlog, and page do not benefit from
 *     read_corrupt.
 */
static bool
util_func_supports_read_corrupt(int (*func)(WT_SESSION *, int, char *[]))
{
    return (func == util_dump || func == util_read || func == util_stat);
}

/*
 * usage --
 *     Display a usage message for the wt utility.
 */
static void
usage(void)
{
    static const char *options[] = {"-B", "maintain release 3.3 log file compatibility",
      "-C config", "wiredtiger_open configuration", "-E key", "secret encryption key", "-h home",
      "database directory", "-L", "turn logging off for debug-mode", "-l",
      "run live restore using the source path specified.", "-m", "run verify on metadata", "-p",
      "disable pre-fetching on the connection (use this option when dumping/verifying corrupted "
      "data)",
      "-q",
      "continue past corrupt pages where possible: asks WiredTiger to skip corrupt pages instead "
      "of panicking, for read-oriented commands (dump, read, stat). Output "
      "is best-effort and the command exits non-zero when corruption was encountered.",
      "-R", "run recovery (if recovery configured)", "-r",
      "access the database via a readonly connection", "-S",
      "run salvage recovery (if recovery configured)", "-V", "display library version and exit",
      "-v", "verbose", "-?", "show this message", NULL, NULL};
    static const char *commands[] = {"alter", "alter an object", "backup", "database backup",
      "compact", "compact an object", "copyright", "display copyright information", "create",
      "create an object", "downgrade", "downgrade a database", "drop", "drop an object", "dump",
      "dump an object", "list", "list database objects", "load", "load an object", "loadtext",
      "load an object from a text file", "page", "read a single page through WT_PAGE_LOG",
      "printlog", "display the database log", "read", "read values from an object", "salvage",
      "salvage a file", "stat", "display statistics for an object", "truncate",
      "truncate an object, removing all content", "verify", "verify an object", "write",
      "write values to an object", NULL, NULL};

    fprintf(stderr, "WiredTiger Data Engine (version %d.%d)\n", WIREDTIGER_VERSION_MAJOR,
      WIREDTIGER_VERSION_MINOR);
    fprintf(stderr, "MongoDB wiredtiger_open configuration: \"%s\"\n", mongodb_config);
    util_usage(NULL, "global_options:", options);
    util_usage(NULL, "commands:", commands);
}

/*
 * main --
 *     The wt utility.
 */
int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    size_t len;
    int ch, major_v, minor_v, tret, (*func)(WT_SESSION *, int, char *[]);
    char *p, *secretkey;
    const char *cmd_config, *conn_config, *live_restore_path, *p1, *p2, *p3, *rec_config,
      *session_config;
    bool backward_compatible, disable_prefetch, logoff, meta_verify, readonly, recover, salvage;

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

    cmd_config = conn_config = live_restore_path = session_config = secretkey = NULL;
    /*
     * We default to returning an error if recovery needs to be run. Generally we expect this to be
     * run after a clean shutdown. The printlog command disables logging entirely. If recovery is
     * needed, the user can specify -R to run recovery.
     */
    rec_config = REC_ERROR;
    backward_compatible = disable_prefetch = logoff = meta_verify = readonly = recover = salvage =
      false;
    /* Check for standard options. */
    __wt_optwt = 1; /* enable WT-specific behavior */
    while ((ch = __wt_getopt(progname, argc, argv, "BC:E:h:l:LmpqRrSVv?")) != EOF)
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
            util_free(secretkey); /* lint: set more than once */
            if ((secretkey = util_strdup(__wt_optarg)) == NULL) {
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
        case 'l':
            live_restore_path = __wt_optarg;
            break;
        case 'm': /* verify metadata on connection open */
            meta_verify = true;
            break;
        case 'p':
            disable_prefetch = true;
            break;
        case 'q':
            read_corrupt = true;
            break;
        case 'R': /* recovery */
            rec_config = REC_RECOVER;
            recover = true;
            break;
        case 'r':
            readonly = true;
            break;
        case 'S': /* salvage */
            salvage = true;
            break;
        case 'V': /* version */
            printf("%s\n", wiredtiger_version(NULL, NULL, NULL));
            goto done;
        case 'v': /* verbose */
            verbose = true;
            break;
        case '?':
            usage();
            goto done;
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
            conn_config = "create";
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
            conn_config = "create";
        } else if (strcmp(command, "loadtext") == 0) {
            func = util_loadtext;
            conn_config = "create";
        }
        break;
    case 'p':
        if (strcmp(command, "page") == 0)
            func = util_page;
        else if (strcmp(command, "printlog") == 0) {
            func = util_printlog;
            rec_config = REC_LOGOFF;
        }
        break;
    case 'r':
        if (strcmp(command, "read") == 0)
            func = util_read;
        break;
    case 's':
        if (strcmp(command, "salvage") == 0)
            func = util_salvage;
        else if (strcmp(command, "stat") == 0) {
            func = util_stat;
            conn_config = "statistics=(all)";
        }
        break;
    case 't':
        if (strcmp(command, "truncate") == 0)
            func = util_truncate;
        break;
    case 'v':
        if (strcmp(command, "verify") == 0) {
            func = util_verify;
            if (disable_prefetch)
                conn_config = "prefetch=(available=false,default=false)";
            else {
                conn_config = "prefetch=(available=true,default=false)";
                session_config = "prefetch=(enabled=true)";
            }
        }
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

    /*
     * -q is only meaningful for read-oriented commands. Reject it on anything else so users get
     * an immediate, clear error rather than a silently ignored flag.
     */
    if (read_corrupt && !util_func_supports_read_corrupt(func)) {
        fprintf(stderr,
          "%s: -q is only valid for read-oriented commands: dump, read, stat (verify has its own "
          "-c flag)\n",
          progname);
        goto err;
    }

open:
    /* Build the configuration string. */
    len = 10; /* some slop */
    p1 = p2 = p3 = "";
    len += strlen("error_prefix=wt");
    if (conn_config != NULL)
        len += strlen(conn_config);
    if (cmd_config != NULL)
        len += strlen(cmd_config);
    len += strlen("live_restore=(enabled=,threads_max=0,path=)");
    if (live_restore_path != NULL)
        len += strlen(live_restore_path) + strlen("true");
    else
        len += strlen("false");
    if (meta_verify)
        len += strlen(VERIFY_METADATA);
    if (readonly)
        len += strlen(READONLY);
    if (salvage)
        len += strlen(SALVAGE);
    if (secretkey != NULL) {
        len += strlen(secretkey) + 30;
        p1 = ",encryption=(secretkey=";
        p2 = secretkey;
        p3 = ")";
    }
    len += strlen(rec_config);
    if ((p = util_malloc(len)) == NULL) {
        (void)util_err(NULL, errno, NULL);
        goto err;
    }
    if ((ret = __wt_snprintf(p, len,
           "error_prefix=wt,%s,%s,live_restore=(enabled=%s,threads_max=0,path=%s),%s,%s,%s,%s%s%s%"
           "s",
           conn_config == NULL ? "" : conn_config, cmd_config == NULL ? "" : cmd_config,
           live_restore_path == NULL ? "false" : "true",
           live_restore_path == NULL ? "" : live_restore_path, meta_verify ? VERIFY_METADATA : "",
           readonly ? READONLY : "", rec_config, salvage ? SALVAGE : "", p1, p2, p3)) != 0) {
        (void)util_err(NULL, ret, NULL);
        goto err;
    }
    conn_config = p;

    if ((ret = wiredtiger_open(home, verbose ? verbose_handler : NULL, conn_config, &conn)) != 0) {
        (void)util_err(NULL, ret, NULL);
        fprintf(stderr,
          "Note: this issue typically arises from running wt util in an incorrect directory or not "
          "specifying one. Ensure you execute wt within a WiredTiger directory, or use the -h flag "
          "to direct it to one.\n");
        goto err;
    }

    if (secretkey != NULL) {
        /* p contains a copy of secretkey, so zero both before freeing */
        wt_explicit_zero(p, strlen(p));
        wt_explicit_zero(secretkey, strlen(secretkey));
    }
    util_free(p);
    util_free(secretkey);
    secretkey = p = NULL;

    /* If we only want to verify the metadata, that is done in wiredtiger_open. We're done. */
    if (func == NULL && meta_verify)
        goto done;

    if ((ret = conn->open_session(conn, NULL, session_config, &session)) != 0) {
        (void)util_err(NULL, ret, NULL);
        goto err;
    }

    if (read_corrupt)
        F_SET((WT_SESSION_IMPL *)session, WT_SESSION_READ_SKIP_CORRUPT);

    if ((ret = util_disagg_pick_up_latest_checkpoint(conn, session)) != 0) {
        (void)util_err(session, ret, "failed to pick up latest disaggregated checkpoint");
        goto err;
    }

    /* Call the function after opening the database and session. */
    ret = func(session, argc, argv);

    /*
     * The block manager sets WT_CONN_DATA_CORRUPTION at every corruption-detection site.
     */
    if (read_corrupt &&
      F_ISSET_ATOMIC_32(S2C((WT_SESSION_IMPL *)session), WT_CONN_DATA_CORRUPTION) && ret == 0)
        ret = WT_ERROR;

    if (0) {
err:
        ret = 1;
    }
done:

    /* may get here via either err or done before the free above happens */
    if (p != NULL) {
        /* p may contain a copy of secretkey, so zero before freeing */
        wt_explicit_zero(p, strlen(p));
        util_free(p);
    }
    if (secretkey != NULL) {
        wt_explicit_zero(secretkey, strlen(secretkey));
        util_free(secretkey);
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
    if ((name = util_calloc(len, 1)) == NULL) {
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
    util_free(name);
    (void)util_err(session, ret, NULL);
    return (NULL);
}
