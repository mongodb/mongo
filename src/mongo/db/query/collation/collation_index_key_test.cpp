// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/collation/collation_index_key.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string_view>


namespace {
using namespace std::literals::string_view_literals;

using namespace mongo;

void assertKeyStringCollatorOutput(const CollatorInterfaceMock& collator,
                                   const BSONObj& dataObj,
                                   const BSONObj& expected) {
    key_string::Builder ks(key_string::Version::kLatestVersion, key_string::ALL_ASCENDING);
    ks.appendBSONElement(dataObj.firstElement(), [&](std::string_view stringData) {
        return collator.getComparisonString(stringData);
    });

    ASSERT_EQ(ks.getValueCopy(),
              key_string::Builder(
                  key_string::Version::kLatestVersion, expected, key_string::ALL_ASCENDING));
}

void assertKeyStringCollatorThrows(const CollatorInterfaceMock& collator, const BSONObj& dataObj) {
    key_string::Builder ks(key_string::Version::kLatestVersion, key_string::ALL_ASCENDING);
    ASSERT_THROWS_CODE(ks.appendBSONElement(dataObj.firstElement(),
                                            [&](std::string_view stringData) {
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
    builder.append("foo", std::string_view());
    BSONObj dataObj = builder.obj();

    BSONObjBuilder expectedBuilder;
    expectedBuilder.append("", std::string_view());
    BSONObj expectedObj = expectedBuilder.obj();

    BSONObjBuilder out;
    CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), &collator, &out);
    ASSERT_BSONOBJ_EQ(out.obj(), expectedObj);
}

TEST(CollationIndexKeyTest, KeyStringAppendCorrectlySerializesEmptyComparisonKey) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObjBuilder builder;
    builder.append("foo", std::string_view());

    BSONObjBuilder expectedBuilder;
    expectedBuilder.append("", std::string_view());
    assertKeyStringCollatorOutput(collator, builder.obj(), expectedBuilder.obj());
}

TEST(CollationIndexKeyTest, CollationAwareAppendCorrectlySerializesWithEmbeddedNullByte) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObjBuilder builder;
    builder.append("foo", "a\0b"sv);
    BSONObj dataObj = builder.obj();

    BSONObjBuilder expectedBuilder;
    expectedBuilder.append("", "b\0a"sv);
    BSONObj expectedObj = expectedBuilder.obj();

    BSONObjBuilder out;
    CollationIndexKey::collationAwareIndexKeyAppend(dataObj.firstElement(), &collator, &out);
    ASSERT_BSONOBJ_EQ(out.obj(), expectedObj);
}

TEST(CollationIndexKeyTest, KeyStringAppendCorrectlySerializesWithEmbeddedNullByte) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObjBuilder builder;
    builder.append("foo", "a\0b"sv);

    BSONObjBuilder expectedBuilder;
    expectedBuilder.append("", "b\0a"sv);
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
