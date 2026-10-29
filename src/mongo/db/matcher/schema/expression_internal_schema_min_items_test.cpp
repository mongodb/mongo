// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

DEATH_TEST_REGEX(InternalSchemaMinItemsMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400215") {
    InternalSchemaMinItemsMatchExpression minItems("a"sv, 2);

    ASSERT_EQ(minItems.numChildren(), 0);
    ASSERT_THROWS_CODE(minItems.getChild(0), AssertionException, 6400215);
}

TEST(InternalSchemaMinItemsMatchExpression, EquivalentTest) {
    InternalSchemaMinItemsMatchExpression minItems1("a"sv, 2);
    InternalSchemaMinItemsMatchExpression minItems2("a"sv, 5);

    auto clone = minItems1.clone();
    ASSERT_TRUE(minItems1.equivalent(clone.get()));
    ASSERT_FALSE(minItems1.equivalent(&minItems2));
}
}  // namespace
}  // namespace mongo
