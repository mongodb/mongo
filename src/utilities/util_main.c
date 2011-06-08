/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

const char *progname;				/* Program name */
const char *usage_prefix = "[-Vv] [-h home]";	/* Global arguments */
int verbose;					/* Verbose flag */

static int usage(void);

int
main(int argc, char *argv[])
{
	char *cmd;
	int ch, major_v,  minor_v;

	/* Get the program name. */
	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

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
	while ((ch = getopt(argc, argv, "h:Vv")) != EOF)
		switch (ch) {
		case 'h':			/* home directory */
			if (chdir(optarg) != 0) {
				fprintf(stderr, "%s: chdir %s: %s\n",
				    progname, optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'V':			/* version */
			printf("%s\n", wiredtiger_version(NULL, NULL, NULL));
			return (EXIT_SUCCESS);
		case 'v':			/* version */
			verbose = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	/* The next argument is the command name. */
	if (argc < 1)
		return (usage());
	cmd = argv[0];

	/* Reset getopt. */
	optreset = 1;
	optind = 1;

	switch (cmd[0]) {
	case 'd':
		if (strcmp(cmd, "dump") == 0)
			return (util_dump(argc, argv));
		break;
	case 'l':
		if (strcmp(cmd, "load") == 0)
			return (util_load(argc, argv));
		break;
	case 'p':
		if (strcmp(cmd, "printlog") == 0)
			return (util_printlog(argc, argv));
		break;
	case 's':
		if (strcmp(cmd, "salvage") == 0)
			return (util_salvage(argc, argv));
		if (strcmp(cmd, "stat") == 0)
			return (util_stat(argc, argv));
		break;
	case 'v':
		if (strcmp(cmd, "verify") == 0)
			return (util_verify(argc, argv));
		break;
	default:
		break;
	}

	return (usage());
}

static int
usage(void)
{
	fprintf(stderr,
	    "WiredTiger Data Engine (version %d.%d)\n", 
	    WIREDTIGER_VERSION_MAJOR, WIREDTIGER_VERSION_MINOR);
	fprintf(stderr,
	    "global options:\n"
	    "\t-h\tdatabase directory\n"
	    "\t-V\tdisplay library version and exit\n"
	    "\t-v\tverbose\n");
	fprintf(stderr,
	    "commands:\n"
	    "\tdump\t  dump a table\n"
	    "\tload\t  load a table\n"
	    "\tprintlog  display the database log\n"
	    "\tsalvage\t  salvage a file\n"
	    "\tstat\t  display statistics for a table\n"
	    "\tverify\t  verify a file\n");

	return (EXIT_FAILURE);
}
