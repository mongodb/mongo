// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
TEST(InternalSchemaUniqueItemsMatchExpression, EquivalentFunctionTest) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems1("foo"sv);
    InternalSchemaUniqueItemsMatchExpression uniqueItems2("bar"sv);

    auto uniqueItems3 = uniqueItems1.clone();
    ASSERT_TRUE(uniqueItems1.equivalent(uniqueItems3.get()));
    ASSERT_FALSE(uniqueItems1.equivalent(&uniqueItems2));
}

DEATH_TEST_REGEX(InternalSchemaUniqueItemsMatchExpressionDeathTest,
                 GetChildFailsIndexLargerThanZero,
                 "Tripwire assertion.*6400219") {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo"sv);

    ASSERT_EQ(uniqueItems.numChildren(), 0);
    ASSERT_THROWS_CODE(uniqueItems.getChild(0), AssertionException, 6400219);
}

}  // namespace
}  // namespace mongo
