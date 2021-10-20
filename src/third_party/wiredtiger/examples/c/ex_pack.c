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
 *
 * ex_pack.c
 *	This is an example demonstrating basic packing and unpacking of fields.
 */
#include <test_util.h>

static const char *home;

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    int i, j, k;

    home = example_setup(argc, argv);

    /* Open a connection to the database, creating it if necessary. */
    error_check(wiredtiger_open(home, NULL, "create", &conn));

    /* Open a session for the current thread's work. */
    error_check(conn->open_session(conn, NULL, NULL, &session));

    {
        /*! [packing] */
        size_t size;
        char buf[50];

        error_check(wiredtiger_struct_size(session, &size, "iii", 42, 1000, -9));
        if (size > sizeof(buf)) {
            /* Allocate a bigger buffer. */
        }

        error_check(wiredtiger_struct_pack(session, buf, size, "iii", 42, 1000, -9));

        error_check(wiredtiger_struct_unpack(session, buf, size, "iii", &i, &j, &k));
        /*! [packing] */
    }

    /* Note: closing the connection implicitly closes open session(s). */
    error_check(conn->close(conn, NULL));

    return (EXIT_SUCCESS);
}
