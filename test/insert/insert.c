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

#define	FIX	1
#define	ROW	2
#define	VAR	3
int	file_type = ROW;

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
	if (nrecs2 != 0 && file_type != ROW) {
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
	WT_SESSION *session;
	const char *key, *value;
	char config[256], kbuf[64], vbuf[64];
	int cnt, ikey, ret;
	uint8_t bitf;

	assert(wiredtiger_open(NULL, NULL, "", &conn) == 0);
	assert(conn->open_session(conn, NULL, NULL, &session) == 0);
	switch (file_type) {
	case FIX:
		(void)snprintf(config, sizeof(config),
		    "key_format=r,value_format=%dt,"
		    "allocation_size=%d,"
		    "internal_node_min=%d,internal_node_max=%d,"
		    "leaf_node_min=%d,leaf_node_max=%d",
		    bitcnt, PSIZE, PSIZE, PSIZE, PSIZE, PSIZE);
		break;
	case ROW:
		(void)snprintf(config, sizeof(config),
		    "key_format=S,value_format=S,"
		    "allocation_size=%d,"
		    "internal_node_min=%d,internal_node_max=%d,"
		    "leaf_node_min=%d,leaf_node_max=%d",
		    PSIZE, PSIZE, PSIZE, PSIZE, PSIZE);
		break;
	case VAR:
		(void)snprintf(config, sizeof(config),
		    "key_format=r,value_format=S,"
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
	    session, "file:" FILENAME, NULL, NULL, &cursor) == 0);

	/* Create the initial key/value pairs. */
	ikey = 100;
	for (cnt = 0; cnt < nrecs; ++cnt, ++ikey) {
		switch (file_type) {			/* Build the key. */
		case FIX:
		case VAR:
			cursor->set_key(cursor, (uint64_t)cnt + 1);
			break;
		case ROW:
			snprintf(kbuf, sizeof(kbuf), "%010d KEY------", ikey);
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

	/* Insert key/value pairs into the set. */
	if (nrecs2 != 0) {
		start = clock();

		for (cnt = 0; cnt < nrecs2; ++cnt) {
			snprintf(kbuf, sizeof(kbuf),
			    "%010d KEY APPEND %010d", 105, cnt);
			cursor->set_key(cursor, kbuf);
			snprintf(vbuf, sizeof(vbuf), "%010d VALUE----", cnt);
			cursor->set_value(cursor, vbuf);
			if ((ret = cursor->insert(cursor)) != 0) {
				fprintf(stderr,
				    "cursor->insert: %s\n",
				    wiredtiger_strerror(ret));
				exit (EXIT_FAILURE);
			}
		}

		stop = clock();
		fprintf(stderr, "timer: %.2lf\n",
		    (stop - start) / (double)CLOCKS_PER_SEC);
	}
	assert(cursor->close(cursor, 0) == 0);

	assert(session->sync(session, "file:" FILENAME, NULL) == 0);

	assert(session->open_cursor(
	    session, "file:" FILENAME, NULL, NULL, &cursor) == 0);
	if (flags & PRINT) {
		while ((ret = cursor->next(cursor)) == 0) {
			switch (file_type) {
			case FIX:
				if ((ret =
				    cursor->get_value(cursor, &bitf)) != 0)
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

	assert(conn->close(conn, 0) == 0);
}
