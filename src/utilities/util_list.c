/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int list_get_allocsize(WT_SESSION *, const char *, size_t *);
static int list_print(WT_SESSION *, const char *, bool, bool);
static int list_print_checkpoint(WT_SESSION *, const char *);
static int usage(void);

int
util_list(WT_SESSION *session, int argc, char *argv[])
{
	WT_DECL_RET;
	int ch;
	bool cflag, vflag;
	char *name;

	cflag = vflag = false;
	name = NULL;
	while ((ch = __wt_getopt(progname, argc, argv, "cv")) != EOF)
		switch (ch) {
		case 'c':
			cflag = true;
			break;
		case 'v':
			vflag = true;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= __wt_optind;
	argv += __wt_optind;

	switch (argc) {
	case 0:
		break;
	case 1:
		if ((name = util_name(session, *argv, "table")) == NULL)
			return (1);
		break;
	default:
		return (usage());
	}

	ret = list_print(session, name, cflag, vflag);

	free(name);

	return (ret);
}

/*
 * list_get_allocsize --
 *	Get the allocation size for this file from the metadata.
 */
static int
list_get_allocsize(WT_SESSION *session, const char *key, size_t *allocsize)
{
	WT_CONFIG_ITEM szvalue;
	WT_CONFIG_PARSER *parser;
	WT_DECL_RET;
	WT_EXTENSION_API *wt_api;
	char *config;

	wt_api = session->connection->get_extension_api(session->connection);
	if ((ret =
	    wt_api->metadata_search(wt_api, session, key, &config)) != 0) {
		fprintf(stderr, "%s: %s: extension_api.metadata_search: %s\n",
		    progname, key, session->strerror(session, ret));
		return (ret);
	}
	if ((ret = wt_api->config_parser_open(wt_api, session, config,
	    strlen(config), &parser)) != 0) {
		fprintf(stderr, "%s: extension_api.config_parser_open: %s\n",
		    progname, session->strerror(session, ret));
		return (ret);
	}
	if ((ret = parser->get(parser, "allocation_size", &szvalue)) != 0) {
		if (ret != WT_NOTFOUND)
			fprintf(stderr, "%s: config_parser.get: %s\n",
			    progname, session->strerror(session, ret));
		(void)parser->close(parser);
		return (ret);
	}
	if ((ret = parser->close(parser)) != 0) {
		fprintf(stderr, "%s: config_parser.close: %s\n",
		    progname, session->strerror(session, ret));
		return (ret);
	}
	*allocsize = (size_t)szvalue.val;
	return (0);
}

/*
 * list_print --
 *	List the high-level objects in the database.
 */
static int
list_print(WT_SESSION *session, const char *name, bool cflag, bool vflag)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	bool found;
	const char *key, *value;

	/* Open the metadata file. */
	if ((ret = session->open_cursor(
	    session, WT_METADATA_URI, NULL, NULL, &cursor)) != 0) {
		/*
		 * If there is no metadata (yet), this will return ENOENT.
		 * Treat that the same as an empty metadata.
		 */
		if (ret == ENOENT)
			return (0);

		fprintf(stderr, "%s: %s: session.open_cursor: %s\n",
		    progname, WT_METADATA_URI, session->strerror(session, ret));
		return (1);
	}

	found = name == NULL;
	while ((ret = cursor->next(cursor)) == 0) {
		/* Get the key. */
		if ((ret = cursor->get_key(cursor, &key)) != 0)
			return (util_cerr(cursor, "get_key", ret));

		/*
		 * If a name is specified, only show objects that match.
		 */
		if (name != NULL) {
			if (!WT_PREFIX_MATCH(key, name))
				continue;
			found = true;
		}

		/*
		 * !!!
		 * We don't normally say anything about the WiredTiger metadata
		 * and lookaside tables, they're not application/user "objects"
		 * in the database.  I'm making an exception for the checkpoint
		 * and verbose options.
		 */
		if (cflag || vflag ||
		    (strcmp(key, WT_METADATA_URI) != 0 &&
		    strcmp(key, WT_LAS_URI) != 0))
			printf("%s\n", key);

		if (!cflag && !vflag)
			continue;

		if (cflag && (ret = list_print_checkpoint(session, key)) != 0)
			return (ret);
		if (vflag) {
			if ((ret = cursor->get_value(cursor, &value)) != 0)
				return (util_cerr(cursor, "get_value", ret));
			printf("%s\n", value);
		}
	}
	if (ret != WT_NOTFOUND)
		return (util_cerr(cursor, "next", ret));
	if (!found) {
		fprintf(stderr, "%s: %s: not found\n", progname, name);
		return (1);
	}

	return (0);
}

