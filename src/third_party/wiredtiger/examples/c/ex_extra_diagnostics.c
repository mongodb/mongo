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
 * ex_extra_diagnostics.c
 *	This is an example demonstrating how to enable and update extra diagnostic
 *  code at runtime.
 */
#include <test_util.h>

static const char *home;

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;

    home = example_setup(argc, argv);

#ifdef HAVE_DIAGNOSTIC
    /*
     * In diagnostic mode diagnostics are always enabled. Attempting to configure them will return
     * an error.
     */
    testutil_assert(
      wiredtiger_open(home, NULL, "create,extra_diagnostics=[key_out_of_order]", &conn) == EINVAL);
#else
    /*! [Configure extra_diagnostics] */
    /* Open a connection to the database, enabling key ordering checks. */
    error_check(wiredtiger_open(home, NULL, "create,extra_diagnostics=[key_out_of_order]", &conn));

    /*
     * Reconfigure the connection to turn on transaction visibility checks. As key_out_of_order is
     * not provided in the new configuration it is disabled.
     */
    error_check(conn->reconfigure(conn, "extra_diagnostics=[txn_visibility]"));
    /*! [Configure extra_diagnostics] */
#endif

    return (EXIT_SUCCESS);
}
