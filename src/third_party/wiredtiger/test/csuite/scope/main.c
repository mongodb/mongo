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
#include "test_util.h"

#define KEY "key"
#define VALUE "value,value,value"

static int ignore_errors;

/*
 * handle_error --
 *     TODO: Add a comment describing this function.
 */
static int
handle_error(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message)
{
    (void)(handler);

    /* Skip the error messages we're expecting to see. */
    if (ignore_errors > 0 &&
      (strstr(message, "requires key be set") != NULL ||
        strstr(message, "requires value be set") != NULL)) {
        --ignore_errors;
        return (0);
    }

    (void)fprintf(stderr, "%s: %s\n", message, session->strerror(session, error));
    return (0);
}

static WT_EVENT_HANDLER event_handler = {handle_error, NULL, NULL, NULL};

#define SET_KEY                                   \
    do {                                          \
        if (recno)                                \
            cursor->set_key(cursor, (uint64_t)1); \
        else {                                    \
            strcpy(keybuf, KEY);                  \
            cursor->set_key(cursor, keybuf);      \
        }                                         \
    } while (0)
#define SET_VALUE                                 \
    do {                                          \
        strcpy(valuebuf, VALUE);                  \
        if (vstring)                              \
            cursor->set_value(cursor, valuebuf);  \
        else {                                    \
            vu.size = strlen(vu.data = valuebuf); \
            cursor->set_value(cursor, &vu);       \
        }                                         \
    } while (0)

/*
 * cursor_scope_ops --
 *     TODO: Add a comment describing this function.
 */
