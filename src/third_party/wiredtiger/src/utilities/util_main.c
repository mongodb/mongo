/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

const char *home = ".";				/* Home directory */
const char *progname;				/* Program name */
						/* Global arguments */
const char *usage_prefix = "[-Vv] [-R] [-C config] [-h home]";
int verbose;					/* Verbose flag */

static const char *command;			/* Command name */

#define	REC_ERROR	"log=(recover=error)"
#define	REC_LOGOFF	"log=(enabled=false)"
#define	REC_RECOVER	"log=(recover=on)"

static int usage(void);

int
main(int argc, char *argv[])
{
	WT_CONNECTION *conn;
	WT_DECL_RET;
	WT_SESSION *session;
	size_t len;
	int ch, major_v, minor_v, tret, (*func)(WT_SESSION *, int, char *[]);
	int logoff, recover;
	char *p;
	const char *cmd_config, *config, *rec_config;

	conn = NULL;
	p = NULL;

	/* Get the program name. */
	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;
	command = "";

	/* Check the version against the library build. */
	(void)wiredtiger_version(&major_v, & minor_v, NULL);
	if (major_v != WIREDTIGER_VERSION_MAJOR ||
	    minor_v != WIREDTIGER_VERSION_MINOR) {
		fprintf(stderr,
		    "%s: program build version %d.%d does not match "
		    "library build version %d.%d\n",
		    progname,
		    WIREDTIGER_VERSION_MAJOR, WIREDTIGER_VERSION_MINOR,
		    major_v,  minor_v);
		return (EXIT_FAILURE);
	}

	cmd_config = config = NULL;
	/*
	 * We default to returning an error if recovery needs to be run.
	 * Generally we expect this to be run after a clean shutdown.
	 * The printlog command disables logging entirely.  If recovery is
	 * needed, the user can specify -R to run recovery.
	 */
	rec_config = REC_ERROR;
	logoff = recover = 0;
	/* Check for standard options. */
	while ((ch = __wt_getopt(progname, argc, argv, "C:h:LRVv")) != EOF)
		switch (ch) {
		case 'C':			/* wiredtiger_open config */
			cmd_config = __wt_optarg;
			break;
		case 'h':			/* home directory */
			home = __wt_optarg;
			break;
		case 'L':			/* no logging */
			rec_config = REC_LOGOFF;
			logoff = 1;
			break;
		case 'R':			/* recovery */
			rec_config = REC_RECOVER;
			recover = 1;
			break;
		case 'V':			/* version */
			printf("%s\n", wiredtiger_version(NULL, NULL, NULL));
			return (EXIT_SUCCESS);
		case 'v':			/* verbose */
			verbose = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	if (logoff && recover) {
		fprintf(stderr, "Only one of -L and -R is allowed.\n");
		return (EXIT_FAILURE);
	}
	argc -= __wt_optind;
	argv += __wt_optind;

	/* The next argument is the command name. */
	if (argc < 1)
		return (usage());
	command = argv[0];

	/* Reset getopt. */
	__wt_optreset = __wt_optind = 1;

	func = NULL;
	switch (command[0]) {
	case 'b':
		if (strcmp(command, "backup") == 0)
			func = util_backup;
		break;
	case 'c':
		if (strcmp(command, "compact") == 0)
			func = util_compact;
		else if (strcmp(command, "copyright") == 0) {
			util_copyright();
			return (EXIT_SUCCESS);
		} else if (strcmp(command, "create") == 0) {
			func = util_create;
			config = "create";
		}
		break;
	case 'd':
		if (strcmp(command, "drop") == 0)
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
	if (func == NULL)
		return (usage());

	/* Build the configuration string. */
	len = 10;					/* some slop */
	if (config != NULL)
		len += strlen(config);
	if (cmd_config != NULL)
		len += strlen(cmd_config);
	len += strlen(rec_config);
	if ((p = malloc(len)) == NULL) {
		ret = util_err(NULL, errno, NULL);
		goto err;
	}
	(void)snprintf(p, len, "%s,%s,%s",
	    config == NULL ? "" : config,
	    cmd_config == NULL ? "" : cmd_config, rec_config);
	config = p;

	/* Open the database and a session. */
	if ((ret = wiredtiger_open(home,
	    verbose ? verbose_handler : NULL, config, &conn)) != 0) {
		ret = util_err(NULL, ret, NULL);
		goto err;
	}
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		ret = util_err(NULL, ret, NULL);
		goto err;
	}

	/* Call the function. */
	ret = func(session, argc, argv);

	/* Close the database. */

err:	if (conn != NULL && (tret = conn->close(conn, NULL)) != 0 && ret == 0)
		ret = tret;

	if (p != NULL)
		free(p);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int
usage(void)
{
	fprintf(stderr,
	    "WiredTiger Data Engine (version %d.%d)\n",
	    WIREDTIGER_VERSION_MAJOR, WIREDTIGER_VERSION_MINOR);
	fprintf(stderr,
	    "global options:\n"
	    "\t" "-C\t" "wiredtiger_open configuration\n"
	    "\t" "-h\t" "database directory\n"
	    "\t" "-L\t" "turn logging off for debug-mode\n"
	    "\t" "-R\t" "run recovery if configured\n"
	    "\t" "-V\t" "display library version and exit\n"
	    "\t" "-v\t" "verbose\n");
	fprintf(stderr,
	    "commands:\n"
	    "\t" "backup\t  database backup\n"
	    "\t" "compact\t  compact an object\n"
	    "\t" "copyright copyright information\n"
	    "\t" "create\t  create an object\n"
	    "\t" "drop\t  drop an object\n"
	    "\t" "dump\t  dump an object\n"
	    "\t" "list\t  list database objects\n"
	    "\t" "load\t  load an object\n"
	    "\t" "loadtext\t  load an object from a text file\n"
	    "\t" "printlog  display the database log\n"
	    "\t" "read\t  read values from an object\n"
	    "\t" "rename\t  rename an object\n"
	    "\t" "salvage\t  salvage a file\n"
	    "\t" "stat\t  display statistics for an object\n"
	    "\t" "upgrade\t  upgrade an object\n"
	    "\t" "verify\t  verify an object\n"
	    "\t" "write\t  write values to an object\n");

	return (EXIT_FAILURE);
}

/*
 * util_name --
 *	Build a name.
 */
char *
util_name(WT_SESSION *session, const char *s, const char *type)
{
	size_t len;
	char *name;

	if (WT_PREFIX_MATCH(s, "backup:") ||
	    WT_PREFIX_MATCH(s, "config:") ||
	    WT_PREFIX_MATCH(s, "statistics:")) {
		fprintf(stderr,
		    "%s: %s: unsupported object type: %s\n",
		    progname, command, s);
		return (NULL);
	}

	len = strlen(type) + strlen(s) + 2;
	if ((name = calloc(len, 1)) == NULL) {
		(void)util_err(session, errno, NULL);
		return (NULL);
	}

	/*
	 * If the string has a URI prefix, use it verbatim, otherwise prepend
	 * the default type for the operation.
	 */
	if (strchr(s, ':') != NULL)
		strcpy(name, s);
	else
		snprintf(name, len, "%s:%s", type, s);
	return (name);
}
