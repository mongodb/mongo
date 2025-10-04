/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
TEST(InternalSchemaUniqueItemsMatchExpression, EquivalentFunctionTest) {
    InternalSchemaUniqueItemsMatchExpression uniqueItems1("foo"_sd);
    InternalSchemaUniqueItemsMatchExpression uniqueItems2("bar"_sd);

    auto uniqueItems3 = uniqueItems1.clone();
    ASSERT_TRUE(uniqueItems1.equivalent(uniqueItems3.get()));
    ASSERT_FALSE(uniqueItems1.equivalent(&uniqueItems2));
}

DEATH_TEST_REGEX(InternalSchemaUniqueItemsMatchExpression,
                 GetChildFailsIndexLargerThanZero,
                 "Tripwire assertion.*6400219") {
    InternalSchemaUniqueItemsMatchExpression uniqueItems("foo"_sd);

    ASSERT_EQ(uniqueItems.numChildren(), 0);
    ASSERT_THROWS_CODE(uniqueItems.getChild(0), AssertionException, 6400219);
}

}  // namespace
}  // namespace mongo
