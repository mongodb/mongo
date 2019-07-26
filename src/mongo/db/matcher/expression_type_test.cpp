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

#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ExpressionTypeTest, MatchesElementStringType) {
    BSONObj match = BSON("a"
                         << "abc");
    BSONObj notMatch = BSON("a" << 5);
    TypeMatchExpression type("", String);
    ASSERT(type.matchesSingleElement(match["a"]));
    ASSERT(!type.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionTypeTest, MatchesElementNullType) {
    BSONObj match = BSON("a" << BSONNULL);
    BSONObj notMatch = BSON("a"
                            << "abc");
    TypeMatchExpression type("", jstNULL);
    ASSERT(type.matchesSingleElement(match["a"]));
    ASSERT(!type.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionTypeTest, MatchesElementNumber) {
    BSONObj match1 = BSON("a" << 1);
    BSONObj match2 = BSON("a" << 1LL);
    BSONObj match3 = BSON("a" << 2.5);
    BSONObj notMatch = BSON("a"
                            << "abc");
    ASSERT_EQ(BSONType::NumberInt, match1["a"].type());
    ASSERT_EQ(BSONType::NumberLong, match2["a"].type());
    ASSERT_EQ(BSONType::NumberDouble, match3["a"].type());

    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    TypeMatchExpression typeExpr("a", std::move(typeSet));

    ASSERT_EQ("a", typeExpr.path());
    ASSERT_TRUE(typeExpr.matchesSingleElement(match1["a"]));
    ASSERT_TRUE(typeExpr.matchesSingleElement(match2["a"]));
    ASSERT_TRUE(typeExpr.matchesSingleElement(match3["a"]));
    ASSERT_FALSE(typeExpr.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionTypeTest, MatchesScalar) {
    TypeMatchExpression type("a", Bool);
    ASSERT(type.matchesBSON(BSON("a" << true), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << 1), NULL));
}

TEST(ExpressionTypeTest, MatchesArray) {
    TypeMatchExpression type("a", NumberInt);
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(4)), NULL));
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(4 << "a")), NULL));
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY("a" << 4)), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON_ARRAY("a")), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(4))), NULL));
}

TEST(ExpressionTypeTest, TypeArrayMatchesOuterAndInnerArray) {
    TypeMatchExpression type("a", Array);
    ASSERT(type.matchesBSON(BSON("a" << BSONArray()), nullptr));
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(4 << "a")), nullptr));
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(BSONArray() << 2)), nullptr));
    ASSERT(!type.matchesBSON(BSON("a"
                                  << "bar"),
                             nullptr));
}

TEST(ExpressionTypeTest, MatchesObject) {
    TypeMatchExpression type("a", Object);
    ASSERT(type.matchesBSON(BSON("a" << BSON("b" << 1)), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << 1), NULL));
}

TEST(ExpressionTypeTest, MatchesDotNotationFieldObject) {
    TypeMatchExpression type("a.b", Object);
    ASSERT(type.matchesBSON(BSON("a" << BSON("b" << BSON("c" << 1))), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON("b" << 1)), NULL));
}

TEST(ExpressionTypeTest, MatchesDotNotationArrayElementArray) {
    TypeMatchExpression type("a.0", Array);
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(1))), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON_ARRAY("b")), NULL));
}

TEST(ExpressionTypeTest, MatchesDotNotationArrayElementScalar) {
    TypeMatchExpression type("a.0", String);
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY("b")), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON_ARRAY(1)), NULL));
}

TEST(ExpressionTypeTest, MatchesDotNotationArrayElementObject) {
    TypeMatchExpression type("a.0", Object);
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 1))), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON_ARRAY(1)), NULL));
}

TEST(ExpressionTypeTest, MatchesNull) {
    TypeMatchExpression type("a", jstNULL);
    ASSERT(type.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << 4), NULL));
    ASSERT(!type.matchesBSON(BSONObj(), NULL));
}

TEST(ExpressionTypeTest, ElemMatchKey) {
    TypeMatchExpression type("a.b", String);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!type.matchesBSON(BSON("a" << 1), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(type.matchesBSON(BSON("a" << BSON("b"
                                             << "string")),
                            &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(type.matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY("string"))), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("0", details.elemMatchKey());
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(2 << BSON("b" << BSON_ARRAY("string")))),
                            &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(ExpressionTypeTest, Equivalent) {
    TypeMatchExpression e1("a", BSONType::String);
    TypeMatchExpression e2("a", BSONType::NumberDouble);
    TypeMatchExpression e3("b", BSONType::String);

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
    ASSERT(!e1.equivalent(&e3));
}

TEST(ExpressionTypeTest, InternalSchemaTypeArrayOnlyMatchesArrays) {
    InternalSchemaTypeExpression expr("a", BSONType::Array);
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: []}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: [1]}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: 1}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: {b: []}}")));
}

TEST(ExpressionTypeTest, InternalSchemaTypeNumberDoesNotMatchArrays) {
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    InternalSchemaTypeExpression expr("a", std::move(typeSet));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: []}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: [1]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: ['b', 2, 3]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: {b: []}}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: 1}")));
}

TEST(ExpressionTypeTest, TypeExprWithMultipleTypesMatchesAllSuchTypes) {
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    typeSet.bsonTypes.insert(BSONType::String);
    typeSet.bsonTypes.insert(BSONType::Object);
    TypeMatchExpression expr("a", std::move(typeSet));

    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: []}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: 1}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: [1]}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: null}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: 'str'}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: ['str']}")));
}

