// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(InternalSchemaFmodMatchExpression, ZeroDivisor) {
    ASSERT_THROWS_CODE(InternalSchemaFmodMatchExpression(""sv, Decimal128(0), Decimal128(1)),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST(InternalSchemaFmodMatchExpression, Equality) {
    InternalSchemaFmodMatchExpression m1("a"sv, Decimal128(1.7), Decimal128(2));
    InternalSchemaFmodMatchExpression m2("a"sv, Decimal128(2), Decimal128(2));
    InternalSchemaFmodMatchExpression m3("a"sv, Decimal128(1.7), Decimal128(1));
    InternalSchemaFmodMatchExpression m4("b"sv, Decimal128(1.7), Decimal128(2));

    ASSERT_TRUE(m1.equivalent(&m1));
    ASSERT_FALSE(m1.equivalent(&m2));
    ASSERT_FALSE(m1.equivalent(&m3));
    ASSERT_FALSE(m1.equivalent(&m4));
}
}  // namespace
}  // namespace mongo
