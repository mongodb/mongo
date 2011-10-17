/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

static int str2recno(const char *, uint64_t *);
static int usage(void);

int
util_read(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	uint64_t recno;
	int ch, rkey, ret, rval;
	const char *uri, *value;

	ret = 0;

	while ((ch = util_getopt(argc, argv, "")) != EOF)
		switch (ch) {
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	/* The remaining arguments are a uri followed by a list of keys. */
	if (argc < 2)
		return (usage());
	if ((uri =
	    util_name(*argv, "table", UTIL_FILE_OK | UTIL_TABLE_OK)) == NULL)
		return (1);

	/* Open the object. */
	if ((ret = session->open_cursor(
	    session, uri, NULL, NULL, &cursor)) != 0)
		return (util_err(ret, "%s: session.open", uri));

	/*
	 * A simple search only makes sense if the key format is a string or a
	 * record number, and the value format is a single string.
	 */
	if (strcmp(cursor->key_format, "r") != 0 &&
	    strcmp(cursor->key_format, "S") != 0) {
		fprintf(stderr,
		    "%s: read command only possible when the key format is "
		    "a record number or string\n",
		    progname);
		return (1);
	}
	rkey = strcmp(cursor->key_format, "r") == 0 ? 1 : 0;
	if (strcmp(cursor->value_format, "S") != 0) {
		fprintf(stderr,
		    "%s: read command only possible when the value format is "
		    "a string\n",
		    progname);
		return (1);
	}

	/* Run through the keys. */
	for (rval = 0; *++argv != NULL;) {
		if (rkey) {
			if (str2recno(*argv, &recno))
				return (1);
			cursor->set_key(cursor, recno);
		} else
			cursor->set_key(cursor, *argv);

		switch (ret = cursor->search(cursor)) {
		case 0:
			if ((ret = cursor->get_value(cursor, &value)) != 0)
				return (util_cerr(uri, "get_value", ret));
			if (printf("%s\n", value) < 0)
				return (util_err(EIO, NULL));
			break;
		case WT_NOTFOUND:
			(void)util_err(0, "%s: not found", *argv);
			rval = 1;
			break;
		default:
			return (util_cerr(uri, "search", ret));
		}
	}
		
	return (rval);
}

/*
 * str2recno --
 *	Convert a string to a record number.
 */
static int
str2recno(const char *p, uint64_t *recnop)
{
	uint64_t recno;
	char *endptr;

	/*
	 * strtouq takes lots of things like hex values, signs and so on and so
	 * forth -- none of them are OK with us.  Check the string starts with
	 * digit, that turns off the special processing.
	 */
	if (!isdigit(p[0]))
		goto format;

	errno = 0;
	recno = strtouq(p, &endptr, 0);
	if (recno == ULLONG_MAX && errno == ERANGE)
		return (util_err(ERANGE, "%s: invalid record number", p));

	if (endptr[0] != '\0')
format:		return (util_err(EINVAL, "%s: invalid record number", p));

	*recnop = recno;
	return (0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s %s "
	    "read uri key ...\n",
	    progname, usage_prefix);
	return (1);
}
