/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <cstdio>
#include <string>
#include <stdexcept>

#include <catch2/catch.hpp>

#include "utils.h"

namespace utils {

/*
 * throw_if_non_zero --
 *     Test result. Throw if it is non-zero.
 *
 * @param result The value to test.
 */
void
throw_if_non_zero(const int result)
{
    if (result != 0)
        throw std::runtime_error("Error result is " + std::to_string(result));
}

/*
 * remove_wrapper --
 *     Delete a file or a directory.
 *
 * @param path The pathname of the file or directory to delete.
 */
static int
remove_wrapper(std::string const &path)
{
    return std::remove(path.c_str());
}

/*
 * wiredtiger_cleanup --
 *     Delete WiredTiger files in directory home and directory home.
 *
 * @param home The directory that contains WiredTiger files.
 */
void
wiredtiger_cleanup(std::string const &home)
{
    // Ignoring errors here; we don't mind if something doesn't exist.
    remove_wrapper(home + "/WiredTiger");
    remove_wrapper(home + "/WiredTiger.basecfg");
    remove_wrapper(home + "/WiredTiger.lock");
    remove_wrapper(home + "/WiredTiger.turtle");
    remove_wrapper(home + "/WiredTiger.wt");
    remove_wrapper(home + "/WiredTigerHS.wt");
    remove_wrapper(home + "/backup_test1.wt");
    remove_wrapper(home + "/backup_test2.wt");
    remove_wrapper(home + "/cursor_test.wt");
    remove_wrapper(home);
}

/*
 * break_here --
 *     Make it easier to set a breakpoint within a unit test.
 *
 * Functions generated for TEST_CASE() have undocumented names. Call break_here via BREAK at the
 *     start of a TEST_CASE() and then set a gdb breakpoint via "break break_here".
 */
void
break_here(const char *file, const char *func, int line)
{
    INFO(">> " << file << " line " << line << ": " << func);
}

/*
 * check_error_info --
 *     Perform checks on each of the three members of a WT_ERROR_INFO struct.
 */
void
check_error_info(WT_ERROR_INFO *err_info, int err, int sub_level_err, const char *err_msg_content)
{
    CHECK(err_info->err == err);
    CHECK(err_info->sub_level_err == sub_level_err);
    CHECK(strcmp(err_info->err_msg, err_msg_content) == 0);
}
} // namespace utils.
