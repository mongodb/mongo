// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

TEST(InternalSchemaMinLengthMatchExpression, SameMinLengthTreatedEquivalent) {
    InternalSchemaMinLengthMatchExpression minLength1("a"sv, 2);
    InternalSchemaMinLengthMatchExpression minLength2("a"sv, 2);
    InternalSchemaMinLengthMatchExpression minLength3("a"sv, 3);

    ASSERT_TRUE(minLength1.equivalent(&minLength2));
    ASSERT_FALSE(minLength1.equivalent(&minLength3));
}

}  // namespace
}  // namespace mongo
