/*
 * Copyright (c) 2020 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "rnp_tests.h"
#include "file-utils.h"

TEST_F(rnp_tests, test_rnp_mkstemp)
{
#ifdef _WIN32
    const char  tmpl[17] = "test-file.XXXXXX";
    char        buf[17];
    const int   size = 20;
    int         fds[size];
    std::string filenames[size];
    for (int i = 0; i < size; i++) {
        memcpy(buf, tmpl, sizeof(buf));
        int fd = rnp_mkstemp(buf);
        assert_int_not_equal(-1, fd);
        fds[i] = fd;
        filenames[i] = buf;
    }
    assert_false(filenames[0] == filenames[1]);
    for (int i = 0; i < size; i++) {
        if (fds[i] != -1) {
            assert_int_equal(0, close(fds[i]));
            assert_int_equal(0, unlink(filenames[i].c_str()));
        }
    }
#endif
}

TEST_F(rnp_tests, test_rnp_access)
{
#ifdef _WIN32
    /* Assume we are running as non-Administrator user */
    assert_int_equal(0, rnp_access("C:\\Windows", F_OK));
    assert_int_equal(0, rnp_access("C:\\Windows", R_OK));
    assert_int_equal(0, rnp_access("C:\\Windows", W_OK));
    assert_int_equal(0, rnp_access("C:\\Windows\\System32\\User32.dll", F_OK));
    assert_int_equal(0, rnp_access("C:\\Windows\\System32\\User32.dll", R_OK));
    /* Should fail, but unfortunately _waccess() works this way */
    assert_int_equal(0, rnp_access("C:\\Windows\\System32\\User32.dll", W_OK));
#else
    /* Assume we are running as non-root and root directory is non-writeable for us */
    assert_int_equal(0, rnp_access("/", F_OK));
    assert_int_equal(0, rnp_access("/", R_OK));
    assert_int_equal(-1, rnp_access("/", W_OK));
    assert_int_equal(0, rnp_access("/tmp", R_OK | W_OK));
#endif
}
