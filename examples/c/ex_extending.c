/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * ex_extending.c
 *	This is an example demonstrating ways to extend WiredTiger with
 *	extractors, collators and loadable modules.
 */

#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

const char *home = "WT_TEST";

/*! [case insensitive comparator] */
/* A simple case insensitive comparator. */
static int
__compare_nocase(WT_COLLATOR *collator, WT_SESSION *session,
    const WT_ITEM *v1, const WT_ITEM *v2, int *cmp)
{
	const char *s1 = (const char *)v1->data;
	const char *s2 = (const char *)v2->data;

	(void)session; /* unused */
	(void)collator; /* unused */

	*cmp = strcasecmp(s1, s2);
	return (0);
}

static WT_COLLATOR nocasecoll = { __compare_nocase };
/*! [case insensitive comparator] */

/*! [n character comparator] */
/*
 * Comparator that only compares the first N prefix characters of the string.
 * This has associated data, so we need to extend WT_COLLATOR.
 */
typedef struct {
	WT_COLLATOR iface;
	uint32_t maxlen;
} PREFIX_COLLATOR;

static int
__compare_prefixes(WT_COLLATOR *collator, WT_SESSION *session,
    const WT_ITEM *v1, const WT_ITEM *v2, int *cmp)
{
	PREFIX_COLLATOR *pcoll = (PREFIX_COLLATOR *)collator;
	const char *s1 = (const char *)v1->data;
	const char *s2 = (const char *)v2->data;

	(void)session; /* unused */

	*cmp = strncmp(s1, s2, pcoll->maxlen);
	return (0);
}

static PREFIX_COLLATOR pcoll10 = { {__compare_prefixes}, 10 };
/*! [n character comparator] */

int main(void)
{
	int ret;
	WT_CONNECTION *conn;
	WT_SESSION *session;

	/* Open a connection to the database, creating it if necessary. */
	if ((ret = wiredtiger_open(home, NULL, "create", &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	/*! [add collator nocase] */
	ret = conn->add_collator(conn, "nocase", &nocasecoll, NULL);
	/*! [add collator nocase] */
	/*! [add collator prefix10] */
	ret = conn->add_collator(conn, "prefix10", &pcoll10.iface, NULL);

	/* Open a session for the current thread's work. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error opening a session on %s: %s\n",
		    home, wiredtiger_strerror(ret));

	/* XXX Do some work... */

	/* Note: closing the connection implicitly closes open session(s). */
	if ((ret = conn->close(conn, NULL)) != 0)
	/*! [add collator prefix10] */
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));

	return (ret);
}
