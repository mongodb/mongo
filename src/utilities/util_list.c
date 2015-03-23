/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int list_print(WT_SESSION *, const char *, int, int);
static int list_print_checkpoint(WT_SESSION *, const char *);
static int usage(void);

int
util_list(WT_SESSION *session, int argc, char *argv[])
{
	WT_DECL_RET;
	int cflag, ch, vflag;
	char *name;

	cflag = vflag = 0;
	name = NULL;
	while ((ch = __wt_getopt(progname, argc, argv, "cv")) != EOF)
		switch (ch) {
		case 'c':
			cflag = 1;
			break;
		case 'v':
			vflag = 1;
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

	if (name != NULL)
		free(name);

	return (ret);
}

/*
 * list_print --
 *	List the high-level objects in the database.
 */
static int
list_print(WT_SESSION *session, const char *name, int cflag, int vflag)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	int found;
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
			found = 1;
		}

		/*
		 * XXX
		 * We don't normally say anything about the WiredTiger
		 * metadata, it's not a normal "object" in the database.  I'm
		 * making an exception for the checkpoint and verbose options.
		 */
		if (strcmp(key, WT_METADATA_URI) != 0 || cflag || vflag)
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
	WT_DECL_RET;
	WT_CKPT *ckpt, *ckptbase;
	size_t len;
	time_t t;
	uint64_t v;

	/*
	 * We may not find any checkpoints for this file, in which case we don't
	 * report an error, and continue our caller's loop.  Otherwise, read the
	 * list of checkpoints and print each checkpoint's name and time.
	 */
	if ((ret = __wt_metadata_get_ckptlist(session, key, &ckptbase)) != 0)
		return (ret == WT_NOTFOUND ? 0 : ret);

	/* Find the longest name, so we can pretty-print. */
	len = 0;
	WT_CKPT_FOREACH(ckptbase, ckpt)
		if (strlen(ckpt->name) > len)
			len = strlen(ckpt->name);
	++len;

	WT_CKPT_FOREACH(ckptbase, ckpt) {
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
