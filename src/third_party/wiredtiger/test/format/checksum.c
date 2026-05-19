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

#include "format.h"

struct checksum_table_arg {
    WT_SESSION *session;
    uint64_t hash;
};

/*
 * checksum_key --
 *     Update an incremental checksum with the key part of a key/value pair.
 */
static void
checksum_key(uint64_t *hash, TABLE *table, WT_ITEM *key)
{
    /* Skip any key prefix added by (e.g.) mirrored tables. */
    uint32_t keyno = atou32("checksum-key", (char *)key->data + NTV(table, BTREE_PREFIX_LEN), '.');
    *hash = testutil_fnv1a_add_bytes(*hash, (const uint8_t *)&keyno, sizeof(uint32_t));
}

/*
 * checksum_value --
 *     Update an incremental checksum with the value part of a key/value pair.
 */
static void
checksum_value(uint64_t *hash, WT_ITEM *value)
{
    *hash = testutil_fnv1a_add_bytes(*hash, value->data, value->size);
}

/*
 * checksum_table --
 *     Update an incremental checksum with the contents of a table.
 */
static void
checksum_table(TABLE *t, void *arg)
{
    testutil_assert(t->type == ROW);

    struct checksum_table_arg *args = arg;

    uint64_t *hash = &args->hash;
    WT_SESSION *session = args->session;
    const char *uri = t->uri;

    wt_wrap_begin_transaction(session, NULL);
    testutil_check(
      session->timestamp_transaction_uint(session, WT_TS_TXN_TYPE_READ, g.stable_timestamp));

    WT_CURSOR *cursor;
    wt_wrap_open_cursor(session, uri, NULL, &cursor);

    int ret = 0;
    while ((ret = cursor->next(cursor)) == 0) {
        WT_ITEM key;
        testutil_check(cursor->get_key(cursor, &key));
        checksum_key(hash, t, &key);

        WT_ITEM value;
        testutil_check(cursor->get_value(cursor, &value));
        checksum_value(hash, &value);
    }
    if (ret == WT_NOTFOUND)
        ret = 0;
    testutil_check(ret);

    testutil_check(cursor->close(cursor));
    testutil_check(session->rollback_transaction(session, NULL));
}

/*
 * checksum_database --
 *     Calculate and report a checksum over every table we know about. Eventually we should compare
 *     this checksum against one reported on the follower node, but that's future work.
 */
uint64_t
checksum_database(WT_SESSION *session)
{
    struct checksum_table_arg arg = {
      .session = session,
      .hash = testutil_fnv1a_init(),
    };

    tables_apply(checksum_table, &arg);

    return (arg.hash);
}
