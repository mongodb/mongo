/*-
 * Public Domain 2014-present MongoDB, Inc.
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

#include <string.h>

#include <wiredtiger_ext.h>

/*
 * collate_reverse --
 *     WiredTiger reverse collation.
 */
static int
collate_reverse(
  WT_COLLATOR *collator, WT_SESSION *session, const WT_ITEM *k1, const WT_ITEM *k2, int *ret)
{
    size_t len;
    int cmp;

    (void)collator; /* Unused */
    (void)session;

    len = (k1->size < k2->size) ? k1->size : k2->size;
    cmp = memcmp(k1->data, k2->data, len);
    if (cmp < 0)
        *ret = 1;
    else if (cmp > 0)
        *ret = -1;
    else if (k1->size < k2->size)
        *ret = 1;
    else if (k1->size > k2->size)
        *ret = -1;
    else
        *ret = 0;
    return (0);
}

static WT_COLLATOR reverse_collator = {collate_reverse, NULL, NULL};

/*
 * wiredtiger_extension_init --
 *     WiredTiger reverse collation extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    (void)config; /* Unused parameters */

    return (connection->add_collator(connection, "reverse", &reverse_collator, NULL));
}
