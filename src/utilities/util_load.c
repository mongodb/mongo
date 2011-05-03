/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wiredtiger.h"
#include "util.h"

const char *progname;

struct record_t {
	WT_ITEM key;
	size_t   key_memsize;

	WT_ITEM value;
	size_t   value_memsize;
};

int bulk_read(struct record_t *, int *);
int bulk_read_line(WT_ITEM *, size_t *, int, int *);
int usage(void);

struct {
	int pagesize_set;
	uint32_t allocsize, intlmin, intlmax, leafmin, leafmax;
} config;

int
main(int argc, char *argv[])
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	struct record_t record;
	uint64_t insert_count;
	int ch, debug, eof, ret, text_input, tret, verbose;
	const char *tablename, *table_config, *home;
	char cursor_config[100], datasink[100];

	WT_UTILITY_INTRO(progname, argv);

	conn = NULL;
	table_config = NULL;
	home = NULL;
	debug = text_input = verbose = 0;

	while ((ch = getopt(argc, argv, "c:f:h:TVv")) != EOF)
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
		case 'h':			/* command-line option */
			home = optarg;
			break;
		case 'T':
			text_input = 1;
			break;
		case 'V':			/* version */
			printf("%s\n", wiredtiger_version(NULL, NULL, NULL));
			return (EXIT_SUCCESS);
		case 'v':
			verbose = 1;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	/* The remaining argument is the file name. */
	if (argc != 1)
		return (usage());
	tablename = *argv;

	/*
	 * Right now, we only support text input -- require the T option to
	 * match Berkeley DB's API.
	 */
	if (text_input == 0) {
		fprintf(stderr,
		    "%s: the -T option is currently required\n", progname);
		return (EXIT_FAILURE);
	}

	if ((ret = wiredtiger_open(home, NULL, NULL, &conn)) != 0 ||
	    (ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		goto err;

	snprintf(datasink, sizeof(datasink), "table:%s", tablename);

	if ((ret = session->drop(session, datasink, "force")) != 0)
		goto err;

	if ((ret = session->create(session, datasink, table_config)) != 0)
		goto err;

	snprintf(cursor_config, sizeof(cursor_config), "bulk,dump=%s%s",
	    text_input ? "print" : "raw", debug ? ",debug" : "");

	if ((ret = session->open_cursor(session, datasink, NULL,
	    cursor_config, &cursor)) != 0) {
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
                        printf("\r\t%s: %llu", tablename, insert_count);
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
		printf("\r\t%s: %llu\n", tablename, insert_count);

	if (0) {
err:		ret = 1;
	}
	if (conn != NULL && (tret = conn->close(conn, NULL)) != 0 && ret == 0)
		ret = tret;
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

int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-TVv] [-c configuration] [-f input-file] file\n",
	    progname);
	return (EXIT_FAILURE);
}
