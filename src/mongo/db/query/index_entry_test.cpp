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

#include "mongo/platform/basic.h"

#include "mongo/db/query/index_entry.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

IndexEntry makeIndexEntry(BSONObj keyPattern, MultikeyPaths multiKeyPaths) {
    bool multiKey = std::any_of(multiKeyPaths.cbegin(),
                                multiKeyPaths.cend(),
                                [](const auto& entry) { return !entry.empty(); });

    return {keyPattern,
            IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
            multiKey,
            multiKeyPaths,
            {},
            false,
            false,
            CoreIndexInfo::Identifier("test_foo"),
            nullptr,
            {},
            nullptr,
            nullptr};
}

TEST(QueryPlannerIXSelectTest, IndexedFieldHasMultikeyComponents) {
    auto indexEntry = makeIndexEntry(BSON("a" << 1 << "b.c" << 1), {{}, {}});
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("a"_sd));
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("b.c"_sd));

    indexEntry = makeIndexEntry(BSON("a" << 1 << "b" << 1), {{}, {0U}});
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("a"_sd));
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("b"_sd));

    indexEntry = makeIndexEntry(BSON("a" << 1 << "b" << 1 << "c.d" << 1), {{}, {}, {1U}});
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("a"_sd));
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("b"_sd));
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("c.d"_sd));

    indexEntry = makeIndexEntry(BSON("a.b" << 1 << "a.c" << 1), {{}, {1U}});
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("a.b"_sd));
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a.c"_sd));

    indexEntry = makeIndexEntry(BSON("a.b" << 1 << "a.c" << 1), {{0U, 1U}, {0U}});
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a.b"_sd));
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a.c"_sd));

    indexEntry = makeIndexEntry(BSON("a" << 1 << "b" << 1), {{0U}, {}});
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a"_sd));
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("b"_sd));

    indexEntry = makeIndexEntry(BSON("a.b.c" << 1 << "d" << 1), {{1U, 2U}, {}});
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a.b.c"_sd));
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("d"_sd));

    indexEntry = makeIndexEntry(BSON("a.b" << 1 << "c" << 1 << "d" << 1), {{1U}, {}, {0U}});
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a.b"_sd));
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("c"_sd));
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("d"_sd));
}

DEATH_TEST(QueryPlannerIXSelectTest,
           IndexedFieldHasMultikeyComponentsPassingInvalidFieldIsFatal,
           "Invariant failure Hit a MONGO_UNREACHABLE!") {
    auto indexEntry = makeIndexEntry(BSON("a" << 1), {{}});
    indexEntry.pathHasMultikeyComponent("b"_sd);
}

}  // namespace
}  // namespace mongo
