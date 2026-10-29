// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"

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

DEATH_TEST_REGEX(InternalSchemaMaxItemsMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400215") {
    InternalSchemaMaxItemsMatchExpression maxItems("a"sv, 2);

    ASSERT_EQ(maxItems.numChildren(), 0);
    ASSERT_THROWS_CODE(maxItems.getChild(0), AssertionException, 6400215);
}

}  // namespace
}  // namespace mongo
