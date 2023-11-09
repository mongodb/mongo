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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

extern "C" {
#include "test_util.h"
}

#include "model/test/util.h"

/*
 * create_tmp_file --
 *     Create an empty temporary file and return its name.
 */
std::string
create_tmp_file(const char *dir, const char *prefix, const char *suffix)
{
    size_t dir_len = dir ? strlen(dir) + strlen(DIR_DELIM_STR) : 0;
    size_t prefix_len = strlen(prefix);
    size_t suffix_len = suffix ? strlen(suffix) : 0;
    size_t buf_len = dir_len + prefix_len + 6 + suffix_len + 4;

    char *buf = (char *)alloca(buf_len);
    testutil_snprintf(buf, buf_len, "%s%s%sXXXXXX%s", dir ? dir : "", dir ? DIR_DELIM_STR : "",
      prefix, suffix ? suffix : "");

    /* This will also create the file - we only care about a name, but this is ok. */
    int fd = mkstemps(buf, suffix_len);
    testutil_assert_errno(fd > 0);
    testutil_check(close(fd));

    return std::string(buf);
}
