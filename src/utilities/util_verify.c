/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int usage(void);

int
util_verify(WT_SESSION *session, int argc, char *argv[])
{
	WT_DECL_RET;
	int ch, dump_address, dump_blocks, dump_pages;
	char *name, config[128];

	name = NULL;
	dump_address = dump_blocks = dump_pages = 0;
	while ((ch = util_getopt(argc, argv, "d:")) != EOF)
		switch (ch) {
		case 'd':
			if (strcmp(util_optarg, "dump_address") == 0)
				dump_address = 1;
			else if (strcmp(util_optarg, "dump_blocks") == 0)
				dump_blocks = 1;
			else if (strcmp(util_optarg, "dump_pages") == 0)
				dump_pages = 1;
			else
				return (usage());
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	/* The remaining argument is the table name. */
	if (argc != 1)
		return (usage());
	if ((name = util_name(*argv,
	    "table", UTIL_FILE_OK | UTIL_LSM_OK | UTIL_TABLE_OK)) == NULL)
		return (1);

	/* Build the configuration string as necessary. */
	config[0] = '\0';
	if (dump_address)
		(void)strcat(config, "dump_address,");
	if (dump_blocks)
		(void)strcat(config, "dump_blocks,");
	if (dump_pages)
		(void)strcat(config, "dump_pages,");

	if ((ret = session->verify(session, name, config)) != 0) {
		fprintf(stderr, "%s: verify(%s): %s\n",
		    progname, name, wiredtiger_strerror(ret));
		goto err;
	}

	/* Verbose configures a progress counter, move to the next line. */
	if (verbose)
		printf("\n");

	if (0) {
err:		ret = 1;
	}

	if (name != NULL)
		free(name);

	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "verify [-d dump_address | dump_blocks | dump_pages] uri\n",
	    progname, usage_prefix);
	return (1);
}
