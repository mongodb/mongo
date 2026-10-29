// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

TEST(InternalSchemaMaxLengthMatchExpression, SameMaxLengthTreatedEquivalent) {
    InternalSchemaMaxLengthMatchExpression maxLength1("a"sv, 2);
    InternalSchemaMaxLengthMatchExpression maxLength2("a"sv, 2);
    InternalSchemaMaxLengthMatchExpression maxLength3("a"sv, 3);

    ASSERT_TRUE(maxLength1.equivalent(&maxLength2));
    ASSERT_FALSE(maxLength1.equivalent(&maxLength3));
}

TEST(InternalSchemaMaxLengthMatchExpression, MinLengthAndMaxLengthAreNotEquivalent) {
    InternalSchemaMinLengthMatchExpression minLength("a"sv, 2);
    InternalSchemaMaxLengthMatchExpression maxLength("a"sv, 2);

    ASSERT_FALSE(maxLength.equivalent(&minLength));
}

}  // namespace
}  // namespace mongo