static void
cursor_scope_ops(WT_SESSION *session, const char *uri)
{
    struct {
        const char *op;
        enum {
            INSERT_GET_KEY,
            INSERT_GET_VALUE,
            MODIFY,
            SEARCH,
            SEARCH_NEAR,
            REMOVE_GET_KEY,
            REMOVE_GET_VALUE,
            REMOVE_POS,
            RESERVE,
            UPDATE
        } func;
    } * op,
      ops[] = {/*
                * The ops order is specific: insert has to happen first so
                * other operations are possible, and remove has to be last.
                */
        {
          "insert",
          INSERT_GET_KEY,
        },
        {
          "insert",
          INSERT_GET_VALUE,
        },
        {
          "search",
          SEARCH,
        },
        {
          "search",
          SEARCH_NEAR,
        },
        {
          "reserve",
          RESERVE,
        },
        {
          "insert",
          MODIFY,
        },
        {
          "update",
          UPDATE,
        },
        {
          "remove",
          REMOVE_GET_KEY,
        },
        {
          "remove",
          REMOVE_GET_VALUE,
        },
        {
          "remove",
          REMOVE_POS,
        },
        {NULL, INSERT_GET_KEY}};
    WT_CURSOR *cursor;
#define MODIFY_ENTRIES 2
    WT_MODIFY entries[MODIFY_ENTRIES];
    WT_ITEM vu;
    uint64_t keyr;
    const char *key, *vs;
    char keybuf[100], valuebuf[100];
    int exact;
    bool recno, rollback, vstring;

    cursor = NULL;
    for (op = ops; op->op != NULL; op++) {
        /* Open a cursor, track key/value formats. */
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
        recno = strcmp(cursor->key_format, "r") == 0;
        vstring = strcmp(cursor->value_format, "S") == 0;

        /* Remove any leftover key/value pair, start fresh. */
        SET_KEY;
        testutil_check(cursor->remove(cursor));

        /* If not an insert operation, make sure there's a key/value pair to operate on. */
        if (op->func != INSERT_GET_KEY && op->func != INSERT_GET_VALUE) {
            SET_KEY;
            SET_VALUE;
            testutil_check(cursor->insert(cursor));
        }
        /* Discard that cursor, we'll open one inside the transaction. */
        testutil_check(cursor->close(cursor));

        /* Modify and reserve require a transaction, modify requires snapshot isolation. */
        testutil_check(session->begin_transaction(session, "isolation=snapshot"));
        rollback = false;

        /* Open a cursor, track key/value formats. */
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
        recno = strcmp(cursor->key_format, "r") == 0;
        vstring = strcmp(cursor->value_format, "S") == 0;

        /*
         * Set up application buffers so we can detect overwrites or failure to copy application
         * information into library memory.
         */
        SET_KEY;
        SET_VALUE;

        /*
         * The application must keep key and value memory valid until the next operation that
         * positions the cursor, modifies the data, or resets or closes the cursor.
         *
         * Modifying either the key or value buffers is not permitted.
         */
        switch (op->func) {
        case INSERT_GET_KEY:
        case INSERT_GET_VALUE:
            testutil_check(cursor->insert(cursor));
            break;
        case MODIFY:
            /* Modify, but don't really change anything. */
            entries[0].data.data = &VALUE[0];
            entries[0].data.size = 2;
            entries[0].offset = 0;
            entries[0].size = 2;
            entries[1].data.data = &VALUE[3];
            entries[1].data.size = 5;
            entries[1].offset = 3;
            entries[1].size = 5;

            testutil_check(cursor->modify(cursor, entries, MODIFY_ENTRIES));
            break;
        case SEARCH:
            testutil_check(cursor->search(cursor));
            break;
        case SEARCH_NEAR:
            testutil_check(cursor->search_near(cursor, &exact));
            break;
        case REMOVE_POS:
            /*
             * Remove has two modes, one where the remove is based on a cursor position, the other
             * where it's based on a set key. The results are different, so test them separately.
             */
            testutil_check(cursor->search(cursor));
        /* FALLTHROUGH */
        case REMOVE_GET_KEY:
        case REMOVE_GET_VALUE:
            testutil_check(cursor->remove(cursor));
            break;
        case RESERVE:
            testutil_check(cursor->reserve(cursor));
            break;
        case UPDATE:
            testutil_check(cursor->update(cursor));
            break;
        }

        /*
         * The cursor should no longer reference application memory, and application buffers can be
         * safely overwritten.
         */
        memset(keybuf, 'K', sizeof(keybuf));
        memset(valuebuf, 'V', sizeof(valuebuf));

        /*
         * Check that get_key/get_value behave as expected after the operation.
         */
        key = vs = NULL;
        keyr = 37;
        memset(&vu, 0, sizeof(vu));
        switch (op->func) {
        case INSERT_GET_KEY:
        case REMOVE_GET_KEY:
            /*
             * Insert and remove configured with a search key do not position the cursor and have no
             * key or value.
             *
             * There should be two error messages, ignore them, and errors require rollback.
             */
            ignore_errors = 1;
            rollback = true;
            if (recno)
                testutil_assert(cursor->get_key(cursor, &keyr) != 0);
            else
                testutil_assert(cursor->get_key(cursor, &key) != 0);
            testutil_assert(ignore_errors == 0);
            break;
        case INSERT_GET_VALUE:
        case REMOVE_GET_VALUE:
            /*
             * Insert and remove configured with a search key do not position the cursor and have no
             * key or value.
             *
             * There should be two error messages, ignore them, and errors require rollback.
             */
            ignore_errors = 1;
            rollback = true;
            if (vstring)
                testutil_assert(cursor->get_value(cursor, &vs) != 0);
            else
                testutil_assert(cursor->get_value(cursor, &vu) != 0);
            testutil_assert(ignore_errors == 0);
            break;
        case REMOVE_POS:
            /*
             * Remove configured with a cursor position has a key, but no value.
             *
             * There should be one error message, ignore it, and errors require rollback.
             */
            if (recno) {
                testutil_assert(cursor->get_key(cursor, &keyr) == 0);
                testutil_assert(keyr == 1);
            } else {
                testutil_assert(cursor->get_key(cursor, &key) == 0);
                testutil_assert(key != keybuf);
                testutil_assert(strcmp(key, KEY) == 0);
            }
            ignore_errors = 1;
            rollback = true;
            if (vstring)
                testutil_assert(cursor->get_value(cursor, &vs) != 0);
            else
                testutil_assert(cursor->get_value(cursor, &vu) != 0);
            testutil_assert(ignore_errors == 0);
            break;
        case MODIFY:
        case RESERVE:
        case SEARCH:
        case SEARCH_NEAR:
        case UPDATE:
            /*
             * Modify, reserve, search, search-near and update all position the cursor and have both
             * a key and value.
             *
             * Any key/value should not reference application memory.
             */
            if (recno) {
                testutil_assert(cursor->get_key(cursor, &keyr) == 0);
                testutil_assert(keyr == 1);
            } else {
                testutil_assert(cursor->get_key(cursor, &key) == 0);
                testutil_assert(key != keybuf);
                testutil_assert(strcmp(key, KEY) == 0);
            }
            if (vstring) {
                testutil_assert(cursor->get_value(cursor, &vs) == 0);
                testutil_assert(vs != valuebuf);
                testutil_assert(strcmp(vs, VALUE) == 0);
            } else {
                testutil_assert(cursor->get_value(cursor, &vu) == 0);
                testutil_assert(vu.data != valuebuf);
                testutil_assert(vu.size == strlen(VALUE));
                testutil_assert(memcmp(vu.data, VALUE, strlen(VALUE)) == 0);
            }
            break;
        }

        if (rollback)
            testutil_check(session->rollback_transaction(session, NULL));
        else
            testutil_check(session->commit_transaction(session, NULL));
        testutil_check(cursor->close(cursor));
    }
}

/*
 * run --
 *     TODO: Add a comment describing this function.
 */
static void
run(WT_CONNECTION *conn, const char *uri, const char *config)
{
    WT_SESSION *session;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri, config));
    cursor_scope_ops(session, uri);
    testutil_check(session->close(session, NULL));
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, &event_handler, "create", &opts->conn));

    run(opts->conn, "file:file.SS", "key_format=S,value_format=S");
    run(opts->conn, "file:file.Su", "key_format=S,value_format=u");
    run(opts->conn, "file:file.rS", "key_format=r,value_format=S");
    run(opts->conn, "file:file.ru", "key_format=r,value_format=u");

    run(opts->conn, "lsm:lsm.SS", "key_format=S,value_format=S");
    run(opts->conn, "lsm:lsm.Su", "key_format=S,value_format=u");

    run(opts->conn, "table:table.SS", "key_format=S,value_format=S");
    run(opts->conn, "table:table.Su", "key_format=S,value_format=u");
    run(opts->conn, "table:table.rS", "key_format=r,value_format=S");
    run(opts->conn, "table:table.ru", "key_format=r,value_format=u");

    testutil_cleanup(opts);

    return (EXIT_SUCCESS);
}