TEST(ExpressionTypeTest, InternalSchemaTypeExprWithMultipleTypesMatchesAllSuchTypes) {
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    typeSet.bsonTypes.insert(BSONType::String);
    typeSet.bsonTypes.insert(BSONType::Object);
    InternalSchemaTypeExpression expr("a", std::move(typeSet));

    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: []}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: 1}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: [1]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: null}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: 'str'}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: ['str']}")));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataGeneral) {
    BSONObj match = BSON("a" << BSONBinData(NULL, 0, BinDataType::BinDataGeneral));
    BSONObj notMatch = BSON("a" << BSONBinData(NULL, 0, BinDataType::bdtCustom));
    InternalSchemaBinDataSubTypeExpression type("", BinDataType::BinDataGeneral);
    ASSERT_TRUE(type.matchesSingleElement(match["a"]));
    ASSERT_FALSE(type.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataFunction) {
    BSONObj match = BSON("a" << BSONBinData(NULL, 0, BinDataType::Function));
    BSONObj notMatch = BSON("a" << BSONBinData(NULL, 0, BinDataType::MD5Type));
    InternalSchemaBinDataSubTypeExpression type("", BinDataType::Function);
    ASSERT_TRUE(type.matchesSingleElement(match["a"]));
    ASSERT_FALSE(type.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataNewUUID) {
    BSONObj match = BSON("a" << BSONBinData(NULL, 0, BinDataType::newUUID));
    BSONObj notMatch = BSON("a" << BSONBinData(NULL, 0, BinDataType::BinDataGeneral));
    InternalSchemaBinDataSubTypeExpression type("", BinDataType::newUUID);
    ASSERT_TRUE(type.matchesSingleElement(match["a"]));
    ASSERT_FALSE(type.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataMD5Type) {
    BSONObj match = BSON("a" << BSONBinData(NULL, 0, BinDataType::MD5Type));
    BSONObj notMatch = BSON("a" << BSONBinData(NULL, 0, BinDataType::newUUID));
    InternalSchemaBinDataSubTypeExpression type("", BinDataType::MD5Type);
    ASSERT_TRUE(type.matchesSingleElement(match["a"]));
    ASSERT_FALSE(type.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataEncryptType) {
    BSONObj match = BSON("a" << BSONBinData(NULL, 0, BinDataType::Encrypt));
    BSONObj notMatch = BSON("a" << BSONBinData(NULL, 0, BinDataType::newUUID));
    InternalSchemaBinDataSubTypeExpression type("", BinDataType::Encrypt);
    ASSERT_TRUE(type.matchesSingleElement(match["a"]));
    ASSERT_FALSE(type.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataBdtCustom) {
    BSONObj match = BSON("a" << BSONBinData(NULL, 0, BinDataType::bdtCustom));
    BSONObj notMatch = BSON("a" << BSONBinData(NULL, 0, BinDataType::Function));
    InternalSchemaBinDataSubTypeExpression type("", BinDataType::bdtCustom);
    ASSERT_TRUE(type.matchesSingleElement(match["a"]));
    ASSERT_FALSE(type.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, DoesNotMatchArrays) {
    InternalSchemaBinDataSubTypeExpression type("a", BinDataType::BinDataGeneral);
    ASSERT_FALSE(type.matchesBSON(
        BSON("a" << BSON_ARRAY(BSONBinData(NULL, 0, BinDataType::BinDataGeneral)
                               << BSONBinData(NULL, 0, BinDataType::BinDataGeneral)))));
    ASSERT_FALSE(
        type.matchesBSON(BSON("a" << BSON_ARRAY(BSONBinData(NULL, 0, BinDataType::BinDataGeneral)
                                                << BSONBinData(NULL, 0, BinDataType::Function)))));
}

TEST(ExpressionBinDataSubTypeTest, DoesNotMatchString) {
    BSONObj notMatch = BSON("a"
                            << "str");
    InternalSchemaBinDataSubTypeExpression type("", BinDataType::bdtCustom);
    ASSERT_FALSE(type.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, Equivalent) {
    InternalSchemaBinDataSubTypeExpression e1("a", BinDataType::newUUID);
    InternalSchemaBinDataSubTypeExpression e2("a", BinDataType::MD5Type);
    InternalSchemaBinDataSubTypeExpression e3("b", BinDataType::newUUID);

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
    ASSERT(!e1.equivalent(&e3));
}

TEST(InternalSchemaBinDataEncryptedTypeTest, DoesNotTraverseLeafArrays) {
    MatcherTypeSet typeSet;
    typeSet.bsonTypes.insert(BSONType::String);
    typeSet.bsonTypes.insert(BSONType::Date);
    InternalSchemaBinDataEncryptedTypeExpression expr("a", std::move(typeSet));

    FleBlobHeader blob;
    blob.fleBlobSubtype = FleBlobSubtype::Deterministic;
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = BSONType::String;
    auto binData = BSONBinData(
        reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);

    BSONObj matchingDoc = BSON("a" << BSONBinData(reinterpret_cast<const void*>(&blob),
                                                  sizeof(FleBlobHeader),
                                                  BinDataType::Encrypt));
    ASSERT_TRUE(expr.matchesBSON(BSON("a" << binData)));
    ASSERT_FALSE(expr.matchesBSON(BSON("a" << BSON_ARRAY(binData))));
    ASSERT_FALSE(expr.matchesBSON(BSON("a" << BSONArray())));
}

}  // namespace
}  // namespace mongo
