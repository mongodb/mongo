// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

TEST(InternalSchemaMinPropertiesMatchExpression, EquivalentFunctionIsAccurate) {
    InternalSchemaMinPropertiesMatchExpression minProperties1(1);
    InternalSchemaMinPropertiesMatchExpression minProperties2(1);
    InternalSchemaMinPropertiesMatchExpression minProperties3(2);

    ASSERT_TRUE(minProperties1.equivalent(&minProperties1));
    ASSERT_TRUE(minProperties1.equivalent(&minProperties2));
    ASSERT_FALSE(minProperties1.equivalent(&minProperties3));
}

DEATH_TEST_REGEX(InternalSchemaMinPropertiesMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400216") {
    InternalSchemaMaxPropertiesMatchExpression minProperties(1);

    ASSERT_EQ(minProperties.numChildren(), 0);
    ASSERT_THROWS_CODE(minProperties.getChild(0), AssertionException, 6400216);
}

}  // namespace
}  // namespace mongo
