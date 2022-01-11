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

#include <stdlib.h>

#include <unistd.h> // TODO
#include <fcntl.h>  // TODO
#include <wt_internal.h>

static void fail(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * fail --
 *     TODO: Add a comment describing this function.
 */
static void
fail(int ret)
{
    fprintf(stderr, "%s: %d (%s)\n", "wt2336_fileop_basic", ret, wiredtiger_strerror(ret));
    exit(ret);
}

#define SEPARATOR "--------------"

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    int ret;

    (void)argc;
    (void)argv;
    fprintf(stderr, SEPARATOR "wiredtiger_open\n");
    if ((ret = wiredtiger_open(".", NULL, "create", &conn)) != 0)
        fail(ret);

    usleep(100);
    fflush(stderr);
    fprintf(stderr, SEPARATOR "open_session\n");
    fflush(stderr);

    if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
        fail(ret);

    usleep(100);
    fflush(stderr);
    fprintf(stderr, SEPARATOR "create\n");
    fflush(stderr);

    if ((ret = session->create(session, "table:hello", "key_format=S,value_format=S")) != 0)
        fail(ret);

    usleep(100);
    fprintf(stderr, SEPARATOR "rename\n");

    if ((ret = session->rename(session, "table:hello", "table:world", NULL)) != 0)
        fail(ret);

    fflush(stdout);
    fprintf(stderr, SEPARATOR "drop\n");
    fflush(stdout);

    if ((ret = session->drop(session, "table:world", NULL)) != 0)
        fail(ret);

    fprintf(stderr, SEPARATOR "WT_CONNECTION::close\n");

    if ((ret = conn->close(conn, NULL)) != 0)
        fail(ret);

    return (0);
}
