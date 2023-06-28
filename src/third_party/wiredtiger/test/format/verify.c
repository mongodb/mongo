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

/*
 * table_verify --
 *     Verify a single table.
 */
void
table_verify(TABLE *table, void *arg)
{
    SAP sap;
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;

    conn = (WT_CONNECTION *)arg;
    testutil_assert(table != NULL);

    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, table->track_prefix, &session);
    ret = session->verify(session, table->uri, "strict");
    testutil_assert(ret == 0 || ret == EBUSY);
    if (ret == EBUSY)
        WARN("table.%u skipped verify because of EBUSY", table->id);
    wt_wrap_close_session(session);
}

/*
 * table_mirror_row_next --
 *     Move to the next row-store original record.
 */
static int
table_mirror_row_next(TABLE *table, WT_CURSOR *cursor, WT_ITEM *key, uint64_t *keynop)
{
    WT_DECL_RET;
    const char *p;

    /* RS tables insert records in the table, skip to the next original key/value pair. */
    for (;;) {
        if ((ret = read_op(cursor, NEXT, NULL)) == WT_NOTFOUND)
            return (WT_NOTFOUND);
        /* WT_ROLLBACK isn't illegal, but it would mean restarting the verify somehow. */
        testutil_assert(ret == 0);

        /* The original keys are either short or have ".00" as a suffix. */
        testutil_check(cursor->get_key(cursor, key));
        testutil_assert((p = strchr(key->data, '.')) != NULL);
        testutil_assert(key->size - WT_PTRDIFF(p, key->data) >= 3);
        if (p[1] == '0' && p[2] == '0')
            break;
    }

    /* There may be a common key prefix, skip over it. */
    *keynop = atou32("mirror-verify", (char *)key->data + NTV(table, BTREE_PREFIX_LEN), '.');
    return (0);
}

/*
 * table_mirror_fail_msg --
 *     Messages on failure.
 */
static void
table_mirror_fail_msg(WT_SESSION *session, const char *checkpoint, TABLE *base, uint64_t base_keyno,
  WT_ITEM *base_key, WT_ITEM *base_value, TABLE *table, uint64_t table_keyno, WT_ITEM *table_key,
  WT_ITEM *table_value, uint64_t last_match)
{
    if (checkpoint != NULL)
        trace_msg(session, "checkpoint %s\n", checkpoint);
    trace_msg(session,
      "mirror: %" PRIu64 "/%" PRIu64 " mismatch: %s: {%.*s}/{%.*s}, %s: {%.*s}/{%.*s}\n",
      base_keyno, table_keyno, base->uri, base->type == ROW ? (int)base_key->size : 1,
      base->type == ROW ? (char *)base_key->data : "#", (int)base_value->size,
      (char *)base_value->data, table->uri, table->type == ROW ? (int)table_key->size : 1,
      table->type == ROW ? (char *)table_key->data : "#", (int)table_value->size,
      (char *)table_value->data);
    trace_msg(session, "last successful match was %" PRIu64 "\n", last_match);
    if (checkpoint != NULL)
        fprintf(stderr, "checkpoint %s\n", checkpoint);
    fprintf(stderr,
      "mirror: %" PRIu64 "/%" PRIu64 " mismatch: %s: {%.*s}/{%.*s}, %s: {%.*s}/{%.*s}\n",
      base_keyno, table_keyno, base->uri, base->type == ROW ? (int)base_key->size : 1,
      base->type == ROW ? (char *)base_key->data : "#", (int)base_value->size,
      (char *)base_value->data, table->uri, table->type == ROW ? (int)table_key->size : 1,
      table->type == ROW ? (char *)table_key->data : "#", (int)table_value->size,
      (char *)table_value->data);
    fprintf(stderr, "last successful match was %" PRIu64 "\n", last_match);
}

/*
 * table_mirror_fail_msg_flcs --
 *     Messages on failure, for when the table is FLCS.
 */
