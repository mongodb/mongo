/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wiredtiger.h>

#define	FILENAME	"xx"

#define	DUPS	0x01
#define	PRINT	0x02

const char *progname;
u_int	flags =  0;
int	bitcnt = 3;
int	nrecs = 10;

#define	FIX	1
#define	ROW	2
#define	VAR	3
int	file_type;

void col_search(WT_SESSION *);
void print(WT_SESSION *);
void row_search(WT_SESSION *);
void run(void);
int  usage(void);

int
main(int argc, char *argv[])
{
	int ch;

	(void)remove(FILENAME);
	(void)remove("__schema.wt");

	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

	file_type = ROW;		/* Row-store by default */
#if 0
	flags = PRINT;			/* Don't print table by default */
#endif

	while ((ch = getopt(argc, argv, "b:dn:pt:")) != EOF)
		switch (ch) {
		case 'b':
			bitcnt = atoi(optarg);
			break;
		case 'd':
			flags |= DUPS;
			break;
		case 'n':
			nrecs = atoi(optarg);
			break;
		case 't':
			if (strcmp(optarg, "fix") == 0)
				file_type = FIX;
			else if (strcmp(optarg, "row") == 0)
				file_type = ROW;
			else if (strcmp(optarg, "var") == 0)
				file_type = VAR;
			else
				return (usage());
			break;
		case 'p':
			flags |= PRINT;
			break;
		case '?':
		default:
			return (usage());
		}
	argc -= optind;
	argv += optind;

	if (argc != 0)
		return (usage());

	if ((flags & DUPS) && file_type == FIX) {
		fprintf(stderr,
		    "%s: -d requires row-store or variable-length "
		    "column-store file type\n", progname);
		return (EXIT_FAILURE);
	}

	run();

	return (EXIT_SUCCESS);
}

int
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-dp] [-b bits] [-n rows] [-t fix|var|row]\n", progname);
	return (EXIT_FAILURE);
}

/*
 * run --
 *	Build a row- or column-store page in a file.
 */
void
run(void)
{
	clock_t start, stop;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	char config[256], kbuf[64], vbuf[64];
	int cnt, ret;
	uint8_t bitf;

	assert(wiredtiger_open(NULL, NULL, "create", &conn) == 0);
	assert(conn->open_session(conn, NULL, NULL, &session) == 0);
	switch (file_type) {
	case FIX:
		(void)snprintf(
		    config, sizeof(config), "key_format=r,value_format=%dt",
		    bitcnt);
		break;
	case ROW:
		(void)snprintf(
		    config, sizeof(config), "key_format=S,value_format=S");
		break;
	case VAR:
		(void)snprintf(
		    config, sizeof(config), "key_format=r,value_format=S");
		break;
	default:
		assert(0);
	}
	assert(session->create(session, "file:" FILENAME, config) == 0);
	assert(session->open_cursor(
	    session, "file:" FILENAME, NULL, NULL, &cursor) == 0);

	/* Create the initial key/value pairs. */
	start = clock();
	for (cnt = 1; cnt < nrecs + 1; ++cnt) {
		switch (file_type) {			/* Build the key. */
		case FIX:
		case VAR:
			cursor->set_key(cursor, (uint64_t)cnt);
			break;
		case ROW:
			snprintf(kbuf, sizeof(kbuf), "%010d KEY------", cnt);
			cursor->set_key(cursor, kbuf);
			break;
		}

		switch (file_type) {			/* Build the value. */
		case FIX:
			switch (bitcnt) {
			case 8: bitf = cnt & 0xff; break;
			case 7: bitf = cnt & 0x7f; break;
			case 6: bitf = cnt & 0x3f; break;
			case 5: bitf = cnt & 0x1f; break;
			case 4: bitf = cnt & 0x0f; break;
			case 3: bitf = cnt & 0x07; break;
			case 2: bitf = cnt & 0x03; break;
			case 1:
			default: bitf = cnt & 0x01; break;
			}
			cursor->set_value(cursor, bitf);
			break;
		case ROW:
		case VAR:
			if (cnt != 1 &&
			    cnt != 500 && cnt != 1000 && flags & DUPS)
				strcpy(vbuf, "----- VALUE -----");
			else
				snprintf(vbuf, sizeof(vbuf),
				    "%010d VALUE----", cnt);
			cursor->set_value(cursor, vbuf);
			break;
		}
		if ((ret = cursor->insert(cursor)) != 0) {
			fprintf(stderr,
			    "cursor->insert: %s\n", wiredtiger_strerror(ret));
			exit (EXIT_FAILURE);
		}
	}

	stop = clock();
	fprintf(stderr, "timer: %.2lf\n",
	    (stop - start) / (double)CLOCKS_PER_SEC);

	assert(cursor->close(cursor, 0) == 0);
	assert(session->sync(session, "file:" FILENAME, NULL) == 0);

	/* Optionally print out the results and let the user search for keys. */
	if (flags & PRINT) {
		print(session);
		if (file_type == ROW)
			row_search(session);
		else
			col_search(session);
	}

	assert(conn->close(conn, 0) == 0);
}

