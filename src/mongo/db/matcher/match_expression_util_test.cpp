// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/match_expression_util.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"


namespace mongo::match_expression_util {
namespace {
TEST(BSONObjIteratorUtils, AdvanceBy) {
    auto obj = BSON("a" << 1 << "b" << 2 << "c" << 3);
    BSONObjIterator iter = BSONObjIterator(obj);
    advanceBy(0, iter);
    ASSERT_TRUE(iter.more());
    ASSERT_EQUALS(iter.next().fieldNameStringData(), "a");
    advanceBy(1, iter);
    ASSERT_TRUE(iter.more());
    ASSERT_EQUALS(iter.next().fieldNameStringData(), "c");
    ASSERT_FALSE(iter.more());
    advanceBy(0, iter);
    ASSERT_FALSE(iter.more());
    advanceBy(1, iter);
    ASSERT_FALSE(iter.more());
}
}  // namespace
}  // namespace mongo::match_expression_util
