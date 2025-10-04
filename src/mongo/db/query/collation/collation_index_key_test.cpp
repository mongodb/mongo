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

#include "mongo/db/query/collation/collation_index_key.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>

namespace {

using namespace mongo;

void assertKeyStringCollatorOutput(const CollatorInterfaceMock& collator,
                                   const BSONObj& dataObj,
                                   const BSONObj& expected) {
    key_string::Builder ks(key_string::Version::kLatestVersion, key_string::ALL_ASCENDING);
    ks.appendBSONElement(dataObj.firstElement(), [&](StringData stringData) {
        return collator.getComparisonString(stringData);
    });

    ASSERT_EQ(ks.getValueCopy(),
              key_string::Builder(
                  key_string::Version::kLatestVersion, expected, key_string::ALL_ASCENDING));
}

void assertKeyStringCollatorThrows(const CollatorInterfaceMock& collator, const BSONObj& dataObj) {
    key_string::Builder ks(key_string::Version::kLatestVersion, key_string::ALL_ASCENDING);
    ASSERT_THROWS_CODE(ks.appendBSONElement(dataObj.firstElement(),
                                            [&](StringData stringData) {
                                                return collator.getComparisonString(stringData);
                                            }),
                       AssertionException,
                       ErrorCodes::CannotBuildIndexKeys);
}

TEST(CollationIndexKeyTest, IsCollatableTypeShouldBeTrueForString) {
    BSONObj obj = BSON("foo" << "string");
    ASSERT_TRUE(CollationIndexKey::isCollatableType(obj.firstElement().type()));
}

TEST(CollationIndexKeyTest, IsCollatableTypeShouldBeTrueForObject) {
    BSONObj obj = BSON("foo" << BSON("bar" << 99));
    ASSERT_TRUE(CollationIndexKey::isCollatableType(obj.firstElement().type()));
}

TEST(CollationIndexKeyTest, IsCollatableTypeShouldBeTrueForArray) {
    BSONObj obj = BSON("foo" << BSON_ARRAY(98 << 99));
    ASSERT_TRUE(CollationIndexKey::isCollatableType(obj.firstElement().type()));
}

TEST(CollationIndexKeyTest, IsCollatableTypeShouldBeFalseForNumber) {
    BSONObj obj = BSON("foo" << 99);
    ASSERT_FALSE(CollationIndexKey::isCollatableType(obj.firstElement().type()));
}

TEST(CollationIndexKeyTest, CollationAwareAppendCorrectlyAppendsElementWithNullCollator) {
    BSONObj dataObj = BSON("test" << 1);
    BSONObjBuilder out;
    CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), nullptr, &out);
    ASSERT_BSONOBJ_EQ(out.obj(), BSON("" << 1));
}

TEST(CollationIndexKeyTest, CollationAwareAppendReversesStringWithReverseMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("foo" << "string");
    BSONObjBuilder out;
    CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), &collator, &out);
    ASSERT_BSONOBJ_EQ(out.obj(), BSON("" << "gnirts"));
}

TEST(CollationIndexKeyTest, KeyStringAppendReversesStringWithReverseMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("foo" << "string");
    assertKeyStringCollatorOutput(collator, dataObj, BSON("" << "gnirts"));
}

TEST(CollationIndexKeyTest, CollationAwareAppendCorrectlySerializesEmptyComparisonKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObjBuilder builder;
    builder.append("foo", StringData());
    BSONObj dataObj = builder.obj();

    BSONObjBuilder expectedBuilder;
    expectedBuilder.append("", StringData());
    BSONObj expectedObj = expectedBuilder.obj();

    BSONObjBuilder out;
    CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), &collator, &out);
    ASSERT_BSONOBJ_EQ(out.obj(), expectedObj);
}

TEST(CollationIndexKeyTest, KeyStringAppendCorrectlySerializesEmptyComparisonKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObjBuilder builder;
    builder.append("foo", StringData());

    BSONObjBuilder expectedBuilder;
    expectedBuilder.append("", StringData());
    assertKeyStringCollatorOutput(collator, builder.obj(), expectedBuilder.obj());
}

TEST(CollationIndexKeyTest, CollationAwareAppendCorrectlySerializesWithEmbeddedNullByte) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObjBuilder builder;
    builder.append("foo", "a\0b"_sd);
    BSONObj dataObj = builder.obj();

    BSONObjBuilder expectedBuilder;
    expectedBuilder.append("", "b\0a"_sd);
    BSONObj expectedObj = expectedBuilder.obj();

    BSONObjBuilder out;
    CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), &collator, &out);
    ASSERT_BSONOBJ_EQ(out.obj(), expectedObj);
}

TEST(CollationIndexKeyTest, KeyStringAppendCorrectlySerializesWithEmbeddedNullByte) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObjBuilder builder;
    builder.append("foo", "a\0b"_sd);

    BSONObjBuilder expectedBuilder;
    expectedBuilder.append("", "b\0a"_sd);
    assertKeyStringCollatorOutput(collator, builder.obj(), expectedBuilder.obj());
}

