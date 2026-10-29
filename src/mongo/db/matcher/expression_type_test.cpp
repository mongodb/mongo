// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_type.h"

#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <cstring>
#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(ExpressionTypeTest, Equivalent) {
    TypeMatchExpression e1("a"sv, BSONType::string);
    TypeMatchExpression e2("a"sv, BSONType::numberDouble);
    TypeMatchExpression e3("b"sv, BSONType::string);

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
    ASSERT(!e1.equivalent(&e3));
}

TEST(ExpressionTypeTest, RedactsTypesCorrectly) {
    TypeMatchExpression type(""sv, BSONType::string);
    auto opts = query_shape::SerializationOptions{
        query_shape::LiteralSerializationPolicy::kToDebugTypeString};
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$type":[2]})",
        type.getSerializedRightHandSide(opts));
}

TEST(ExpressionBinDataSubTypeTest, Equivalent) {
    InternalSchemaBinDataSubTypeExpression e1("a"sv, BinDataType::newUUID);
    InternalSchemaBinDataSubTypeExpression e2("a"sv, BinDataType::MD5Type);
    InternalSchemaBinDataSubTypeExpression e3("b"sv, BinDataType::newUUID);

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
    ASSERT(!e1.equivalent(&e3));
}

TEST(ExpressionBinDataSubTypeTest, RedactsCorrectly) {
    InternalSchemaBinDataSubTypeExpression e("b"sv, BinDataType::newUUID);
    auto opts = query_shape::SerializationOptions{
        query_shape::LiteralSerializationPolicy::kToDebugTypeString};
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalSchemaBinDataSubType":"?number"})",
        e.getSerializedRightHandSide(opts));
}

}  // namespace
}  // namespace mongo
