/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

const char *home = ".";				/* Home directory */
const char *progname;				/* Program name */
						/* Global arguments */
const char *usage_prefix = "[-Vv] [-C config] [-h home]";
int verbose;					/* Verbose flag */

static const char *command;			/* Command name */

static int usage(void);

int
main(int argc, char *argv[])
{
	WT_CONNECTION *conn;
	WT_DECL_RET;
	WT_SESSION *session;
	int ch, major_v, minor_v, tret;
	const char *config;

	conn = NULL;

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

	/* Check for standard options. */
	config = NULL;
	while ((ch = util_getopt(argc, argv, "C:h:Vv")) != EOF)
		switch (ch) {
		case 'C':			/* wiredtiger_open config */
			config = util_optarg;
			break;
		case 'h':			/* home directory */
			home = util_optarg;
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
	argc -= util_optind;
	argv += util_optind;

	/* The next argument is the command name. */
	if (argc < 1)
		return (usage());
	command = argv[0];

	/* Reset getopt. */
	util_optreset = 1;
	util_optind = 1;

	/* The copyright option doesn't require a database. */
	switch (command[0]) {
	case 'c':
		if (strcmp(command, "copyright") == 0) {
			util_copyright();
			return (EXIT_SUCCESS);
		}
		break;
	}

	/* The "create" and "load" commands can create the database. */
	if (config == NULL &&
	    (strcmp(command, "create") == 0 || strcmp(command, "load") == 0))
		config = "create";

	if ((ret = wiredtiger_open(home,
	    verbose ? verbose_handler : NULL, config, &conn)) != 0)
		goto err;
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		goto err;

	switch (command[0]) {
	case 'b':
		if (strcmp(command, "backup") == 0)
			ret = util_backup(session, argc, argv);
		else
			ret = usage();
		break;
	case 'c':
		if (strcmp(command, "create") == 0)
			ret = util_create(session, argc, argv);
		else
			ret = usage();
		break;
	case 'd':
		if (strcmp(command, "drop") == 0)
			ret = util_drop(session, argc, argv);
		else if (strcmp(command, "dump") == 0)
			ret = util_dump(session, argc, argv);
		else if (strcmp(command, "dumpfile") == 0)
			ret = util_dumpfile(session, argc, argv);
		else
			ret = usage();
		break;
	case 'l':
		if (strcmp(command, "list") == 0)
			ret = util_list(session, argc, argv);
		else if (strcmp(command, "load") == 0)
			ret = util_load(session, argc, argv);
		else if (strcmp(command, "loadtext") == 0)
			ret = util_loadtext(session, argc, argv);
		else
			ret = usage();
		break;
	case 'p':
		if (strcmp(command, "printlog") == 0)
			ret = util_printlog(session, argc, argv);
		else
			ret = usage();
		break;
	case 'r':
		if (strcmp(command, "read") == 0)
			ret = util_read(session, argc, argv);
		else if (strcmp(command, "rename") == 0)
			ret = util_rename(session, argc, argv);
		else
			ret = usage();
		break;
	case 's':
		if (strcmp(command, "salvage") == 0)
			ret = util_salvage(session, argc, argv);
		else if (strcmp(command, "stat") == 0)
			ret = util_stat(session, argc, argv);
		else
			ret = usage();
		break;
	case 'v':
		if (strcmp(command, "verify") == 0)
			ret = util_verify(session, argc, argv);
		else
			ret = usage();
		break;
	case 'u':
		if (strcmp(command, "upgrade") == 0)
			ret = util_upgrade(session, argc, argv);
		else
			ret = usage();
		break;
	case 'w':
		if (strcmp(command, "write") == 0)
			ret = util_write(session, argc, argv);
		else
			ret = usage();
		break;
	default:
		ret = usage();
		break;
	}

err:	if (conn != NULL && (tret = conn->close(conn, NULL)) != 0 && ret == 0)
		ret = tret;

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
	    "\t-C\twiredtiger_open configuration\n"
	    "\t-h\tdatabase directory\n"
	    "\t-V\tdisplay library version and exit\n"
	    "\t-v\tverbose\n");
	fprintf(stderr,
	    "commands:\n"
	    "\tbackup database backup\n"
	    "\tcopyright copyright information\n"
	    "\tcreate\t  create an object\n"
	    "\tdrop\t  drop an object\n"
	    "\tdump\t  dump an object\n"
	    "\tdumpfile  dump a physical file in debugging format\n"
	    "\tlist\t  list database objects\n"
	    "\tload\t  load an object\n"
	    "\tprintlog  display the database log\n"
	    "\tread\t  read values from an object\n"
	    "\trename\t  rename an object\n"
	    "\tsalvage\t  salvage a file\n"
	    "\tstat\t  display statistics for an object\n"
	    "\tupgrade\t  upgrade an object\n"
	    "\tverify\t  verify an object\n"
	    "\twrite\t  write values to an object\n");

	return (EXIT_FAILURE);
}

/*
 * util_name --
 *	Build a name.
 */
char *
util_name(const char *s, const char *type, u_int flags)
{
	size_t len;
	int copy;
	char *name;

	copy = 0;
	if (WT_PREFIX_MATCH(s, "colgroup:")) {
		if (!(flags & UTIL_COLGROUP_OK)) {
			fprintf(stderr,
			    "%s: %s: \"colgroup\" type not supported\n",
			    progname, command);
			return (NULL);
		}
		copy = 1;
	} else if (WT_PREFIX_MATCH(s, "file:")) {
		if (!(flags & UTIL_FILE_OK)) {
			fprintf(stderr,
			    "%s: %s: \"file\" type not supported\n",
			    progname, command);
			return (NULL);
		}
		copy = 1;
	} else if (WT_PREFIX_MATCH(s, "index:")) {
		if (!(flags & UTIL_INDEX_OK)) {
			fprintf(stderr,
			    "%s: %s: \"index\" type not supported\n",
			    progname, command);
			return (NULL);
		}
		copy = 1;
	} else if (WT_PREFIX_MATCH(s, "table:")) {
		if (!(flags & UTIL_TABLE_OK)) {
			fprintf(stderr,
			    "%s: %s: \"table\" type not supported\n",
			    progname, command);
			return (NULL);
		}
		copy = 1;
	}

	len = strlen(type) + strlen(s) + 2;
	if ((name = calloc(len, 1)) == NULL) {
		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
		return (NULL);
	}

	if (copy)
		strcpy(name, s);
	else
		snprintf(name, len, "%s:%s", type, s);
	return (name);
}
