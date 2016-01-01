/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int copy(const char *, const char *);
static int usage(void);

#define	CBUF_LEN	(128 * 1024)		/* Copy buffer and size. */
static char *cbuf;

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
	if (remain < strlen(target) + 20) {
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
		if ((ret = copy(name, directory)) != 0)
			goto err;
	if (ret == WT_NOTFOUND)
		ret = 0;

	if (ret != 0) {
		fprintf(stderr, "%s: cursor next(backup:) failed: %s\n",
		    progname, session->strerror(session, ret));
		goto err;
	}

err:	free(config);
	free(cbuf);

	return (ret);
}

static int
copy(const char *name, const char *directory)
{
	WT_DECL_RET;
	ssize_t n;
	int ifd, ofd;

	ret = 1;
	ifd = ofd = -1;

	if (verbose &&
	    printf("Backing up %s/%s to %s\n", home, name, directory) < 0) {
		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
		return (1);
	}

	/* Allocate a large copy buffer (use it to build pathnames as well. */
	if (cbuf == NULL && (cbuf = malloc(CBUF_LEN)) == NULL)
		goto memerr;

	/* Open the read file. */
	if (snprintf(cbuf, CBUF_LEN, "%s/%s", home, name) >= CBUF_LEN)
		goto memerr;
	if ((ifd = open(cbuf, O_BINARY | O_RDONLY, 0)) < 0)
		goto readerr;

	/* Open the write file. */
	if (snprintf(cbuf, CBUF_LEN, "%s/%s", directory, name) >= CBUF_LEN)
		goto memerr;
	if ((ofd = open(
	    cbuf, O_BINARY | O_CREAT | O_WRONLY | O_TRUNC, 0666)) < 0)
		goto writerr;

	/* Copy the file. */
	while ((n = read(ifd, cbuf, CBUF_LEN)) > 0)
		if (write(ofd, cbuf, (size_t)n) != n)
			goto writerr;
	if (n != 0)
		goto readerr;

	/*
	 * Close file descriptors (forcing a flush on the write side), and
	 * check for any errors.
	 */
	ret = close(ifd);
	ifd = -1;
	if (ret != 0)
		goto readerr;

	/*
	 * We need to know this file was successfully written, it's a backup.
	 */
#ifdef _WIN32
	if (FlushFileBuffers((HANDLE)_get_osfhandle(ofd)) == 0) {
		DWORD err = GetLastError();
		ret = err;
		goto writerr;
	}
#else
	if (fsync(ofd))
		goto writerr;
#endif
	ret = close(ofd);
	ofd = -1;
	if (ret != 0)
		goto writerr;

	/* Success. */
	ret = 0;

	if (0) {
readerr:	fprintf(stderr,
		    "%s: %s/%s: %s\n", progname, home, name, strerror(errno));
	}
	if (0) {
writerr:	fprintf(stderr, "%s: %s/%s: %s\n",
		    progname, directory, name, strerror(errno));
	}
	if (0) {
memerr:		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
	}

	if (ifd >= 0)
		(void)close(ifd);
	if (ofd >= 0)
		(void)close(ofd);

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
