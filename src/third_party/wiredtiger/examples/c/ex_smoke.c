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
 * ex_smoke.c
 *	A simple program you can build to prove include files and libraries
 * are linking correctly.
 */
#include <stdlib.h>

#include <wiredtiger.h>

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    int ret;

    (void)argc; /* Unused variable */

    /*
     * This code deliberately doesn't use the standard test_util macros, we don't want to link
     * against that code to smoke-test a build.
     */
    if ((ret = system("rm -rf WT_HOME && mkdir WT_HOME")) != 0) {
        fprintf(stderr, "Failed to clean up prior to running example.\n");
        return (EXIT_FAILURE);
    }

    /* Open a connection to the database, creating it if necessary. */
    if ((ret = wiredtiger_open("WT_HOME", NULL, "create", &conn)) != 0) {
        fprintf(stderr, "%s: wiredtiger_open: %s\n", argv[0], wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }

    /* Close the connection to the database. */
    if ((ret = conn->close(conn, NULL)) != 0) {
        fprintf(stderr, "%s: WT_CONNECTION.close: %s\n", argv[0], wiredtiger_strerror(ret));
        return (EXIT_FAILURE);
    }

    return (EXIT_SUCCESS);
}
