// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

TEST(InternalSchemaMaxPropertiesMatchExpression, EquivalentFunctionIsAccurate) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties1(1);
    InternalSchemaMaxPropertiesMatchExpression maxProperties2(1);
    InternalSchemaMaxPropertiesMatchExpression maxProperties3(2);

    ASSERT_TRUE(maxProperties1.equivalent(&maxProperties1));
    ASSERT_TRUE(maxProperties1.equivalent(&maxProperties2));
    ASSERT_FALSE(maxProperties1.equivalent(&maxProperties3));
}

TEST(InternalSchemaMaxPropertiesMatchExpression, MinPropertiesNotEquivalentToMaxProperties) {
    InternalSchemaMaxPropertiesMatchExpression maxProperties(5);
    InternalSchemaMinPropertiesMatchExpression minProperties(5);

    ASSERT_FALSE(maxProperties.equivalent(&minProperties));
}

DEATH_TEST_REGEX(InternalSchemaMaxPropertiesMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400216") {
    InternalSchemaMaxPropertiesMatchExpression maxProperties(5);

    ASSERT_EQ(maxProperties.numChildren(), 0);
    ASSERT_THROWS_CODE(maxProperties.getChild(0), AssertionException, 6400216);
}

}  // namespace
}  // namespace mongo
