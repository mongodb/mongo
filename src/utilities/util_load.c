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
util_load(int argc, char *argv[])
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	struct record_t record;
	size_t len;
	uint64_t insert_count;
	int ch, debug, eof, ret, printable, tret;
	const char *table_config;
	char cursor_config[100];
	char *tablename;

	conn = NULL;
	table_config = NULL;
	tablename = NULL;
	debug = printable = 0;

	while ((ch = getopt(argc, argv, "c:df:T")) != EOF)
		switch (ch) {
		case 'c':			/* command-line option */
			table_config = optarg;
			break;
		case 'd':			/* command-line option */
			debug = 1;
			break;
		case 'f':			/* input file */
			if (freopen(optarg, "r", stdin) == NULL) {
				fprintf(stderr, "%s: %s: reopen: %s\n",
				    progname, optarg, strerror(errno));
				return (EXIT_FAILURE);
			}
			break;
		case 'T':
			printable = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	/*
	 * Right now, we only support text input -- require the T option to
	 * match Berkeley DB's API.
	 */
	if (printable == 0) {
		fprintf(stderr,
		    "%s: the -T option is currently required\n", progname);
		return (EXIT_FAILURE);
	}

	/* The remaining argument is the table name. */
	if (argc != 1)
		return (usage());

	len = sizeof("table:") + strlen(*argv);
	if ((tablename = calloc(len, 1)) == NULL) {
		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
		return (EXIT_FAILURE);
	}
	snprintf(tablename, len, "table:%s", *argv);

	if ((ret = wiredtiger_open(home,
	    verbose ? verbose_handler : NULL, NULL, &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		goto err;

	if ((ret = session->drop(session, tablename, "force")) != 0)
		goto err;

	if ((ret = session->create(session, tablename, table_config)) != 0)
		goto err;

	snprintf(cursor_config, sizeof(cursor_config), "dump%s%s",
	    printable ? ",printable" : ",raw", debug ? ",debug" : "");

	if ((ret = session->open_cursor(
	    session, tablename, NULL, cursor_config, &cursor)) != 0) {
		fprintf(stderr, "%s: cursor open(%s) failed: %s\n",
		    progname, tablename, wiredtiger_strerror(ret));
		goto err;
	}

	insert_count = 0;
	record.key.data = record.value.data = NULL;
	record.key_memsize = record.value_memsize = 0;

	for (eof = 0; (ret = bulk_read(&record, &eof)) == 0 && !eof;) {
                /* Report on progress every 100 inserts. */
                if (verbose && ++insert_count % 100 == 0) {
                        printf("\r\t%s: %" PRIu64, tablename, insert_count);
			fflush(stdout);
		}
	
		cursor->set_key(cursor, &record.key);
		cursor->set_value(cursor, &record.value);
		if ((ret = cursor->insert(cursor)) != 0) {
			fprintf(stderr, "%s: cursor insert(%s) failed: %s\n",
			    progname, tablename, wiredtiger_strerror(ret));
			goto err;
		}
	}

	if (verbose)
		printf("\r\t%s: %" PRIu64 "\n", tablename, insert_count);

	if (0) {
err:		ret = 1;
	}
	if (conn != NULL && (tret = conn->close(conn, NULL)) != 0 && ret == 0)
		ret = tret;

	if (tablename != NULL)
		free(tablename);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
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
	    "load [-dT] [-c configuration] [-f input-file] file\n",
	    progname, usage_prefix);
	return (EXIT_FAILURE);
}
