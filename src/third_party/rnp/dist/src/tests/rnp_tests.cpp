/*
 * Copyright (c) 2017-2019 [Ribose Inc](https://www.ribose.com).
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

#include "gtest/gtest.h"
#include <crypto/rng.h>
#include "rnp_tests.h"
#include "support.h"
#ifdef _WIN32
#include <cstdlib>
#include <crtdbg.h>
#endif

static char original_dir[PATH_MAX];

#ifdef _WIN32
void
rnpInvalidParameterHandler(const wchar_t *expression,
                           const wchar_t *function,
                           const wchar_t *file,
                           unsigned int   line,
                           uintptr_t      pReserved)
{
    wprintf(L"%s:%d %s: invalid param: %s.\n", file, line, function, expression);
}
#endif

rnp_tests::rnp_tests() : m_dir(make_temp_dir())
{
#ifdef _WIN32
    _invalid_parameter_handler handler = rnpInvalidParameterHandler;
    _set_invalid_parameter_handler(handler);
    _CrtSetReportMode(_CRT_ASSERT, 0);
#endif
    /* We use LOGNAME in a few places within the tests
     * and it isn't always set in every environment.
     */
    if (!getenv("LOGNAME")) {
        setenv("LOGNAME", "test-user", 1);
    }
    EXPECT_EQ(0, setenv("HOME", m_dir, 1));
    EXPECT_EQ(0, chdir(m_dir));
    /* fully specified path works correctly here with cp and xcopy */
    std::string data_str = std::string(m_dir) + "/data";
    copy_recursively(getenv("RNP_TEST_DATA"), data_str.c_str());
}

rnp_tests::~rnp_tests()
{
    clean_temp_dir(m_dir);
    free(m_dir);
}

const char *
rnp_tests::original_dir() const
{
    return ::original_dir;
}

int
main(int argc, char *argv[])
{
    EXPECT_NE(nullptr, getcwd(original_dir, sizeof(original_dir)));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
