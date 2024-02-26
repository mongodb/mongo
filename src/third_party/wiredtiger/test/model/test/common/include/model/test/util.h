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

#pragma once

#include <string>
#include "model/driver/kv_workload.h"

extern "C" {
#include "test_util.h"
}

/*
 * model_testutil_assert_exception --
 *     Assert that the given exception is thrown.
 */
#define model_testutil_assert_exception(call, exception)                                        \
    try {                                                                                       \
        call;                                                                                   \
        testutil_die(0, #call " did not throw " #exception);                                    \
    } catch (exception &) {                                                                     \
    } catch (...) {                                                                             \
        testutil_die(0, #call " did not throw " #exception "; it threw a different exception"); \
    }

/*
 * create_tmp_file --
 *     Create an empty temporary file and return its name.
 */
std::string create_tmp_file(const char *dir, const char *prefix, const char *suffix = nullptr);

/*
 * verify_using_debug_log --
 *     Verify the database using the debug log. Try both the regular and the JSON version.
 */
void verify_using_debug_log(TEST_OPTS *opts, const char *home, bool test_failing = false);

/*
 * verify_workload --
 *     Verify the workload by running it in both the model and WiredTiger.
 */
void verify_workload(const model::kv_workload &workload, TEST_OPTS *opts, const std::string &home,
  const char *env_config);
