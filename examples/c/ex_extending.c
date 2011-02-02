/*
 * ex_extending.c
 * Copyright (c) 2010 WiredTiger, Inc.  All rights reserved.
 *
 * This is an example demonstrating ways to extend WiredTiger with extractors,
 * collators and loadable modules.
 */

#include <stdio.h>
#include <string.h>

#include <wiredtiger_ext.h>

const char *home = "WT_TEST";

/* Case insensitive comparator. */
static int
__compare_nocase(WT_SESSION *session, WT_COLLATOR *collator, 
    const WT_DATAITEM *v1, const WT_DATAITEM *v2, int *cmp)
{
	const char *s1 = v1->data;
	const char *s2 = v2->data;

	session = NULL; /* unused */
	collator = NULL; /* unused */

	*cmp = strcasecmp(s1, s2);
	return (0);
}

static WT_COLLATOR nocasecoll = { __compare_nocase };

/*
 * Comparator that only compares the first N characters in strings.  This
 * has associated data, so we need to extend WT_COLLATOR.
 */
typedef struct {
	WT_COLLATOR iface;
	int maxlen;
} PREFIX_COLLATOR;

static int
__compare_prefixes(WT_SESSION *session, WT_COLLATOR *collator, 
    const WT_DATAITEM *v1, const WT_DATAITEM *v2, int *cmp)
{
	PREFIX_COLLATOR *pcoll = (PREFIX_COLLATOR *)collator;
	const char *s1 = v1->data;
	const char *s2 = v2->data;

	session = NULL; /* unused */

	*cmp = strncmp(s1, s2, pcoll->maxlen);
	return (0);
}

static PREFIX_COLLATOR pcoll10 = { {__compare_prefixes}, 10 };

int main()
{
	int ret;
	WT_CONNECTION *conn;
	WT_SESSION *session;

	/* Open a connection to the database, creating it if necessary. */
	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	ret = conn->add_collator(conn, "nocase", &nocasecoll, NULL);
	ret = conn->add_collator(conn, "prefix10", &pcoll10.iface, NULL);

	/* Open a session for the current thread's work. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error opening a session on %s: %s\n",
		    home, wiredtiger_strerror(ret));

	/* XXX Do some work... */

	/* Note: closing the connection implicitly closes open session(s). */
	if ((ret = conn->close(conn, NULL)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	return (ret);
}
