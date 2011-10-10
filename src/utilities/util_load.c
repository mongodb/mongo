/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

static int format(void);
static int load_col(WT_CURSOR *, const char *, int);
static int load_file(WT_SESSION *, const char *, const char *, int, int);
static int load_row(WT_CURSOR *, const char *);
static int load_table(WT_SESSION *);
static int read_line(WT_BUF *, int, int *);
static int read_schema(char ***, int *);
static int str2recno(const char *, uint64_t *);
static int usage(void);

int
util_load(WT_SESSION *session, int argc, char *argv[])
{
	int append, ch, ret, read_recno;
	const char *config;
	char *name;

	append = read_recno = ret = 0;
	config = NULL;
	name = NULL;

	while ((ch = util_getopt(argc, argv, "ac:f:R")) != EOF)
		switch (ch) {
		case 'a':	/* append */
			append = 1;
			break;
		case 'c':	/* command-line table configuration option */
			config = util_optarg;
			break;
		case 'f':	/* input file */
			if (freopen(util_optarg, "r", stdin) == NULL) {
				fprintf(stderr, "%s: %s: reopen: %s\n",
				    progname, util_optarg, strerror(errno));
				return (1);
			}
			break;
		case 'R':
			read_recno = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	/*
	 * If we have another argument, the user is loading a single file or
	 * table, otherwise, we're reading a WiredTiger dump.
	 */
	if (argc == 0) {
		if (append || config != NULL || read_recno) {
			fprintf(stderr,
			    "%s: the -a, -c and  -R options to the load "
			    "command do not apply when loading WiredTiger "
			    "dumps\n",
			    progname);
			return (1);
		}
		ret = load_table(session);
	} else {
		if ((name = util_name(*argv, "file", UTIL_FILE_OK)) == NULL)
			goto err;

		if (append && config != NULL) {
			fprintf(stderr,
			    "%s: the -a option to the load command causes the "
			    "-c option to be ignored\n",
			    progname);
			return (1);
		}
		ret = load_file(session, name, config, read_recno, append);
	}

	if (0) {
err:		ret = 1;
	}

	if (name != NULL)
		free(name);

	return (ret);
}

/*
 * load_table --
 *	Load a table.
 */
static int
load_table(WT_SESSION *session)
{
	char **list, **p, *tmp;
	int hex, ret;

	if ((ret = read_schema(&list, &hex)) != 0)
		return (ret);

	/* Find the table name, and remove it from the database. */
	for (p = list; *p != NULL; ++p)
		if (strncmp(*p, "table:", strlen("table:")) == 0)
			break;
	if (*p == NULL)
		return (format());
	if ((ret = session->drop(session, *p, "force")) != 0) {
		fprintf(stderr, "%s: session.drop: %s: %s\n", 
		    progname, *p, wiredtiger_strerror(ret));
		return (1);
	}

	/*
	 * Make sure the table key/value pair comes first, then we can just
	 * run through the array in order.  (We already checked that we had
	 * a multiple of 2 entries, so this is safe.)
	 */
	if (p != list) {
		tmp = list[0]; list[0] = p[0]; p[0] = tmp;
		tmp = list[1]; list[1] = p[1]; p[1] = tmp;
	}
	for (p = list; *p != NULL; p += 2)
		if ((ret = session->create(session, p[0], p[1])) != 0) {
			fprintf(stderr, "%s: session.create: %s: %s\n",
			    progname, p[0], wiredtiger_strerror(errno));
			return (1);
		}

	return (0);
}

/*
 * read_schema --
 *	Read the schema lines and do some basic validation.
 */
static int
read_schema(char ***listp, int *hexp)
{
	int entry, max_entry;
	const char *s;
	char *p, **list, buf[4 * 1024];

	/* Header line #1: "WiredTiger Dump" and a WiredTiger version. */
	if (fgets(buf, sizeof(buf), stdin) == NULL)
		return (format());
	s = "WiredTiger Dump ";
	if (strncmp(buf, s, strlen(s)) != 0)
		return (format());

	/* Header line #2: "Format={hex,print}\n". */
	if (fgets(buf, sizeof(buf), stdin) == NULL)
		return (format());
	if (strcmp(buf, "Format=print\n") != 0) {
		if (strcmp(buf, "Format=hex\n") != 0)
			return (format());
		*hexp = 1;
	}

	/* Header line #3: "Header\n". */
	if (fgets(buf, sizeof(buf), stdin) == NULL)
		return (format());
	if (strcmp(buf, "Header\n") != 0)
		return (format());

	/* Now, read in lines until we get to the end of the headers. */
	for (entry = max_entry = 0, list = NULL;; ++entry) {
		if (fgets(buf, sizeof(buf), stdin) == NULL)
			return (format());
		if (strcmp(buf, "Data\n") == 0)
			break;

		/* Grow the array of header lines as necessary. */
		if ((max_entry == 0 || entry == max_entry - 1) &&
		    (list = realloc(list,
		    (size_t)(max_entry += 100) * sizeof(char *))) == NULL)
			goto err;
		if ((list[entry] = strdup(buf)) == NULL)
			goto err;
		if ((p = strchr(list[entry], '\n')) == NULL)
			return (format());
		*p = '\0';
	}
	list[entry] = NULL;
	*listp = list;

	/* The lines are supposed to be in pairs. */
	if (entry / 2 == 0)
		return (format());

	/* Leak the memory, I don't care. */
	return (0);

err:	fprintf(stderr, "%s: %s\n", progname, wiredtiger_strerror(errno));
	return (1);
}

/*
 * format --
 *	The input doesn't match the dump format.
 */
static int
format(void)
{
	fprintf(stderr,
	    "%s: input does not match WiredTiger dump format\n", progname);
	return (1);
}

/*
 * load_file --
 *	Load a single file.
 */
static int
load_file(WT_SESSION *session,
    const char *name, const char *config, int read_recno, int append)
{
	WT_CURSOR *cursor;
	int ret;
	const char *fmt, *p;

	/* Optionally remove and re-create the file. */
	if (!append) {
		if ((ret = session->drop(session, name, "force")) != 0) {
			fprintf(stderr, "%s: session.drop: %s: %s\n", 
			    progname, name, wiredtiger_strerror(ret));
			return (1);
		}
		if ((ret = session->create(session, name, config)) != 0) {
			fprintf(stderr, "%s: session.create: %s: %s\n", 
			    progname, name, wiredtiger_strerror(ret));
			return (1);
		}
	}

	/* Open the insert cursor. */
	if ((ret = session->open_cursor(
	    session, name, NULL, "overwrite,raw", &cursor)) != 0) {
		fprintf(stderr, "%s: session.open: %s: %s\n", 
		    progname, name, wiredtiger_strerror(ret));
		return (1);
	}

	/*
	 * Find out if we're loading a column-store file.  If read_recno is set,
	 * it's obvious, otherwise we check the table configuation for a record
	 * number format.
	 */
	fmt = "key_format=r";
	if (read_recno ||
	    (config != NULL &&
	    (p = strstr(config, fmt)) != NULL &&
	    (p[strlen(fmt)] == '\0' || p[strlen(fmt)] == ',')))
		return (load_col(cursor, name, read_recno));

	return (load_row(cursor, name));
}

/*
 * load_col --
 *	Load a single column-store file.
 */
static int
load_col(WT_CURSOR *cursor, const char *name, int read_recno)
{
	WT_BUF key, value;
	uint64_t insert_count, recno;
	int eof, ret;

	memset(&key, 0, sizeof(key));
	memset(&value, 0, sizeof(value));
	
	/* Read key/value pairs and insert them into the file. */
	for (insert_count = 0;;) {
		if (read_recno) {
			if (read_line(&key, 1, &eof))
				return (1);
			if (eof == 1)
				break;
			if (str2recno(key.data, &recno))
				return (1);
			key.data = &recno;
			key.size = sizeof(recno);
			cursor->set_key(cursor, &key);
		}
		if (read_line(&value, read_recno ? 0 : 1, &eof))
			return (1);
		if (eof == 1)
			break;

		/* Report on progress every 100 inserts. */
		if (verbose && ++insert_count % 100 == 0) {
			printf("\r\t%s: %" PRIu64, name, insert_count);
			fflush(stdout);
		}
	
		cursor->set_value(cursor, &value);
		if ((ret = cursor->insert(cursor)) != 0) {
			fprintf(stderr,
			    "%s: %s: cursor.insert: %s\n",
			    progname, name, wiredtiger_strerror(ret));
			return (1);
		}
	}

	if (verbose)
		printf("\r\t%s: %" PRIu64 "\n", name, insert_count);

	return (0);
}

/*
 * load_row --
 *	Load a single row-store file.
 */
static int
load_row(WT_CURSOR *cursor, const char *name)
{
	WT_BUF key, value;
	uint64_t insert_count;
	int eof, ret;

	memset(&key, 0, sizeof(key));
	memset(&value, 0, sizeof(value));
	
	/* Read key/value pairs and insert them into the file. */
	for (insert_count = 0;;) {
		if (read_line(&key, 1, &eof))
			return (1);
		if (eof == 1)
			break;
		if (read_line(&value, 0, &eof))
			return (1);

		/* Report on progress every 100 inserts. */
		if (verbose && ++insert_count % 100 == 0) {
			printf("\r\t%s: %" PRIu64, name, insert_count);
			fflush(stdout);
		}
	
		cursor->set_key(cursor, &key);
		cursor->set_value(cursor, &value);
		if ((ret = cursor->insert(cursor)) != 0) {
			fprintf(stderr,
			    "%s: %s: cursor.insert: %s\n",
			    progname, name, wiredtiger_strerror(ret));
			return (1);
		}
	}

	if (verbose)
		printf("\r\t%s: %" PRIu64 "\n", name, insert_count);

	return (0);
}

/*
 * read_line --
 *	Read a line from stdin into a WT_ITEM.
 */
static int
read_line(WT_BUF *l, int eof_expected, int *eofp)
{
	static unsigned long long line = 0;
	uint32_t len;
	int ch;

	++line;
	*eofp = 0;

	for (len = 0;; ++len) {
		if ((ch = getchar()) == EOF) {
			if (len == 0) {
				if (eof_expected) {
					*eofp = 1;
					return (0);
				}
				fprintf(stderr,
				    "%s: line %llu: unexpected end-of-file\n",
				    progname, line);
				return (1);
			}
			fprintf(stderr,
			    "%s: line %llu: no newline terminator\n",
			    progname, line);
			return (1);
		}
		if (ch == '\n')
			break;
		/*
		 * We nul-terminate the string so it's easier to convert the
		 * line into a record number, that means we always need one
		 * extra byte at the end.
		 */
		if (l->memsize == 0 || len >= l->memsize - 1) {
			if ((l->mem =
			    realloc(l->mem, l->memsize + 1024)) == NULL) {
				fprintf(stderr, "%s: %s\n",
				    progname, wiredtiger_strerror(errno));
				return (1);
			}
			l->memsize += 1024;
		}
		((uint8_t *)l->mem)[len] = (uint8_t)ch;
	}

	((uint8_t *)l->mem)[len] = '\0';		/* nul-terminate */

	l->data = l->mem;
	l->size = len;
	return (0);
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
	if (recno == ULLONG_MAX && errno == ERANGE) {
		fprintf(stderr,
		    "%s: %s: %s\n", progname, p, wiredtiger_strerror(ERANGE));
		return (1);
	}
	if (endptr[0] != '\0') {
format:		fprintf(stderr,
		    "%s: %s: invalid record number format\n", progname, p);
		return (1);
	}
	*recnop = recno;
	return (0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s%s "
	    "load [-aR] [-c table-config] [-f input-file] uri\n",
	    progname, usage_prefix);
	return (1);
}
