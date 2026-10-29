// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/metadata/index_entry.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>


namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

IndexEntry makeIndexEntry(BSONObj keyPattern, MultikeyPaths multiKeyPaths) {
    bool multiKey = std::any_of(multiKeyPaths.cbegin(),
                                multiKeyPaths.cend(),
                                [](const auto& entry) { return !entry.empty(); });

    return {keyPattern,
            IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
            IndexConfig::kLatestIndexVersion,
            multiKey,
            multiKeyPaths,
            {},
            false,
            false,
            CoreIndexInfo::Identifier("test_foo"),
            {},
            nullptr};
}

TEST(QueryPlannerIXSelectTest, IndexedFieldHasMultikeyComponents) {
    auto indexEntry = makeIndexEntry(BSON("a" << 1 << "b.c" << 1), {{}, {}});
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("a"sv));
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("b.c"sv));

    indexEntry = makeIndexEntry(BSON("a" << 1 << "b" << 1), {{}, {0U}});
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("a"sv));
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("b"sv));

    indexEntry = makeIndexEntry(BSON("a" << 1 << "b" << 1 << "c.d" << 1), {{}, {}, {1U}});
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("a"sv));
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("b"sv));
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("c.d"sv));

    indexEntry = makeIndexEntry(BSON("a.b" << 1 << "a.c" << 1), {{}, {1U}});
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("a.b"sv));
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a.c"sv));

    indexEntry = makeIndexEntry(BSON("a.b" << 1 << "a.c" << 1), {{0U, 1U}, {0U}});
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a.b"sv));
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a.c"sv));

    indexEntry = makeIndexEntry(BSON("a" << 1 << "b" << 1), {{0U}, {}});
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a"sv));
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("b"sv));

    indexEntry = makeIndexEntry(BSON("a.b.c" << 1 << "d" << 1), {{1U, 2U}, {}});
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a.b.c"sv));
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("d"sv));

    indexEntry = makeIndexEntry(BSON("a.b" << 1 << "c" << 1 << "d" << 1), {{1U}, {}, {0U}});
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("a.b"sv));
    ASSERT_FALSE(indexEntry.pathHasMultikeyComponent("c"sv));
    ASSERT_TRUE(indexEntry.pathHasMultikeyComponent("d"sv));
}

DEATH_TEST_REGEX(QueryPlannerIXSelectTestDeathTest,
                 IndexedFieldHasMultikeyComponentsPassingInvalidFieldIsFatal,
                 "Invariant failure.*Hit a MONGO_UNREACHABLE!") {
    auto indexEntry = makeIndexEntry(BSON("a" << 1), {{}});
    indexEntry.pathHasMultikeyComponent("b"sv);
}

}  // namespace
}  // namespace mongo
