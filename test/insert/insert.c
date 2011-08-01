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

#include "wt_internal.h"

#define	FILENAME	"xx"
#define	PSIZE		2048

#define	DUPS	0x01
#define	PRINT	0x02

#define timespecsub(vvp, uvp)						\
	do {								\
		(vvp)->tv_sec -= (uvp)->tv_sec;				\
		(vvp)->tv_nsec -= (uvp)->tv_nsec;			\
		if ((vvp)->tv_nsec < 0) {				\
			(vvp)->tv_sec--;				\
			(vvp)->tv_nsec += 1000000000;			\
		}							\
	} while (0)

const char *progname;
u_int	flags = PRINT;
int	bitcnt = 3;
int	nrecs = 10;
int	nrecs2 = 0;
int	page_type = WT_PAGE_ROW_LEAF;

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

	while ((ch = getopt(argc, argv, "b:dN:n:pt:")) != EOF)
		switch (ch) {
		case 'b':
			bitcnt = atoi(optarg);
			break;
		case 'd':
			flags |= DUPS;
			break;
		case 'N':
			nrecs2 = atoi(optarg);
			break;
		case 'n':
			nrecs = atoi(optarg);
			break;
		case 't':
			if (strcmp(optarg, "fix") == 0)
				page_type = WT_PAGE_COL_FIX;
			else if (strcmp(optarg, "var") == 0)
				page_type = WT_PAGE_COL_VAR;
			else if (strcmp(optarg, "row") == 0)
				page_type = WT_PAGE_ROW_LEAF;
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

	if ((flags & DUPS) && page_type == WT_PAGE_COL_FIX) {
		fprintf(stderr,
		    "%s: -d requires row-store or variable-length "
		    "column-store file type\n", progname);
		return (EXIT_FAILURE);
	}
	if (nrecs2 != 0 && page_type != WT_PAGE_ROW_LEAF) {
		fprintf(stderr,
		    "%s: -N requires row-store file type\n",
		    progname);
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
	WT_ITEM key, value;
	WT_SESSION *session;
	int cnt, ikey, ret;
	char config[256], kbuf[64], vbuf[64];

	assert(wiredtiger_open(NULL, NULL, "", &conn) == 0);
	assert(conn->open_session(conn, NULL, NULL, &session) == 0);
	switch (page_type) {
	case WT_PAGE_COL_FIX:
		(void)snprintf(config, sizeof(config),
		    "key_format=r,"
		    "value_format=%dt,"
		    "allocation_size=%d,"
		    "internal_node_min=%d,internal_node_max=%d,"
		    "leaf_node_min=%d,leaf_node_max=%d",
		    bitcnt, PSIZE, PSIZE, PSIZE, PSIZE, PSIZE);
		break;
	case WT_PAGE_COL_VAR:
		(void)snprintf(config, sizeof(config),
		    "key_format=r,"
		    "allocation_size=%d,"
		    "internal_node_min=%d,internal_node_max=%d,"
		    "leaf_node_min=%d,leaf_node_max=%d",
		    PSIZE, PSIZE, PSIZE, PSIZE, PSIZE);
		break;
	case WT_PAGE_ROW_LEAF:
		(void)snprintf(config, sizeof(config),
		    "key_format=u,"
		    "allocation_size=%d,"
		    "internal_node_min=%d,internal_node_max=%d,"
		    "leaf_node_min=%d,leaf_node_max=%d",
		    PSIZE, PSIZE, PSIZE, PSIZE, PSIZE);
		break;
	default:
		assert(0);
	}
	assert(session->create(session, "file:" FILENAME, config) == 0);
	assert(session->open_cursor(
	    session, "file:" FILENAME, NULL, "raw", &cursor) == 0);
	if (page_type != WT_PAGE_ROW_LEAF)
		cursor->recno = 0;

	/* Create the initial key/value pairs. */
	ikey = 100;
	for (cnt = 0; cnt < nrecs; ++cnt, ++ikey) {
		switch (page_type) {			/* Build the key. */
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			++cursor->recno;
			break;
		case WT_PAGE_ROW_LEAF:
			key.size = snprintf(
			    kbuf, sizeof(kbuf), "%010d KEY------", ikey);
			key.data = kbuf;
			cursor->set_key(cursor, &key);
			break;
		}

		switch (page_type) {			/* Build the value. */
		case WT_PAGE_COL_FIX:
			switch (bitcnt) {
			case 8: vbuf[0] = cnt & 0xff; break;
			case 7: vbuf[0] = cnt & 0x7f; break;
			case 6: vbuf[0] = cnt & 0x3f; break;
			case 5: vbuf[0] = cnt & 0x1f; break;
			case 4: vbuf[0] = cnt & 0x0f; break;
			case 3: vbuf[0] = cnt & 0x07; break;
			case 2: vbuf[0] = cnt & 0x03; break;
			case 1:
			default:vbuf[0] = cnt & 0x01; break;
			}
			value.size = 1;
			break;
		case WT_PAGE_COL_VAR:
		case WT_PAGE_ROW_LEAF:
			if (cnt != 1 &&
			    cnt != 500 && cnt != 1000 && flags & DUPS)
				value.size =
				    strlen(strcpy(vbuf, "----- VALUE -----"));
			else
				value.size = snprintf(vbuf,
				    sizeof(vbuf), "%010d VALUE----", cnt);
			break;
		}
		value.data = vbuf;
		cursor->set_value(cursor, &value);
		if ((ret = cursor->update(cursor)) != 0) {
			fprintf(stderr,
			    "cursor->update: %s\n", wiredtiger_strerror(ret));
			exit (EXIT_FAILURE);
		}
	}

	/* Insert key/value pairs into the set. */
	if (nrecs2 != 0) {
		start = clock();

		for (cnt = 0; cnt < nrecs2; ++cnt) {
			key.size = snprintf(kbuf, sizeof(kbuf),
			    "%010d KEY APPEND %010d", 105, cnt);
			key.data = kbuf;
			cursor->set_key(cursor, &key);
			value.size = snprintf(vbuf,
			    sizeof(vbuf), "%010d VALUE----", cnt);
			value.data = vbuf;
			cursor->set_value(cursor, &value);
			if ((ret = cursor->update(cursor)) != 0) {
				fprintf(stderr,
				    "cursor->update: %s\n",
				    wiredtiger_strerror(ret));
				exit (EXIT_FAILURE);
			}
		}

		stop = clock();
		fprintf(stderr, "timer: %.2lf\n",
		    (stop - start) / (double)CLOCKS_PER_SEC);
	}

	assert(session->sync(session, "file:" FILENAME, NULL) == 0);

	if (flags & PRINT) {
		while ((ret = cursor->next(cursor)) == 0) {
			if ((ret = cursor->get_key(cursor, &key)) != 0)
				break;
			if ((ret = cursor->get_value(cursor, &value)) != 0)
				break;
			switch (page_type) {
			case WT_PAGE_COL_FIX:
				printf("0x%02x\n", ((uint8_t *)value.data)[0]);
				break;
			case WT_PAGE_COL_VAR:
			case WT_PAGE_ROW_LEAF:
				if ((key.size != 0 && (fwrite(key.data,
				    1, key.size, stdout) != key.size ||
				    fwrite("\n", 1, 1, stdout) != 1)) ||
				    fwrite(value.data,
					1, value.size, stdout) != value.size ||
				    fwrite("\n", 1, 1, stdout) != 1) {
					ret = errno;
					break;
				}
				break;
			}
		}
		if (ret != WT_NOTFOUND) {
			fprintf(stderr, "%s: cursor get failed: %s\n",
			    progname, wiredtiger_strerror(ret));
			exit (EXIT_FAILURE);
		}
	}

	assert(conn->close(conn, 0) == 0);
}
