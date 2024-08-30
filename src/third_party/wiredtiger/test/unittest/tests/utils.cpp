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
 * throwIfNonZero --
 *     Test result. Throw if it is non-zero.
 *
 * @param result The value to test.
 */
void
throwIfNonZero(int result)
{
    if (result != 0) {
        std::string errorMessage("Error result is " + std::to_string(result));
        throw std::runtime_error(errorMessage);
    }
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
 * wiredtigerCleanup --
 *     Delete WiredTiger files in directory home and directory home.
 *
 * @param home The directory that contains WiredTiger files.
 */
void
wiredtigerCleanup(std::string const &home)
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
} // namespace utils.