static void
table_mirror_fail_msg_flcs(WT_SESSION *session, const char *checkpoint, TABLE *base,
  uint64_t base_keyno, WT_ITEM *base_key, WT_ITEM *base_value, uint8_t base_bitv, TABLE *table,
  uint64_t table_keyno, uint8_t table_bitv)
{
    testutil_assert(table->type == FIX);
    trace_msg(session,
      "mirror: %" PRIu64 "/%" PRIu64 " mismatch: %s: {%.*s}/{%.*s} [%#x], %s: {#}/{%#x} %s%s%s\n",
      base_keyno, table_keyno, base->uri, base->type == ROW ? (int)base_key->size : 1,
      base->type == ROW ? (char *)base_key->data : "#", (int)base_value->size,
      (char *)base_value->data, base_bitv, table->uri, table_bitv,
      checkpoint ? " (checkpoint " : "", checkpoint ? checkpoint : "", checkpoint ? ")" : "");
    fprintf(stderr,
      "mirror: %" PRIu64 "/%" PRIu64 " mismatch: %s: {%.*s}/{%.*s} [%#x], %s: {#}/{%#x} %s%s%s\n",
      base_keyno, table_keyno, base->uri, base->type == ROW ? (int)base_key->size : 1,
      base->type == ROW ? (char *)base_key->data : "#", (int)base_value->size,
      (char *)base_value->data, base_bitv, table->uri, table_bitv,
      checkpoint ? " (checkpoint " : "", checkpoint ? checkpoint : "", checkpoint ? ")" : "");
}

/*
 * table_verify_mirror --
 *     Verify a mirrored pair.
 */