/*
 * list_print_checkpoint --
 *	List the checkpoint information.
 */
static int
list_print_checkpoint(WT_SESSION *session, const char *key)
{
	WT_BLOCK_CKPT ci;
	WT_DECL_RET;
	WT_CKPT *ckpt, *ckptbase;
	size_t allocsize, len;
	time_t t;
	uint64_t v;

	/*
	 * We may not find any checkpoints for this file, in which case we don't
	 * report an error, and continue our caller's loop.  Otherwise, read the
	 * list of checkpoints and print each checkpoint's name and time.
	 */
	if ((ret = __wt_metadata_get_ckptlist(session, key, &ckptbase)) != 0)
		return (ret == WT_NOTFOUND ? 0 : ret);

	/* We need the allocation size for decoding the checkpoint addr */
	if ((ret = list_get_allocsize(session, key, &allocsize)) != 0) {
		if (ret == WT_NOTFOUND)
			allocsize = 0;
		else
			return (ret);
	}

	/* Find the longest name, so we can pretty-print. */
	len = 0;
	WT_CKPT_FOREACH(ckptbase, ckpt)
		if (strlen(ckpt->name) > len)
			len = strlen(ckpt->name);
	++len;

	memset(&ci, 0, sizeof(ci));
	WT_CKPT_FOREACH(ckptbase, ckpt) {
		if (allocsize != 0 && (ret = __wt_block_ckpt_decode(
		    session, allocsize, ckpt->raw.data, &ci)) != 0) {
			fprintf(stderr, "%s: __wt_block_buffer_to_ckpt: %s\n",
			    progname, session->strerror(session, ret));
			/* continue if damaged */
			ci.root_size = 0;
		}
		/*
		 * Call ctime, not ctime_r; ctime_r has portability problems,
		 * the Solaris version is different from the POSIX standard.
		 */
		t = (time_t)ckpt->sec;
		printf("\t%*s: %.24s", (int)len, ckpt->name, ctime(&t));

		v = ckpt->ckpt_size;
		if (v >= WT_PETABYTE)
			printf(" (%" PRIu64 " PB)\n", v / WT_PETABYTE);
		else if (v >= WT_TERABYTE)
			printf(" (%" PRIu64 " TB)\n", v / WT_TERABYTE);
		else if (v >= WT_GIGABYTE)
			printf(" (%" PRIu64 " GB)\n", v / WT_GIGABYTE);
		else if (v >= WT_MEGABYTE)
			printf(" (%" PRIu64 " MB)\n", v / WT_MEGABYTE);
		else if (v >= WT_KILOBYTE)
			printf(" (%" PRIu64 " KB)\n", v / WT_KILOBYTE);
		else
			printf(" (%" PRIu64 " B)\n", v);
		if (ci.root_size != 0) {
			printf("\t\t" "root offset: %" PRIuMAX
			    " (0x%" PRIxMAX ")\n",
			    (intmax_t)ci.root_offset, (intmax_t)ci.root_offset);
			printf("\t\t" "root size: %" PRIu32
			    " (0x%" PRIx32 ")\n",
			    ci.root_size, ci.root_size);
			printf("\t\t" "root checksum: %" PRIu32
			    " (0x%" PRIx32 ")\n",
			    ci.root_cksum, ci.root_cksum);
		}
	}

	__wt_metadata_free_ckptlist(session, ckptbase);
	return (0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "list [-cv] [uri]\n",
	    progname, usage_prefix);
	return (1);
}
