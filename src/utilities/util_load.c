/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

static int format(void);
static int load_data(WT_CURSOR *, const char *);
static int load_dump(WT_SESSION *);
static int read_line(WT_BUF *, int, int *);
static int schema_read(char ***, int *);
static int schema_update(char **);
static int usage(void);

static int	append;		/* -a append (ignore record number keys) */
static int	safe;		/* -s safe (don't overwrite existing data) */
static char    *cmdname;	/* -n name (object's name, or rename) */
static char   **cmdconfig;	/* configuration pairs */

int
util_load(WT_SESSION *session, int argc, char *argv[])
{
	int ch;

	while ((ch = util_getopt(argc, argv, "af:n:s")) != EOF)
		switch (ch) {
		case 'a':	/* append (ignore record number keys) */
			append = 1;
			break;
		case 'f':	/* input file */
			if (freopen(util_optarg, "r", stdin) == NULL) {
				fprintf(stderr, "%s: %s: reopen: %s\n",
				    progname, util_optarg, strerror(errno));
				return (1);
			}
			break;
		case 'n':	/* -n name (object's name, or rename) */
			cmdname = util_optarg;
			break;
		case 's':	/* safe (don't overwrite existing data) */
			safe = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= util_optind;
	argv += util_optind;

	/* The remaining arguments are configuration uri/string pairs. */
	if (argc != 0) {
		if (argc % 2 != 0)
			return (usage());
		cmdconfig = argv;
	}

	return (load_dump(session));
}

/*
 * load_dump --
 *	Load from the WiredTiger dump format.
 */
static int
load_dump(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int hex, ret, tret;
	char **entry, **list, *p, *uri, curconfig[64];

	/* Read the schema file. */
	if ((ret = schema_read(&list, &hex)) != 0)
		return (ret);

	/*
	 * Search for a table name -- if we find one, then it's table dump,
	 * otherwise, it's a single file dump.
	 */
	for (entry = list; *entry != NULL; ++entry)
		if (strncmp(*entry, "table:", strlen("table:")) == 0)
			break;
	if (*entry == NULL) {
		/*
		 * Single file dumps can only have two lines, the file name and
		 * the configuration information.
		 */
		if (list[0] == NULL || list[1] == NULL || list[2] != NULL ||
		    strncmp(list[0], "file:", strlen("file:")) != 0)
			return (format());

		entry = list;
	}

	/*
	 * Make sure the table key/value pair comes first, then we can just
	 * run through the array in order.  (We already checked that we had
	 * a multiple of 2 entries, so this is safe.)
	 */
	if (entry != list) {
		p = list[0]; list[0] = entry[0]; entry[0] = p;
		p = list[1]; list[1] = entry[1]; entry[1] = p;
	}

	/* Update the schema based on any command-line configuration. */
	if ((ret = schema_update(list)) != 0)
		return (ret);

	uri = list[0];
	for (entry = list; *entry != NULL; entry += 2)
		if ((ret = session->create(session, entry[0], entry[1])) != 0) {
			fprintf(stderr, "%s: session.create: %s: %s\n",
			    progname, entry[0], wiredtiger_strerror(ret));
			return (1);
		}

	/* Open the insert cursor. */
	(void)snprintf(curconfig, sizeof(curconfig),
	    "dump=%s,%s", hex ? "hex" : "print", safe ? "" : "overwrite");
	if ((ret = session->open_cursor(
	    session, uri, NULL, curconfig, &cursor)) != 0) {
		fprintf(stderr, "%s: session.open: %s: %s\n", 
		    progname, uri, wiredtiger_strerror(ret));
		return (1);
	}

	/*
	 * Check the append flag (it only applies to objects where the primary
	 * key is a record number).
	 */
	if (append && strcmp(cursor->key_format, "r") != 0) {
		fprintf(stderr,
		    "%s: %s: -a option illegal unless the primary key is a "
		    "record number\n",
		    progname, uri);
		ret = EINVAL;
	} else
		ret = load_data(cursor, uri);

	/*
	 * Technically, we don't have to close the cursor because the session
	 * handle will do it for us, but I'd like to see the flush to disk and
	 * the close succeed, it's better to fail early when loading files.
	 */
	if ((tret = cursor->close(cursor, NULL)) != 0) {
		fprintf(stderr, "%s: cursor.close: %s: %s\n", 
		    progname, uri, wiredtiger_strerror(ret));
		if (ret == 0)
			ret = tret;
	}
	if (ret == 0 && (ret = session->sync(session, uri, NULL)) != 0)
		fprintf(stderr, "%s: session.sync: %s: %s\n", 
		    progname, uri, wiredtiger_strerror(ret));
		
	return (ret);
}

/*
 * schema_read --
 *	Read the schema lines and do some basic validation.
 */
static int
schema_read(char ***listp, int *hexp)
{
	WT_BUF l;
	int entry, eof, max_entry;
	const char *s;
	char **list;

	memset(&l, 0, sizeof(l));

	/* Header line #1: "WiredTiger Dump" and a WiredTiger version. */
	if (read_line(&l, 0, &eof))
		return (1);
	s = "WiredTiger Dump ";
	if (strncmp(l.data, s, strlen(s)) != 0)
		return (format());

	/* Header line #2: "Format={hex,print}". */
	if (read_line(&l, 0, &eof))
		return (1);
	if (strcmp(l.data, "Format=print") == 0)
		*hexp = 0;
	else if (strcmp(l.data, "Format=hex") == 0)
		*hexp = 1;
	else
		return (format());

	/* Header line #3: "Header". */
	if (read_line(&l, 0, &eof))
		return (1);
	if (strcmp(l.data, "Header") != 0)
		return (format());

	/* Now, read in lines until we get to the end of the headers. */
	for (entry = max_entry = 0, list = NULL;; ++entry) {
		if (read_line(&l, 0, &eof))
			return (1);
		if (strcmp(l.data, "Data") == 0)
			break;

		/* Grow the array of header lines as necessary. */
		if ((max_entry == 0 || entry == max_entry - 1) &&
		    (list = realloc(list,
		    (size_t)(max_entry += 100) * sizeof(char *))) == NULL)
			return (util_syserr());
		if ((list[entry] = strdup(l.data)) == NULL)
			return (util_syserr());
	}
	list[entry] = NULL;
	*listp = list;

	/* The lines are supposed to be in pairs. */
	if (entry / 2 == 0)
		return (format());

	/* Leak the memory, I don't care. */
	return (0);
}

/*
 * schema_update --
 *	Reconcile and update the command line configuration against the
 * schema we found.
 */
static int
schema_update(char **list)
{
	size_t len;
	int found;
	char *buf, **p, *sep, **t;

#define MATCH(s, tag)                                           	\
	(strncmp(s, tag, strlen(tag)) == 0)

	/*
	 * If the object has been renamed, replace all of the column group,
	 * index, file and table names with the new name.
	 */
	if (cmdname != NULL)
		for (t = list; *t != NULL; t += 2) {
			if (!MATCH(*t, "colgroup:") &&
			    !MATCH(*t, "file:") &&
			    !MATCH(*t, "index:") &&
			    !MATCH(*t, "table:"))
				continue;

			/* Allocate room. */
			len = strlen(*t) + strlen(cmdname) + 10;
			if ((buf = malloc(len)) == NULL)
				return (util_syserr());

			/*
			 * Find the separating colon characters, but not the
			 * trailing one may not be there.
			 */
			sep = strchr(*t, ':');
			*sep = '\0';
			sep = strchr(sep + 1, ':');
			snprintf(buf, len, "%s:%s%s",
			    *t, cmdname, sep == NULL ? "" : sep);
			*t = buf;
		}

	/*
	 * It's possible to update everything except the key/value formats.
	 * If there were command-line configuration pairs, walk the list of
	 * command-line configuration strings, and check.
	 */
	for (p = cmdconfig; cmdconfig != NULL && *p != NULL; p += 2)
		if (strstr(p[1], "key_format=") ||
		    strstr(p[1], "value_format=")) {
			fprintf(stderr,
			    "%s: the command line configuration string may not "
			    "modify the object's key or value format\n",
			    progname);
			return (1);
		}

	/*
	 * If there were command-line configuration pairs, walk the list of
	 * command-line URIs and find a matching dump URI.  For each match,
	 * append the command-line configuration to the dump configuration.
	 * It is an error if a command-line URI doesn't find a match, that's
	 * likely a mistake.
	 */
	for (p = cmdconfig; cmdconfig != NULL && *p != NULL; p += 2) {
		found = 0;
		for (t = list; *t != NULL; t += 2)
			if (strncmp(*p, t[0], strlen(*p)) == 0) {
				found = 1;
				len = strlen(p[1]) + strlen(t[1]) + 10;
				if ((buf = malloc(len)) == NULL)
					return (util_syserr());
				snprintf(buf, len, "%s,%s", t[1], p[1]);
				t[1] = buf;
			}
		if (!found) {
			fprintf(stderr,
			    "%s: the command line object name %s was not "
			    "matched by any loaded object name\n",
			    progname, *p);
			return (1);
		}
	}

	/* Leak the memory, I don't care. */
	return (0);
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

#if 0
/*
 * load_file --
 *	Load a single file.
 */
static int
load_file(WT_SESSION *session,
    const char *name, const char *config, int read_recno, int append)
{
	WT_CURSOR *cursor;
	int ret, tret;

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
	ret = load_data(cursor, name, 
	    read_recno || strcmp(cursor->key_format, "r") == 0 ? 1 : 0);

	/*
	 * Technically, we don't have to close the cursor because the session
	 * handle will do it for us, but I'd like to see the flush to disk and
	 * the close succeed, it's better to fail early when loading files.
	 */
	if ((tret = cursor->close(cursor, NULL)) != 0) {
		fprintf(stderr, "%s: cursor.close: %s: %s\n", 
		    progname, name, wiredtiger_strerror(ret));
		if (ret == 0)
			ret = tret;
	}
	if (ret == 0 && (ret = session->sync(session, name, NULL)) != 0)
		fprintf(stderr, "%s: session.sync: %s: %s\n", 
		    progname, name, wiredtiger_strerror(ret));
		
	return (ret);
}
#endif

/*
 * load_data --
 *	Load the data.
 */
static int
load_data(WT_CURSOR *cursor, const char *name)
{
	WT_BUF key, value;
	uint64_t insert_count;
	int eof, ret;

	memset(&key, 0, sizeof(key));
	memset(&value, 0, sizeof(value));
	
	/* Read key/value pairs and insert them into the file. */
	for (insert_count = 0;;) {
		if (!append) {
			if (read_line(&key, 1, &eof))
				return (1);
			if (eof == 1)
				break;
			cursor->set_key(cursor, &key);
		}
		if (read_line(&value, append ? 1 : 0, &eof))
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
 * read_line --
 *	Read a line from stdin into a WT_BUF.
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
			    realloc(l->mem, l->memsize + 1024)) == NULL)
				return (util_syserr());
			l->memsize += 1024;
		}
		((uint8_t *)l->mem)[len] = (uint8_t)ch;
	}

	((uint8_t *)l->mem)[len] = '\0';		/* nul-terminate */

	l->data = l->mem;
	l->size = len;
	return (0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s%s "
	    "load [-as] [-f input-file] [-n name] [object configuration ...]\n",
	    progname, usage_prefix);
	return (1);
}
