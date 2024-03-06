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

#include <sys/time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

extern "C" {
#include "test_util.h"
}

#include "model/driver/debug_log_parser.h"
#include "model/test/util.h"
#include "model/test/wiredtiger_util.h"
#include "model/util.h"

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

/*
 * current_time --
 *     Get the current time in seconds.
 */
double
current_time()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return tv.tv_sec + tv.tv_usec / 1.0e6;
}

/*
 * parse_uint64_range --
 *     Parse the string into a range of numbers (two numbers separated by '-'). Throw an exception
 *     on error.
 */
std::pair<uint64_t, uint64_t>
parse_uint64_range(const char *str)
{
    char *end;
    uint64_t first = parse_uint64(str, &end);

    uint64_t second = first;
    if (end[0] != '\0') {
        if (end[0] != '-')
            throw std::runtime_error("Not a range");
        second = parse_uint64(end + 1);
    }

    if (first > second)
        std::swap(first, second);
    return std::make_pair(first, second);
}

/*
 * trim --
 *     Trim whitespace from a string.
 */
std::string
trim(const std::string &str, const std::string &to_trim)
{
    size_t a = str.find_first_not_of(to_trim);
    if (a == std::string::npos)
        return "";
    size_t b = str.find_last_not_of(to_trim);
    if (b == std::string::npos || b < a)
        b = str.length();
    return str.substr(a, b - a + 1);
}

/*
 * verify_using_debug_log --
 *     Verify the database using the debug log. Try both the regular and the JSON version.
 */
void
verify_using_debug_log(TEST_OPTS *opts, const char *home, bool test_failing)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;

    testutil_wiredtiger_open(
      opts, home, "log=(enabled,file_max=10M,remove=false)", nullptr, &conn, false, false);
    testutil_check(conn->open_session(conn, nullptr, nullptr, &session));
    std::vector<std::string> tables = model::wt_list_tables(conn);

    /* Verify using the debug log. */
    model::kv_database db_from_debug_log;
    model::debug_log_parser::from_debug_log(db_from_debug_log, conn);
    for (auto &t : tables)
        testutil_assert(db_from_debug_log.table(t.c_str())->verify_noexcept(conn));

    /*
     * Print the debug log to JSON. Note that the debug log has not changed from above, because each
     * database can be opened by only one WiredTiger instance at a time.
     */
    std::string tmp_json = create_tmp_file(home, "debug-log-", ".json");
    wt_print_debug_log(conn, tmp_json.c_str());

    /* Verify again using the debug log JSON. */
    model::kv_database db_from_debug_log_json;
    model::debug_log_parser::from_json(db_from_debug_log_json, tmp_json.c_str());
    for (auto &t : tables)
        testutil_assert(db_from_debug_log_json.table(t.c_str())->verify_noexcept(conn));

    /* Now try to get the verification to fail, just to make sure it's working. */
    if (test_failing)
        for (auto &t : tables) {
            model::data_value key("A key that does not exists");
            model::data_value value("A data value");
            model::kv_table_ptr p = db_from_debug_log.table(t.c_str());
            p->insert(key, value, 100 * 1000);
            testutil_assert(!p->verify_noexcept(conn));
        }

    /* Clean up. */
    testutil_check(session->close(session, nullptr));
    testutil_check(conn->close(conn, nullptr));
}

/*
 * verify_workload --
 *     Verify the workload by running it in both the model and WiredTiger.
 */
void
verify_workload(const model::kv_workload &workload, TEST_OPTS *opts, const std::string &home,
  const char *env_config)
{
    /* Run the workload in the model. */
    model::kv_database database;
    workload.run(database);

    /* When we load the workload from WiredTiger, that would be after running recovery. */
    database.restart();

    /* Run the workload in WiredTiger. */
    testutil_recreate_dir(home.c_str());
    workload.run_in_wiredtiger(home.c_str(), env_config);

    /* Open the database that we just created. */
    WT_CONNECTION *conn;
    testutil_wiredtiger_open(opts, home.c_str(), env_config, nullptr, &conn, false, false);

    /* Verify. */
    std::vector<std::string> tables = model::wt_list_tables(conn);
    for (auto &t : tables)
        testutil_assert(database.table(t.c_str())->verify_noexcept(conn));

    /* Clean up. */
    testutil_check(conn->close(conn, nullptr));
}
