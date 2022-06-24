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
#include "util.h"
#include <dirent.h>

#define ALIGN_UP(p, n) ((p) % (n) == 0 ? (p) : ((p) + (n) - ((p) % (n))))
#define ALIGN_DOWN(p, n) ((p) - ((p) % (n)))

/*
 * util.c
 * 	Utility functions for test that simulates system crashes.
 */
#define COPY_BUF_SIZE ((size_t)(64 * 1024))

/*
 * copy_directory_int --
 *     Copy a directory, using direct IO if indicated. Recursive internal function. Assumes all
 *     cleanup has already happened at the destination.
 */
static void
copy_directory_int(const char *fromdir, const char *todir, bool directio)
{
    struct dirent *dp;
    struct stat sb;
    DIR *dirp;
    size_t blksize, bufsize, readbytes, n, remaining;
    ssize_t ioret;
    uintptr_t bufptr;
    int openflags, rfd, wfd;
    u_char *buf, *orig_buf;
    char fromfile[4096], tofile[4096];

#ifdef O_DIRECT
    openflags = directio ? O_DIRECT : 0;
#else
    testutil_assert(!directio);
    openflags = 0;
#endif
    orig_buf = dcalloc(COPY_BUF_SIZE, sizeof(u_char));
    buf = NULL;
    blksize = bufsize = 0;

    testutil_check(mkdir(todir, 0777));
    dirp = opendir(fromdir);
    testutil_assert(dirp != NULL);

    while ((dp = readdir(dirp)) != NULL) {
        /*
         * Skip . and ..
         */
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
            continue;
        testutil_check(__wt_snprintf(fromfile, sizeof(fromfile), "%s/%s", fromdir, dp->d_name));
        testutil_check(__wt_snprintf(tofile, sizeof(tofile), "%s/%s", todir, dp->d_name));
        if (dp->d_type == DT_DIR) {
            copy_directory_int(fromfile, tofile, directio);
            continue;
        }

        rfd = open(fromfile, O_RDONLY | openflags, 0);
        /*
         * The child process may have been stopped during a drop and WiredTiger drop will do an
         * unlink call followed by syncing the directory. It is possible for the signal to have been
         * delivered in between those calls so the file may no longer exist but reading the
         * directory will still return its entry. Handle that case and skip the file if it happens.
         */
        if (rfd < 0 && errno == ENOENT) {
            printf("COPY_DIR: direct:%d ENOENT Source file %s not found.\n", directio, dp->d_name);
            continue;
        }
        testutil_assertfmt(rfd >= 0, "Open of source %s failed with %d\n", fromfile, errno);
        wfd = open(tofile, O_WRONLY | O_CREAT, 0666);
        testutil_assertfmt(wfd >= 0, "Open of dest %s failed with %d\n", tofile, errno);
        testutil_check(fstat(rfd, &sb));

        /*
         * Do any alignment on the buffer required for direct IO.
         */
        if (buf == NULL) {
            if (directio) {
                blksize = (size_t)WT_MAX(sb.st_blksize, WT_BUFFER_ALIGNMENT_DEFAULT);
                testutil_assert(blksize < COPY_BUF_SIZE);
                /*
                 * Make sure we have plenty of room for adjusting the pointer.
                 */
                bufsize = COPY_BUF_SIZE - blksize;
                bufptr = (uintptr_t)orig_buf;
                /* Align pointer up to next block boundary */
                buf = (u_char *)ALIGN_UP(bufptr, blksize);
                /* Align size down to block boundary */
                testutil_assert(bufsize >= blksize);
                bufsize = ALIGN_DOWN(bufsize, blksize);
            } else {
                buf = orig_buf;
                bufsize = COPY_BUF_SIZE;
            }
        } else if (directio)
            testutil_assert(blksize == (size_t)sb.st_blksize);
        remaining = (size_t)sb.st_size;
        while (remaining > 0) {
            readbytes = n = WT_MIN(remaining, bufsize);
            /*
             * When using direct IO, read sizes must also be a multiple of the block size. For the
             * last block of a file, we must request to read the entire block, and we'll get the
             * remainder back.
             */
            if (directio)
                readbytes = ALIGN_UP(n, blksize);
            ioret = read(rfd, buf, readbytes);
            testutil_assert(ioret >= 0 && (size_t)ioret == n);
            ioret = write(wfd, buf, n);
            testutil_assert(ioret >= 0 && (size_t)ioret == n);
            remaining -= n;
        }
        testutil_check(close(rfd));
        testutil_check(close(wfd));
    }
    testutil_check(closedir(dirp));
    free(orig_buf);
}

/*
 * clean_directory --
 *     Clean up a directory, use system to remove sub-directories too.
 */
static void
clean_directory(const char *todir)
{
    int status;
    char buf[512];

    testutil_check(__wt_snprintf(buf, sizeof(buf), "rm -rf %s", todir));
    if ((status = system(buf)) < 0)
        testutil_die(status, "system: %s", buf);
}

/*
 * copy_directory --
 *     Copy a directory, using direct IO if indicated. Wrapper because the sub functions can be
 *     called recursively if there are sub-directories present.
 */
void
copy_directory(const char *fromdir, const char *todir, bool directio)
{
    clean_directory(todir);
    copy_directory_int(fromdir, todir, directio);
}
