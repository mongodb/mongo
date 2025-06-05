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

TEST(ExpressionTypeTest, Equivalent) {
    TypeMatchExpression e1("a"_sd, BSONType::string);
    TypeMatchExpression e2("a"_sd, BSONType::numberDouble);
    TypeMatchExpression e3("b"_sd, BSONType::string);

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
    ASSERT(!e1.equivalent(&e3));
}

TEST(ExpressionTypeTest, RedactsTypesCorrectly) {
    TypeMatchExpression type(""_sd, BSONType::string);
    auto opts = SerializationOptions{LiteralSerializationPolicy::kToDebugTypeString};
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$type":[2]})",
        type.getSerializedRightHandSide(opts));
}

TEST(ExpressionBinDataSubTypeTest, Equivalent) {
    InternalSchemaBinDataSubTypeExpression e1("a"_sd, BinDataType::newUUID);
    InternalSchemaBinDataSubTypeExpression e2("a"_sd, BinDataType::MD5Type);
    InternalSchemaBinDataSubTypeExpression e3("b"_sd, BinDataType::newUUID);

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
    ASSERT(!e1.equivalent(&e3));
}

TEST(ExpressionBinDataSubTypeTest, RedactsCorrectly) {
    InternalSchemaBinDataSubTypeExpression e("b"_sd, BinDataType::newUUID);
    auto opts = SerializationOptions{LiteralSerializationPolicy::kToDebugTypeString};
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalSchemaBinDataSubType":"?number"})",
        e.getSerializedRightHandSide(opts));
}

}  // namespace
}  // namespace mongo
