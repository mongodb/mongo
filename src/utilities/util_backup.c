/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int copy(WT_SESSION *, const char *, const char *);
static int usage(void);

/*
 * append_target --
 *	Build a list of comma-separated targets.
 */
static int
append_target(WT_SESSION *session, const char *target, char **bufp)
{
	static bool first = true;
	static size_t len = 0, remain = 0;
	static char *buf = NULL;

						/* 20 bytes of slop */
	if (buf == NULL || remain < strlen(target) + 20) {
		len += strlen(target) + 512;
		remain += strlen(target) + 512;
		if ((buf = realloc(buf, len)) == NULL)
			return (util_err(session, errno, NULL));
		*bufp = buf;
	}
	if (first) {
		first = false;
		strcpy(buf, "target=(");
	} else
		buf[strlen(buf) - 1] = ',';	/* overwrite previous ")" */
	strcat(buf, "\"");
	strcat(buf, target);
	strcat(buf, "\")");
	remain -= strlen(target) + 1;

	return (0);
}

int
util_backup(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	int ch;
	char *config;
	const char *directory, *name;

	config = NULL;
	while ((ch = __wt_getopt(progname, argc, argv, "t:")) != EOF)
		switch (ch) {
		case 't':
			if (append_target(session, __wt_optarg, &config))
				return (1);
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= __wt_optind;
	argv += __wt_optind;

	if (argc != 1) {
		(void)usage();
		goto err;
	}
	directory = *argv;

	if ((ret = session->open_cursor(
	    session, "backup:", NULL, config, &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(backup:) failed: %s\n",
		    progname, session->strerror(session, ret));
		goto err;
	}

	/* Copy the files. */
	while (
	    (ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_key(cursor, &name)) == 0)
		if ((ret = copy(session, directory, name)) != 0)
			goto err;
	if (ret == WT_NOTFOUND)
		ret = 0;

	if (ret != 0) {
		fprintf(stderr, "%s: cursor next(backup:) failed: %s\n",
		    progname, session->strerror(session, ret));
		goto err;
	}

err:	free(config);
	return (ret);
}

static int
copy(WT_SESSION *session, const char *directory, const char *name)
{
	WT_DECL_RET;
	size_t len;
	char *to;

	to = NULL;

	/* Build the target pathname. */
	len = strlen(directory) + strlen(name) + 2;
	if ((to = malloc(len)) == NULL)
		goto memerr;
	(void)snprintf(to, len, "%s/%s", directory, name);

	if (verbose && printf("Backing up %s/%s to %s\n", home, name, to) < 0) {
		fprintf(stderr, "%s: %s\n", progname, strerror(EIO));
		goto err;
	}

	/*
	 * Use WiredTiger to copy the file: ensuring stability of the copied
	 * file on disk requires care, and WiredTiger knows how to do it.
	 */
	if ((ret = __wt_copy_and_sync(session, name, to)) != 0)
		fprintf(stderr, "%s/%s to %s: backup copy: %s\n",
		    home, name, to, session->strerror(session, ret));

	if (0) {
memerr:		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
	}
err:	free(to);

	return (ret);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "backup [-t uri] directory\n",
	    progname, usage_prefix);
	return (1);
}
