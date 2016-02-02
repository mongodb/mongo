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

#include <wiredtiger_ext.h>
#include <errno.h>
#include <stdint.h>

/*
 * Set up a collator used with indices having a single integer key,
 * where the ordering is descending (reversed).
 */

/*
 * collate_error --
 *	Handle errors in the collator in a standard way.
 */
static void
collate_error(int ret, const char *description)
{
	fprintf(stderr, "revint_collator error: %s: %s\n",
	    wiredtiger_strerror(ret), description);
	/*
	 * For errors, we call abort. Since this collator is used in testing
	 * WiredTiger, we want it to immediately fail hard. A product-ready
	 * version of this function would not abort.
	 */
	abort();
}

/*
 * collate_revint --
 *	WiredTiger reverse integer collation, used for tests.
 */
static int
collate_revint(WT_COLLATOR *collator,
    WT_SESSION *session, const WT_ITEM *k1, const WT_ITEM *k2, int *cmp)
{
	WT_PACK_STREAM *s1, *s2;
	int ret;
	int64_t i1, i2, p1, p2;

	(void)collator;					/* Unused */
	(void)session;

	s1 = NULL;
	s2 = NULL;

	/*
	 * All indices using this collator have an integer key, and the
	 * primary key is also an integer. A collator is passed the
	 * concatenation of index key and primary key (when available),
	 * hence we unpack using "ii".
	 */
	if ((ret = wiredtiger_unpack_start(session, "ii",
	    k1->data, k1->size, &s1)) != 0 ||
	    (ret = wiredtiger_unpack_start(session, "ii",
	    k2->data, k2->size, &s2)) != 0)
		collate_error(ret, "unpack start");
		/* does not return */

	if ((ret = wiredtiger_unpack_int(s1, &i1)) != 0)
		collate_error(ret, "unpack index key 1");
	if ((ret = wiredtiger_unpack_int(s2, &i2)) != 0)
		collate_error(ret, "unpack index key 2");

	/* sorting is reversed */
	if (i1 < i2)
		*cmp = 1;
	else if (i1 > i2)
		*cmp = -1;
	else {
		/*
		 * Compare primary keys next.
		 *
		 * Allow failures here.  A collator may be called with an
		 * item that includes a index key and no primary key.  Among
		 * items having the same index key, an item with no primary
		 * key should sort before an item with a primary key. The
		 * reason is that if the application calls WT_CURSOR::search
		 * on a index key for which there are more than one value,
		 * the search key will not yet have a primary key.  We want
		 * to position the cursor at the 'first' matching index key
		 * so that repeated calls to WT_CURSOR::next will see them
		 * all.
		 *
		 * To keep this code simple, we do keep secondary sort of
		 * primary keys in forward (not reversed) order.
		 */
		if ((ret = wiredtiger_unpack_int(s1, &p1)) != 0) {
			if (ret == ENOMEM)
				p1 = INT64_MIN; /* sort this first */
			else
				collate_error(ret, "unpack primary key 1");
		}
		if ((ret = wiredtiger_unpack_int(s2, &p2)) != 0) {
			if (ret == ENOMEM)
				p2 = INT64_MIN; /* sort this first */
			else
				collate_error(ret, "unpack primary key 2");
		}

		/* sorting is not reversed here */
		if (p1 < p2)
			*cmp = -1;
		else if (p1 > p2)
			*cmp = 1;
		else
			*cmp = 0; /* index key and primary key are same */
	}
	if ((ret = wiredtiger_pack_close(s1, NULL)) != 0 ||
	    (ret = wiredtiger_pack_close(s2, NULL)) != 0)
		collate_error(ret, "unpack close");

	return (0);
}

static WT_COLLATOR revint_collator = { collate_revint, NULL, NULL };

/*
 * wiredtiger_extension_init --
 *	WiredTiger revint collation extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	(void)config;				/* Unused parameters */

	return (connection->add_collator(
	    connection, "revint", &revint_collator, NULL));
}
