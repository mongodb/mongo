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

#include "mongo/db/multi_key_path_tracker.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <boost/container/vector.hpp>

namespace mongo {
namespace {

void assertMultikeyPathsAreEqual(const MultikeyPaths& actual, const MultikeyPaths& expected) {
    bool match = (expected == actual);
    if (!match) {
        FAIL(std::string(str::stream()
                         << "Expected: " << MultikeyPathTracker::dumpMultikeyPaths(expected) << ", "
                         << "Actual: " << MultikeyPathTracker::dumpMultikeyPaths(actual)));
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

TEST(MultikeyPathTracker, TestSameIndexAndCollectionAtTime) {
    const auto uuid = UUID::gen();
    const auto ts1 = Timestamp(Seconds(1), 0);
    const auto ts2 = Timestamp(Seconds(2), 0);
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");

    MultikeyPathInfo base{nss, uuid, "a_1", {}, {}, ts1};
    MultikeyPathInfo sameIdentity{nss, uuid, "a_1", {}, {}, ts1};
    MultikeyPathInfo differentIndex{nss, uuid, "b_1", {}, {}, ts1};
    MultikeyPathInfo differentTimestamp{nss, uuid, "a_1", {}, {}, ts2};
    MultikeyPathInfo differentCollection{nss, UUID::gen(), "a_1", {}, {}, ts1};

    ASSERT(base.sameIndexAndCollectionAtTime(sameIdentity));
    ASSERT(!base.sameIndexAndCollectionAtTime(differentIndex));
    ASSERT(!base.sameIndexAndCollectionAtTime(differentTimestamp));
    ASSERT(!base.sameIndexAndCollectionAtTime(differentCollection));
}

TEST(MultikeyPathTracker, TestAddMultikeyPathInfoMergesSameIndexAndCollectionAtTime) {
    MultikeyPathTracker tracker;
    tracker.startTrackingMultikeyPathInfo();

    const auto uuid = UUID::gen();
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const auto ts = Timestamp(Seconds(1), 0);

    tracker.addMultikeyPathInfo({nss, uuid, "idx", {}, MultikeyPaths{{0U}}, ts});
    tracker.addMultikeyPathInfo({nss, uuid, "idx", {}, MultikeyPaths{{1U}}, ts});
    tracker.addMultikeyPathInfo(
        {nss, uuid, "idx", {}, MultikeyPaths{{2U}}, Timestamp(Seconds(2), 0)});

    ASSERT_EQ(2UL, tracker.getMultikeyPathInfo().size());
    assertMultikeyPathsAreEqual(tracker.getMultikeyPathInfo()[0].multikeyPaths, {{0U, 1U}});
    assertMultikeyPathsAreEqual(tracker.getMultikeyPathInfo()[1].multikeyPaths, {{2U}});
}

TEST(MultikeyPathTracker, SortByTimestampPreservesNullTimestamps) {
    MultikeyPathTracker tracker;
    tracker.startTrackingMultikeyPathInfo();

    const auto uuid = UUID::gen();
    const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");

    tracker.addMultikeyPathInfo({nss, uuid, "idx", {}, MultikeyPaths{{0U}}, Timestamp()});
    tracker.addMultikeyPathInfo({nss, uuid, "idx2", {}, MultikeyPaths{{1U}}, Timestamp()});
    tracker.addMultikeyPathInfo(
        {nss, uuid, "idx", {}, MultikeyPaths{{2U}}, Timestamp(Seconds(2), 0)});

    auto history = tracker.sortByTimestamp();
    ASSERT_EQ(3UL, history.size());
    ASSERT_TRUE(history[0].earliestTimestamp.isNull());
    ASSERT_TRUE(history[1].earliestTimestamp.isNull());
    ASSERT_EQ(Timestamp(Seconds(2), 0), history[2].earliestTimestamp);
}
}  // namespace
}  // namespace mongo