void
print(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;
	const char *key, *value;
	uint8_t bitf;

	assert(session->open_cursor(
	    session, "file:" FILENAME, NULL, NULL, &cursor) == 0);
	while ((ret = cursor->next(cursor)) == 0) {
		switch (file_type) {
		case FIX:
			if ((ret = cursor->get_value(cursor, &bitf)) != 0)
				break;
			printf("0x%02x\n", bitf);
			break;
		case ROW:
			if ((ret = cursor->get_key(cursor, &key)) != 0)
				break;
			if (printf("%s\n", key) < 0) {
				ret = errno;
				break;
			}
			/* FALLTHROUGH */
		case VAR:
			if ((ret =
			    cursor->get_value(cursor, &value)) != 0)
				break;
			if (printf("%s\n", value) < 0)
				ret = errno;
			break;
		}
	}
	if (ret != WT_NOTFOUND) {
		fprintf(stderr, "%s: cursor get failed: %s\n",
		    progname, wiredtiger_strerror(ret));
		exit (EXIT_FAILURE);
	}
}

void
row_search(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int exact, ret;
	char *p, buf[256];
	const char *key, *value;

	assert(session->open_cursor(
	    session, "file:" FILENAME, NULL, NULL, &cursor) == 0);

	for (;;) {
		fprintf(stdout, "search-near string >>> ");
		fflush(stdout);
		if (fgets(buf, sizeof(buf), stdin) == NULL)
			break;
		if ((p = strchr(buf, '\n')) != NULL)
			*p = '\0';
		cursor->set_key(cursor, buf);
		if ((ret = cursor->search_near(cursor, &exact)) != 0) {
			fprintf(stderr,
			    "cursor->search_near: %s\n",
			    wiredtiger_strerror(ret));
			exit (EXIT_FAILURE);
		}
		if ((ret = cursor->get_key(cursor, &key)) != 0) {
			fprintf(stderr,
			    "cursor->get_key: %s\n",
			    wiredtiger_strerror(ret));
			exit (EXIT_FAILURE);
		}
		if ((ret = cursor->get_value(cursor, &value)) != 0) {
			fprintf(stderr,
			    "cursor->get_value: %s\n",
			    wiredtiger_strerror(ret));
			exit (EXIT_FAILURE);
		}
		fprintf(stdout, "%d: %s/%s\n", exact, key, value);
	}
}

void
col_search(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	uint64_t recno;
	int exact, ret;
	uint8_t bitf;
	char *p, buf[256];
	const char *value;

	assert(session->open_cursor(
	    session, "file:" FILENAME, NULL, NULL, &cursor) == 0);

	for (;;) {
		fprintf(stdout, "search-near string >>> ");
		fflush(stdout);
		if (fgets(buf, sizeof(buf), stdin) == NULL)
			break;
		if ((p = strchr(buf, '\n')) != NULL)
			*p = '\0';
		recno = (uint64_t)atoi(buf);
		cursor->set_key(cursor, recno);
		if ((ret = cursor->search_near(cursor, &exact)) != 0) {
			fprintf(stderr,
			    "cursor->search_near: %s\n",
			    wiredtiger_strerror(ret));
			exit (EXIT_FAILURE);
		}
		if ((ret = cursor->get_key(cursor, &recno)) != 0) {
			fprintf(stderr,
			    "cursor->get_key: %s\n",
			    wiredtiger_strerror(ret));
			exit (EXIT_FAILURE);
		}
		if (file_type == VAR) {
			if ((ret = cursor->get_value(cursor, &value)) != 0) {
				fprintf(stderr,
				    "cursor->get_value: %s\n",
				    wiredtiger_strerror(ret));
				exit (EXIT_FAILURE);
			}
			fprintf(stdout, "%d: %llu/%s\n", exact, recno, value);
		} else {
			if ((ret = cursor->get_value(cursor, &bitf)) != 0) {
				fprintf(stderr,
				    "cursor->get_value: %s\n",
				    wiredtiger_strerror(ret));
				exit (EXIT_FAILURE);
			}
			fprintf(
			    stdout, "%d: %llu/0x%02x\n", exact, recno, bitf);
		}
	}
}
