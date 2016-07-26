/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
	size_t size;
	int ch;
	bool dump_address, dump_blocks, dump_layout, dump_pages;
	char *config, *dump_offsets, *name;

	dump_address = dump_blocks = dump_layout = dump_pages = false;
	config = dump_offsets = name = NULL;
	while ((ch = __wt_getopt(progname, argc, argv, "d:")) != EOF)
		switch (ch) {
		case 'd':
			if (strcmp(__wt_optarg, "dump_address") == 0)
				dump_address = true;
			else if (strcmp(__wt_optarg, "dump_blocks") == 0)
				dump_blocks = true;
			else if (strcmp(__wt_optarg, "dump_layout") == 0)
				dump_layout = true;
			else if (
			    WT_PREFIX_MATCH(__wt_optarg, "dump_offsets=")) {
				if (dump_offsets != NULL) {
					fprintf(stderr,
					    "%s: only a single 'dump_offsets' "
					    "argument supported\n", progname);
					return (usage());
				}
				dump_offsets =
				    __wt_optarg + strlen("dump_offsets=");
			} else if (strcmp(__wt_optarg, "dump_pages") == 0)
				dump_pages = true;
			else
				return (usage());
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= __wt_optind;
	argv += __wt_optind;

	/* The remaining argument is the table name. */
	if (argc != 1)
		return (usage());
	if ((name = util_name(session, *argv, "table")) == NULL)
		return (1);

	/* Build the configuration string as necessary. */
	if (dump_address ||
	    dump_blocks || dump_layout || dump_offsets != NULL || dump_pages) {
		size =
		    strlen("dump_address,") +
		    strlen("dump_blocks,") +
		    strlen("dump_layout,") +
		    strlen("dump_pages,") +
		    strlen("dump_offsets[],") +
		    (dump_offsets == NULL ? 0 : strlen(dump_offsets)) + 20;
		if ((config = malloc(size)) == NULL) {
			(void)util_err(session, errno, NULL);
			goto err;
		}
		snprintf(config, size,
		    "%s%s%s%s%s%s%s",
		    dump_address ? "dump_address," : "",
		    dump_blocks ? "dump_blocks," : "",
		    dump_layout ? "dump_layout," : "",
		    dump_offsets != NULL ? "dump_offsets=[" : "",
		    dump_offsets != NULL ? dump_offsets : "",
		    dump_offsets != NULL ? "]," : "",
		    dump_pages ? "dump_pages," : "");
	}
	if ((ret = session->verify(session, name, config)) != 0) {
		fprintf(stderr, "%s: verify(%s): %s\n",
		    progname, name, session->strerror(session, ret));
		goto err;
	}

	/* Verbose configures a progress counter, move to the next line. */
	if (verbose)
		printf("\n");

	if (0) {
err:		ret = 1;
	}

	free(config);
	free(name);

	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "verify %s\n",
	    progname, usage_prefix,
	    "[-d dump_address | dump_blocks | dump_layout | "
	    "dump_offsets=#,# | dump_pages] uri");
	return (1);
}
