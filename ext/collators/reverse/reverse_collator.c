/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
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

#include <string.h>

#include <wiredtiger_ext.h>

static int
collate_reverse(WT_COLLATOR *collator, WT_SESSION *session,
    const WT_ITEM *k1, const WT_ITEM *k2, int *cmp)
{
	size_t len;

	(void)collator;					/* Unused */
	(void)session;

	len = (k1->size < k2->size) ? k1->size : k2->size;
	if ((*cmp = memcmp(k2->data, k1->data, len)) == 0)
		*cmp = ((int)k1->size - (int)k2->size);
	return (0);
}

static WT_COLLATOR reverse_collator = { collate_reverse };

int
wiredtiger_extension_init(
    WT_SESSION *session, WT_EXTENSION_API *api, const char *config)
{
	WT_CONNECTION *conn;

	(void)config;					/* Unused */
	(void)api;

	conn = session->connection;
	return (conn->add_collator(conn, "reverse", &reverse_collator, NULL));
}
