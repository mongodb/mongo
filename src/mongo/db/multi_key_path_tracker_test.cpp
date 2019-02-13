/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * Unittest for MultikeyPathTracker operations.
 */

#include "mongo/platform/basic.h"

#include <sstream>

#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
    std::stringstream ss;

    ss << "[ ";
    for (const auto multikeyComponents : multikeyPaths) {
        ss << "[ ";
        for (const auto multikeyComponent : multikeyComponents) {
            ss << multikeyComponent << " ";
        }
        ss << "] ";
    }
    ss << "]";

    return ss.str();
}

void assertMultikeyPathsAreEqual(const MultikeyPaths& actual, const MultikeyPaths& expected) {
    bool match = (expected == actual);
    if (!match) {
        FAIL(str::stream() << "Expected: " << dumpMultikeyPaths(expected) << ", "
                           << "Actual: "
                           << dumpMultikeyPaths(actual));
    }
    ASSERT(match);
}

TEST(MultikeyPathTracker, TestMergeMultikeyPaths) {
    // Suppose the index key is {"a.c": 1, "a.b": 1, "c.d.b.e": 1}.
    MultikeyPaths mutablePaths = {{}, {}, {}};
    {
        // `foundPaths` finds `a` to be multikey.
        MultikeyPaths foundPaths = {{0}, {0}, {}};
        MultikeyPathTracker::mergeMultikeyPaths(&mutablePaths, foundPaths);
        assertMultikeyPathsAreEqual(mutablePaths, foundPaths);
    }

    {
        // `foundPaths` finds `c` and `d` to be multikey.
        MultikeyPaths foundPaths = {{1}, {}, {0, 1}};
        MultikeyPathTracker::mergeMultikeyPaths(&mutablePaths, foundPaths);
        assertMultikeyPathsAreEqual(mutablePaths, {{0, 1}, {0}, {0, 1}});
    }

    {
        // `foundPaths` finds `b` to be multikey.
        MultikeyPaths foundPaths = {{}, {1}, {2}};
        MultikeyPathTracker::mergeMultikeyPaths(&mutablePaths, foundPaths);
        assertMultikeyPathsAreEqual(mutablePaths, {{0, 1}, {0, 1}, {0, 1, 2}});
    }
}
}  // namespace
}  // namespace mongo
