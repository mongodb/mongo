/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int usage(void);

#undef OPT_ARGS
#undef USAGE_ARGS
#ifdef HAVE_DIAGNOSTIC
#define	OPT_ARGS	"d:"
#define	USAGE_ARGS	\
	"[-d dump_address | dump_blocks | dump_offsets=#,# | dump_pages] uri"
#else
#define	OPT_ARGS	""
#define	USAGE_ARGS	"uri"
#endif

int
util_verify(WT_SESSION *session, int argc, char *argv[])
{
	WT_DECL_RET;
	size_t size;
	int ch, dump_address, dump_blocks, dump_pages;
	char *config, *dump_offsets, *name;

	dump_address = dump_blocks = dump_pages = 0;
	config = dump_offsets = name = NULL;
	while ((ch = __wt_getopt(progname, argc, argv, OPT_ARGS)) != EOF)
		switch (ch) {
		case 'd':
			if (strcmp(__wt_optarg, "dump_address") == 0)
				dump_address = 1;
			else if (strcmp(__wt_optarg, "dump_blocks") == 0)
				dump_blocks = 1;
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
				dump_pages = 1;
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
	if ((name = util_name(*argv, "table")) == NULL)
		return (1);

	/* Build the configuration string as necessary. */
	if (dump_address || dump_blocks || dump_offsets != NULL || dump_pages) {
		size =
		    strlen("dump_address,") +
		    strlen("dump_blocks,") +
		    strlen("dump_pages,") +
		    strlen("dump_offsets[],") +
		    (dump_offsets == NULL ? 0 : strlen(dump_offsets)) + 20;
		if ((config = malloc(size)) == NULL) {
			(void)util_err(errno, NULL);
			goto err;
		}
		snprintf(config, size,
		    "%s%s%s%s%s%s",
		    dump_address ? "dump_address," : "",
		    dump_blocks ? "dump_blocks," : "",
		    dump_offsets != NULL ? "dump_offsets=[" : "",
		    dump_offsets != NULL ? dump_offsets : "",
		    dump_offsets != NULL ? "]," : "",
		    dump_pages ? "dump_pages" : "");
	}
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

	if (config != NULL)
		free(config);
	if (name != NULL)
		free(name);

	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "verify %s\n",
	    progname, usage_prefix, USAGE_ARGS);
	return (1);
}