static void
table_verify_mirror(WT_CONNECTION *conn, TABLE *base, TABLE *table, const char *checkpoint)
{
    SAP sap;
    WT_CURSOR *base_cursor, *table_cursor;
    WT_ITEM base_key, base_value, table_key, table_value;
    WT_SESSION *session;
    uint64_t base_id, base_keyno, last_match, table_id, table_keyno, rows;
    uint8_t base_bitv, table_bitv;
    u_int failures, i, last_failures;
    int base_ret, table_ret;
    char buf[256], tagbuf[128];

    base_keyno = table_keyno = 0;             /* -Wconditional-uninitialized */
    base_bitv = table_bitv = FIX_VALUE_WRONG; /* -Wconditional-uninitialized */
    base_ret = table_ret = 0;
    last_match = 0;

    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, NULL, &session);

    /* Optionally open a checkpoint to verify. */
    if (checkpoint != NULL)
        testutil_check(__wt_snprintf(buf, sizeof(buf), "checkpoint=%s", checkpoint));

    /*
     * If opening a checkpoint, retry if the cursor checkpoint IDs don't match, it just means that a
     * checkpoint happened between the two open calls.
     */
    for (;;) {
        wt_wrap_open_cursor(session, base->uri, checkpoint == NULL ? NULL : buf, &base_cursor);
        base_id = base_cursor->checkpoint_id(base_cursor);
        wt_wrap_open_cursor(session, table->uri, checkpoint == NULL ? NULL : buf, &table_cursor);
        table_id = table_cursor->checkpoint_id(table_cursor);

        testutil_assert((checkpoint == NULL && base_id == 0 && table_id == 0) ||
          (checkpoint != NULL && base_id != 0 && table_id != 0));

        if (checkpoint == NULL || base_id == table_id)
            break;
        testutil_check(base_cursor->close(base_cursor));
        testutil_check(table_cursor->close(table_cursor));
    }

    testutil_check(__wt_snprintf(buf, sizeof(buf),
      "table %u %s%s"
      "mirror verify",
      table->id, checkpoint == NULL ? "" : checkpoint, checkpoint == NULL ? "" : " checkpoint "));
    trace_msg(session, "%s: start", buf);

    for (failures = 0, rows = 1; rows <= TV(RUNS_ROWS); ++rows) {
        last_failures = failures;
        switch (base->type) {
        case FIX:
            testutil_assert(base->type != FIX);
            break;
        case VAR:
            base_ret = read_op(base_cursor, NEXT, NULL);
            testutil_assert(base_ret == 0 || base_ret == WT_NOTFOUND);
            if (base_ret == 0)
                testutil_check(base_cursor->get_key(base_cursor, &base_keyno));
            break;
        case ROW:
            base_ret = table_mirror_row_next(base, base_cursor, &base_key, &base_keyno);
            break;
        }

        switch (table->type) {
        case FIX:
            /*
             * RS and VLCS skip over removed entries, FLCS returns a value of 0. Skip to the next
             * matching key number or the next nonzero value. If the latter comes early, we'll visit
             * the mismatch logic below.
             */
            for (;;) {
                table_ret = read_op(table_cursor, NEXT, NULL);
                testutil_assert(table_ret == 0 || table_ret == WT_NOTFOUND);
                if (table_ret != 0)
                    break;
                testutil_check(table_cursor->get_key(table_cursor, &table_keyno));
                if (table_keyno >= base_keyno || table_keyno > TV(RUNS_ROWS))
                    break;
                testutil_check(table_cursor->get_value(table_cursor, &table_bitv));
                if (table_bitv != 0)
                    break;
            }
            break;
        case VAR:
            table_ret = read_op(table_cursor, NEXT, NULL);
            testutil_assert(table_ret == 0 || table_ret == WT_NOTFOUND);
            if (table_ret == 0)
                testutil_check(table_cursor->get_key(table_cursor, &table_keyno));
            break;
        case ROW:
            table_ret = table_mirror_row_next(table, table_cursor, &table_key, &table_keyno);
            break;
        }

        /*
         * Tables run out of keys at different times as RS inserts between the initial table rows
         * and VLCS/FLCS inserts after the initial table rows. There's not much to say about the
         * relationships between them (especially as we skip deleted rows in RS and VLCS, so our
         * last successful check can be before the end of the original rows). If we run out of keys,
         * we're done. If both keys are past the end of the original keys, we're done. There are
         * some potential problems we're not going to catch at the end of the original rows, but
         * those problems should also appear in the middle of the tree.
         *
         * If we have two key/value pairs from the original rows, assert the keys have the same key
         * number (the keys themselves won't match), and keys are larger than or equal to the
         * counter. If the counter is smaller than the keys, that means rows were deleted, which is
         * expected.
         */
        if (base_ret == WT_NOTFOUND || table_ret == WT_NOTFOUND)
            break;
        if (base_keyno > TV(RUNS_ROWS) && table_keyno > TV(RUNS_ROWS))
            break;
        testutil_assert(rows <= base_keyno);
        rows = base_keyno;

        testutil_check(base_cursor->get_value(base_cursor, &base_value));
        if (table->type == FIX) {
            val_to_flcs(table, &base_value, &base_bitv);
            testutil_check(table_cursor->get_value(table_cursor, &table_bitv));

            if (base_keyno != table_keyno || base_bitv != table_bitv) {
                table_mirror_fail_msg_flcs(session, checkpoint, base, base_keyno, &base_key,
                  &base_value, base_bitv, table, table_keyno, table_bitv);
                goto page_dump;
            }
        } else {
            testutil_check(table_cursor->get_value(table_cursor, &table_value));

            if (base_keyno != table_keyno || base_value.size != table_value.size ||
              (table_value.size != 0 &&
                memcmp(base_value.data, table_value.data, base_value.size) != 0)) {
                table_mirror_fail_msg(session, checkpoint, base, base_keyno, &base_key, &base_value,
                  table, table_keyno, &table_key, &table_value, last_match);

page_dump:
                /* Dump the cursor pages for the first failure. */
                if (++failures == 1) {
                    testutil_check(__wt_snprintf(
                      tagbuf, sizeof(tagbuf), "mirror error: base cursor (table %u)", base->id));
                    cursor_dump_page(base_cursor, tagbuf);
                    testutil_check(__wt_snprintf(
                      tagbuf, sizeof(tagbuf), "mirror error: table cursor (table %u)", table->id));
                    cursor_dump_page(table_cursor, tagbuf);
                    for (i = 1; i <= ntables; ++i) {
                        if (!tables[i]->mirror)
                            continue;
                        if (tables[i] != base &&
                          (tables[i] != table || table_keyno != base_keyno)) {
                            testutil_check(__wt_snprintf(tagbuf, sizeof(tagbuf),
                              "mirror error: base key number %" PRIu64 " in table %u", base_keyno,
                              i));
                            table_dump_page(session, checkpoint, tables[i], base_keyno, tagbuf);
                        }
                        if (tables[i] != table && table_keyno != base_keyno) {
                            testutil_check(__wt_snprintf(tagbuf, sizeof(tagbuf),
                              "mirror error: table key number %" PRIu64 " in table %u", table_keyno,
                              i));
                            table_dump_page(session, checkpoint, tables[i], table_keyno, tagbuf);
                        }
                    }
                }

                /*
                 * We can't continue if the keys don't match, otherwise, optionally continue showing
                 * failures, up to 20.
                 */
                testutil_assert(base_keyno == table_keyno ||
                  (FLD_ISSET(g.trace_flags, TRACE_MIRROR_FAIL) && failures < 20));
            }
        }

        /* Report progress (unless verifying checkpoints which happens during live operations). */
        if (checkpoint == NULL &&
          ((rows < (5 * WT_THOUSAND) && rows % 10 == 0) || rows % (5 * WT_THOUSAND) == 0))
            track(buf, rows);
        /*
         * Failures in methods using record numbers may match on the key even after reset. Only
         * update the last successful matched key if we didn't have a failure or at least one table
         * is not using record numbers.
         */
        if (last_failures == failures && (base->type == ROW || table->type == ROW))
            last_match = base_keyno;
    }
    testutil_assert(failures == 0);

    trace_msg(session, "%s: stop", buf);
    wt_wrap_close_session(session);
}