TEST(CollationIndexKeyTest, CollationAwareAppendCorrectlyReversesSimpleEmbeddedObject) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("" << BSON("a" << "!foo"));
    BSONObj expected = BSON("" << BSON("a" << "oof!"));

    BSONObjBuilder out;
    CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), &collator, &out);
    ASSERT_BSONOBJ_EQ(out.obj(), expected);
}

TEST(CollationIndexKeyTest, KeyStringAppendCorrectlyReversesSimpleEmbeddedObject) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("" << BSON("a" << "!foo"));
    BSONObj expected = BSON("" << BSON("a" << "oof!"));
    assertKeyStringCollatorOutput(collator, dataObj, expected);
}

TEST(CollationIndexKeyTest, CollationAwareAppendCorrectlyReversesSimpleEmbeddedArray) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("" << BSON_ARRAY("foo" << "bar"));
    BSONObj expected = BSON("" << BSON_ARRAY("oof" << "rab"));

    BSONObjBuilder out;
    CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), &collator, &out);
    ASSERT_BSONOBJ_EQ(out.obj(), expected);
}

TEST(CollationIndexKeyTest, KeyStringAppendCorrectlyReversesSimpleEmbeddedArray) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("" << BSON_ARRAY("foo" << "bar"));
    BSONObj expected = BSON("" << BSON_ARRAY("oof" << "rab"));
    assertKeyStringCollatorOutput(collator, dataObj, expected);
}

TEST(CollationIndexKeyTest, CollationAwareAppendCorrectlyReversesComplexNesting) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = fromjson(
        "{ '' : [{'a': 'ha', 'b': 2},"
        "'bar',"
        "{'c': 2, 'd': 'ah', 'e': 'abc', 'f': ['cba', 'xyz']}]}");
    BSONObj expected = fromjson(
        "{ '' : [{'a': 'ah', 'b': 2},"
        "'rab',"
        "{'c': 2, 'd': 'ha', 'e': 'cba', 'f': ['abc', 'zyx']}]}");

    BSONObjBuilder out;
    CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), &collator, &out);
    ASSERT_BSONOBJ_EQ(out.obj(), expected);
}

TEST(CollationIndexKeyTest, KeyStringAppendCorrectlyReversesComplexNesting) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = fromjson(
        "{ '' : [{'a': 'ha', 'b': 2},"
        "'bar',"
        "{'c': 2, 'd': 'ah', 'e': 'abc', 'f': ['cba', 'xyz']}]}");
    BSONObj expected = fromjson(
        "{ '' : [{'a': 'ah', 'b': 2},"
        "'rab',"
        "{'c': 2, 'd': 'ha', 'e': 'cba', 'f': ['abc', 'zyx']}]}");
    assertKeyStringCollatorOutput(collator, dataObj, expected);
}

TEST(CollationIndexKeyTest, CollationAwareAppendThrowsIfSymbol) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("" << BSONSymbol("mySymbol"));
    BSONObjBuilder out;
    ASSERT_THROWS_CODE(
        CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), &collator, &out),
        AssertionException,
        ErrorCodes::CannotBuildIndexKeys);
}

TEST(CollationIndexKeyTest, KeyStringAppendThrowsIfSymbol) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("" << BSONSymbol("mySymbol"));
    assertKeyStringCollatorThrows(collator, dataObj);
}

TEST(CollationIndexKeyTest, CollationAwareAppendDoesNotThrowOnSymbolIfNoCollation) {
    BSONObj dataObj = BSON("" << BSONSymbol("mySymbol"));
    BSONObj expected = BSON("" << BSONSymbol("mySymbol"));
    BSONObjBuilder out;
    CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), nullptr, &out);
    ASSERT_BSONOBJ_EQ(out.obj(), expected);
}

TEST(CollationIndexKeyTest, CollationAwareAppendThrowsIfSymbolInsideObject) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("" << BSON("a" << "foo"
                                          << "b" << BSONSymbol("mySymbol")));
    BSONObjBuilder out;
    ASSERT_THROWS_CODE(
        CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), &collator, &out),
        AssertionException,
        ErrorCodes::CannotBuildIndexKeys);
}

TEST(CollationIndexKeyTest, KeyStringAppendThrowsIfSymbolInsideObject) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("" << BSON("a" << "foo"
                                          << "b" << BSONSymbol("mySymbol")));
    assertKeyStringCollatorThrows(collator, dataObj);
}

TEST(CollationIndexKeyTest, CollationAwareAppendThrowsIfSymbolInsideArray) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("" << BSON_ARRAY("foo" << BSONSymbol("mySymbol")));
    BSONObjBuilder out;
    ASSERT_THROWS_CODE(
        CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), &collator, &out),
        AssertionException,
        ErrorCodes::CannotBuildIndexKeys);
}

TEST(CollationIndexKeyTest, KeyStringAppendThrowsIfSymbolInsideArray) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj dataObj = BSON("" << BSON_ARRAY("foo" << BSONSymbol("mySymbol")));
    assertKeyStringCollatorThrows(collator, dataObj);
}

}  // namespace
