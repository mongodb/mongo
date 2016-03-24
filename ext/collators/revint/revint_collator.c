/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <wiredtiger_ext.h>

/*
 * A simple WiredTiger collator for indices having a single integer key,
 * where the ordering is descending (reversed).  This collator also
 * requires that primary key be an integer.
 */

/* Local collator structure. */
typedef struct {
	WT_COLLATOR collator;		/* Must come first */
	WT_EXTENSION_API *wt_api;	/* Extension API */
} REVINT_COLLATOR;

/*
 * revint_compare --
 *	WiredTiger reverse integer collation, used for tests.
 */
static int
revint_compare(WT_COLLATOR *collator,
    WT_SESSION *session, const WT_ITEM *k1, const WT_ITEM *k2, int *cmp)
{
	const REVINT_COLLATOR *revint_collator;
	WT_EXTENSION_API *wtapi;
	WT_PACK_STREAM *pstream;
	int ret;
	int64_t i1, i2, p1, p2;

	i1 = i2 = p1 = p2 = 0;
	revint_collator = (const REVINT_COLLATOR *)collator;
	wtapi = revint_collator->wt_api;

	/*
	 * All indices using this collator have an integer key, and the
	 * primary key is also an integer. A collator is usually passed the
	 * concatenation of index key and primary key (when available),
	 * hence we initially unpack using "ii".
	 *
	 * A collator may also be called with an item that includes a index
	 * key and no primary key.  Among items having the same index key,
	 * an item with no primary key should sort before an item with a
	 * primary key. The reason is that if the application calls
	 * WT_CURSOR::search on a index key for which there are more than
	 * one value, the search key will not yet have a primary key.  We
	 * want to position the cursor at the 'first' matching index key so
	 * that repeated calls to WT_CURSOR::next will see them all.
	 *
	 * To keep this code simple, we do not reverse the ordering
	 * when comparing primary keys.
	 */
	if ((ret = wtapi->unpack_start(
	    wtapi, session, "ii", k1->data, k1->size, &pstream)) != 0 ||
	    (ret = wtapi->unpack_int(wtapi, pstream, &i1)) != 0)
		goto err;
	if ((ret = wtapi->unpack_int(wtapi, pstream, &p1)) != 0)
		/* A missing primary key is OK and sorts first. */
		p1 = INT64_MIN;
	if ((ret = wtapi->pack_close(wtapi, pstream, NULL)) != 0)
		goto err;

	/* Unpack the second pair of numbers. */
	if ((ret = wtapi->unpack_start(
	    wtapi, session, "ii", k2->data, k2->size, &pstream)) != 0 ||
	    (ret = wtapi->unpack_int(wtapi, pstream, &i2)) != 0)
		goto err;
	if ((ret = wtapi->unpack_int(wtapi, pstream, &p2)) != 0)
		/* A missing primary key is OK and sorts first. */
		p2 = INT64_MIN;
	if ((ret = wtapi->pack_close(wtapi, pstream, NULL)) != 0)
		goto err;

	/* sorting is reversed */
	if (i1 < i2)
		*cmp = 1;
	else if (i1 > i2)
		*cmp = -1;
	/* compare primary keys next, not reversed */
	else if (p1 < p2)
		*cmp = -1;
	else if (p1 > p2)
		*cmp = 1;
	else
		*cmp = 0; /* index key and primary key are same */

err:	return (ret);
}

/*
 * revint_terminate --
 *	Terminate is called to free the collator and any associated memory.
 */
static int
revint_terminate(WT_COLLATOR *collator, WT_SESSION *session)
{
	(void)session;				/* Unused parameters */

	/* Free the allocated memory. */
	free(collator);
	return (0);
}

/*
 * wiredtiger_extension_init --
 *	WiredTiger revint collation extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	REVINT_COLLATOR *revint_collator;

	(void)config;				/* Unused parameters */

	if ((revint_collator = calloc(1, sizeof(REVINT_COLLATOR))) == NULL)
		return (errno);

	revint_collator->collator.compare = revint_compare;
	revint_collator->collator.terminate = revint_terminate;
	revint_collator->wt_api = connection->get_extension_api(connection);

	return (connection->add_collator(
	    connection, "revint", &revint_collator->collator, NULL));
}