/*
 * wts_verify --
 *     Verify the database tables.
 */
void
wts_verify(WT_CONNECTION *conn, bool mirror_check)
{
    WT_SESSION *session;
    u_int i;

    if (GV(OPS_VERIFY) == 0)
        return;

    /*
     * Individual object verification. Do a full checkpoint to reduce the possibility of returning
     * EBUSY from the following verify calls.
     */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->checkpoint(session, NULL));
    testutil_check(session->close(session, NULL));
    tables_apply(table_verify, conn);

    /*
     * Optionally compare any mirrored objects. If this is a reopen, check and see if salvage was
     * tested on the database. In that case, we can't do mirror verification because salvage will
     * have modified some rows leading to failure.
     */
    if (!mirror_check || g.base_mirror == NULL)
        return;

    if (g.reopen && GV(OPS_SALVAGE)) {
        WARN("%s", "skipping mirror verify on reopen because salvage testing was done");
        return;
    }

    for (i = 1; i <= ntables; ++i)
        if (tables[i]->mirror && tables[i] != g.base_mirror)
            table_verify_mirror(conn, g.base_mirror, tables[i], NULL);
}

/*
 * wts_verify_checkpoint --
 *     Verify the database tables at a checkpoint.
 */
void
wts_verify_checkpoint(WT_CONNECTION *conn, const char *checkpoint)
{
    u_int i;

    if (GV(OPS_VERIFY) == 0)
        return;

    if (g.base_mirror == NULL)
        return;

    for (i = 1; i <= ntables; ++i)
        if (tables[i]->mirror && tables[i] != g.base_mirror)
            table_verify_mirror(conn, g.base_mirror, tables[i], checkpoint);
}
