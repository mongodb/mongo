// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/matcher_type_set.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <cmath>
#include <limits>
#include <string>

namespace mongo {
namespace {

TEST(MatcherTypeSetTest, ParseFromStringAliasesCanParseNumberAlias) {
    auto result = MatcherTypeSet::fromStringAliases({"number"}, findBSONTypeAlias);
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonTypes.size(), 0u);
}

TEST(MatcherTypeSetTest, ParseFromStringAliasesCanParseLongAlias) {
    auto result = MatcherTypeSet::fromStringAliases({"long"}, findBSONTypeAlias);
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonTypes.size(), 1u);
    ASSERT_TRUE(result.getValue().hasType(BSONType::numberLong));
}

TEST(MatcherTypeSetTest, ParseFromStringAliasesCanParseMultipleTypes) {
    auto result =
        MatcherTypeSet::fromStringAliases({"number", "object", "string"}, findBSONTypeAlias);
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonTypes.size(), 2u);
    ASSERT_TRUE(result.getValue().hasType(BSONType::object));
    ASSERT_TRUE(result.getValue().hasType(BSONType::string));
}

TEST(MatcherTypeSetTest, ParseFromStringAliasesCanParseEmptySet) {
    auto result = MatcherTypeSet::fromStringAliases({}, findBSONTypeAlias);
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().isEmpty());
}

TEST(MatcherTypeSetTest, ParseFromStringFailsToParseUnknownAlias) {
    auto result = MatcherTypeSet::fromStringAliases({"long", "unknown"}, findBSONTypeAlias);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetTest, IsSingleTypeTrueForTypeNumber) {
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    ASSERT_TRUE(typeSet.isSingleType());
}

TEST(MatcherTypeSetTest, IsSingleTypeTrueForSingleBSONType) {
    MatcherTypeSet typeSet{BSONType::numberLong};
    ASSERT_TRUE(typeSet.isSingleType());
}

TEST(MatcherTypeSetTest, IsSingleTypeFalseForEmpty) {
    MatcherTypeSet typeSet;
    ASSERT_FALSE(typeSet.isSingleType());
}

TEST(MatcherTypeSetTest, IsSingleTypeFalseForMultipleTypes) {
    MatcherTypeSet typeSet{BSONType::numberLong};
    typeSet.allNumbers = true;
    ASSERT_FALSE(typeSet.isSingleType());
}

TEST(MatcherTypeSetTest, IsEmptyTrueForEmpty) {
    MatcherTypeSet typeSet;
    ASSERT_TRUE(typeSet.isEmpty());
}

TEST(MatcherTypeSetTest, IsEmptyFalseForAllNumbers) {
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    ASSERT_FALSE(typeSet.isEmpty());
}

TEST(MatcherTypeSetTest, IsEmptyFalseForBSONTypeNumberLong) {
    MatcherTypeSet typeSet{BSONType::numberLong};
    ASSERT_FALSE(typeSet.isEmpty());
}

TEST(MatcherTypeSetTest, HasTypeTrueForNumericalTypesWhenSetHasAllNumbersType) {
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    ASSERT_TRUE(typeSet.hasType(BSONType::numberInt));
    ASSERT_TRUE(typeSet.hasType(BSONType::numberLong));
    ASSERT_TRUE(typeSet.hasType(BSONType::numberDouble));
    ASSERT_TRUE(typeSet.hasType(BSONType::numberDecimal));
}

TEST(MatcherTypeSetTest, HasTypeTrueForSetOfTypes) {
    MatcherTypeSet typeSet;
    typeSet.bsonTypes.insert(BSONType::numberLong);
    typeSet.bsonTypes.insert(BSONType::string);
    ASSERT_FALSE(typeSet.hasType(BSONType::numberInt));
    ASSERT_TRUE(typeSet.hasType(BSONType::numberLong));
    ASSERT_FALSE(typeSet.hasType(BSONType::numberDouble));
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
}

}  // namespace
}  // namespace mongo
