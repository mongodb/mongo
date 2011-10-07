/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

struct record_t {
	WT_ITEM key;
	size_t   key_memsize;

	WT_ITEM value;
	size_t   value_memsize;
};

static int bulk_read(struct record_t *, int *);
static int bulk_read_line(WT_ITEM *, size_t *, int, int *);
static int usage(void);

int
util_load(WT_SESSION *session, int argc, char *argv[])
{
	WT_CURSOR *cursor;
	struct record_t record;
	uint64_t insert_count;
	int ch, eof, ret, printable;
	const char *table_config;
	char *name;

	table_config = NULL;
	name = NULL;
	printable = 0;

	while ((ch = util_getopt(argc, argv, "c:f:p")) != EOF)
		switch (ch) {
		case 'c':	/* command-line table configuration option */
			table_config = util_optarg;
			break;
		case 'f':	/* input file */
			if (freopen(util_optarg, "r", stdin) == NULL) {
				fprintf(stderr, "%s: %s: reopen: %s\n",
				    progname, util_optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'p':
			printable = 1;
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
	if ((name = util_name(*argv, "table", UTIL_TABLE_OK)) == NULL)
		return (EXIT_FAILURE);

	/* Remove and re-create the table. */
	if ((ret = session->drop(session, name, "force")) != 0)
		goto err;
	if ((ret = session->create(session, name, table_config)) != 0)
		goto err;

	/* Open the insert cursor. */
	if ((ret = session->open_cursor(session, name, NULL,
	    printable ? "dump,printable" : "dump,raw", &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
		    progname, name, wiredtiger_strerror(ret));
		goto err;
	}

	insert_count = 0;
	record.key.data = record.value.data = NULL;
	record.key_memsize = record.value_memsize = 0;

	for (eof = 0; (ret = bulk_read(&record, &eof)) == 0 && !eof;) {
		/* Report on progress every 100 inserts. */
		if (verbose && ++insert_count % 100 == 0) {
			printf("\r\t%s: %" PRIu64, name, insert_count);
			fflush(stdout);
		}
	
		cursor->set_key(cursor, &record.key);
		cursor->set_value(cursor, &record.value);
		if ((ret = cursor->insert(cursor)) != 0) {
			fprintf(stderr, "%s: cursor insert(%s) failed: %s\n",
			    progname, name, wiredtiger_strerror(ret));
			goto err;
		}
	}

	if (verbose)
		printf("\r\t%s: %" PRIu64 "\n", name, insert_count);

	if (0) {
err:		ret = 1;
	}

	if (name != NULL)
		free(name);

	return (ret);
}

/*
 * bulk_read --
 *	Read a key/value pair from stdin
 */
int
bulk_read(struct record_t *r, int *eofp)
{
	int ret;

	if ((ret = bulk_read_line(&r->key, &r->key_memsize, 1, eofp)) != 0)
		return (ret);
	if (*eofp)
		return (0);
	if ((ret = bulk_read_line(&r->value, &r->value_memsize, 0, eofp)) != 0)
		return (ret);

	return (0);
}

/*
 * bulk_read_line --
 *	Read a line from stdin into a WT_ITEM.
 */
int
bulk_read_line(WT_ITEM *item, size_t *memsize, int iskey, int *eofp)
{
	static unsigned long long line = 0;
	uint8_t *buf;
	uint32_t len;
	int ch;

	*eofp = 0;
	buf = (uint8_t *)item->data;
	for (len = 0;; ++len) {
		if ((ch = getchar()) == EOF) {
			if (iskey && len == 0) {
				*eofp = 1;
				return (0);
			}
			fprintf(stderr, "%s: corrupted input at line %llu\n",
			    progname, line + 1);
			return (WT_ERROR);
		}
		if (ch == '\n') {
			++line;
			break;
		}
		if (len >= *memsize) {
			if ((buf = realloc(buf, len + 128)) == NULL)
				return (errno);
			*memsize = len + 128;
		}
		buf[len] = (uint8_t)ch;
	}
	item->data = buf;
	item->size = len;
	return (0);
}

static int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s%s "
	    "load [-p] [-c table-config] [-f input-file] table\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
