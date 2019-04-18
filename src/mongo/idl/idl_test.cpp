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

#include "mongo/platform/basic.h"

#include <limits>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/unittest_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"

using namespace mongo::idl::test;
using namespace mongo::idl::import;

namespace mongo {

namespace {

bool isEquals(ConstDataRange left, const std::vector<uint8_t>& right) {
    ConstDataRange rightCDR(right);
    return std::equal(left.data(),
                      left.data() + left.length(),
                      rightCDR.data(),
                      rightCDR.data() + rightCDR.length());
}

bool isEquals(const std::array<uint8_t, 16>& left, const std::array<uint8_t, 16>& right) {
    return std::equal(
        left.data(), left.data() + left.size(), right.data(), right.data() + right.size());
}

bool isEqual(const ConstDataRange& left, const ConstDataRange& right) {
    return std::equal(
        left.data(), left.data() + left.length(), right.data(), right.data() + right.length());
}

bool isEquals(const std::vector<ConstDataRange>& left,
              const std::vector<std::vector<std::uint8_t>>& rightVector) {
    auto right = transformVector(rightVector);
    return std::equal(
        left.data(), left.data() + left.size(), right.data(), right.data() + right.size(), isEqual);
}

bool isEquals(const std::vector<std::array<std::uint8_t, 16>>& left,
              const std::vector<std::array<std::uint8_t, 16>>& right) {
    return std::equal(
        left.data(), left.data() + left.size(), right.data(), right.data() + right.size());
}

/**
 * Flatten an OpMsgRequest into a BSONObj.
 */
BSONObj flatten(const OpMsgRequest& msg) {
    BSONObjBuilder builder;
    builder.appendElements(msg.body);

    for (auto&& docSeq : msg.sequences) {
        builder.append(docSeq.name, docSeq.objs);
    }

    return builder.obj();
}

/**
 * Validate two OpMsgRequests are the same regardless of whether they both use DocumentSequences.
 */
void assertOpMsgEquals(const OpMsgRequest& left, const OpMsgRequest& right) {
    auto flatLeft = flatten(left);
    auto flatRight = flatten(right);

    ASSERT_BSONOBJ_EQ(flatLeft, flatRight);
}

/**
* Validate two OpMsgRequests are the same including their DocumentSequences.
*/
void assertOpMsgEqualsExact(const OpMsgRequest& left, const OpMsgRequest& right) {

    ASSERT_BSONOBJ_EQ(left.body, right.body);

    ASSERT_EQUALS(left.sequences.size(), right.sequences.size());

    for (size_t i = 0; i < left.sequences.size(); ++i) {
        auto leftItem = left.sequences[i];
        auto rightItem = right.sequences[i];

        ASSERT_TRUE(std::equal(leftItem.objs.begin(),
                               leftItem.objs.end(),
                               rightItem.objs.begin(),
                               rightItem.objs.end(),
                               [](const BSONObj& leftBson, const BSONObj& rightBson) {
                                   return SimpleBSONObjComparator::kInstance.compare(
                                              leftBson, rightBson) == 0;
                               }));
        ASSERT_EQUALS(leftItem.name, rightItem.name);
    }
}


BSONObj appendDB(const BSONObj& obj, StringData dbName) {
    BSONObjBuilder builder;
    builder.appendElements(obj);
    builder.append("$db", dbName);
    return builder.obj();
}

// Use a separate function to get better error messages when types do not match.
template <typename T1, typename T2>
void assert_same_types() {
    MONGO_STATIC_ASSERT(std::is_same<T1, T2>::value);
}

template <typename ParserT, typename TestT, BSONType Test_bson_type>
void TestLoopback(TestT test_value) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON("value" << test_value);

    auto element = testDoc.firstElement();
    ASSERT_EQUALS(element.type(), Test_bson_type);

    auto testStruct = ParserT::parse(ctxt, testDoc);
    assert_same_types<decltype(testStruct.getValue()), TestT>();

    ASSERT_EQUALS(testStruct.getValue(), test_value);

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can roundtrip from the just parsed document
    {
        auto loopbackDoc = testStruct.toBSON();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        ParserT one_new;
        one_new.setValue(test_value);
        one_new.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);

        // Validate the operator == works
        // Use ASSERT instead of ASSERT_EQ to avoid operator<<
        ASSERT(one_new == testStruct);
    }
}

/// Type tests:
// Positive: Test we can serialize the type out and back again
TEST(IDLOneTypeTests, TestLoopbackTest) {
    TestLoopback<One_string, const StringData, String>("test_value");
    TestLoopback<One_int, std::int32_t, NumberInt>(123);
    TestLoopback<One_long, std::int64_t, NumberLong>(456);
    TestLoopback<One_double, double, NumberDouble>(3.14159);
    TestLoopback<One_bool, bool, Bool>(true);
    TestLoopback<One_objectid, const OID&, jstOID>(OID::max());
    TestLoopback<One_date, const Date_t&, Date>(Date_t::now());
    TestLoopback<One_timestamp, const Timestamp&, bsonTimestamp>(Timestamp::max());
}

// Test a BSONObj can be passed through an IDL type
TEST(IDLOneTypeTests, TestObjectLoopbackTest) {
    IDLParserErrorContext ctxt("root");

    auto testValue = BSON("Hello"
                          << "World");
    auto testDoc = BSON("value" << testValue);

    auto element = testDoc.firstElement();
    ASSERT_EQUALS(element.type(), Object);

    auto testStruct = One_plain_object::parse(ctxt, testDoc);
    assert_same_types<decltype(testStruct.getValue()), const BSONObj&>();

    ASSERT_BSONOBJ_EQ(testStruct.getValue(), testValue);

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        One_plain_object one_new;
        one_new.setValue(testValue);
        one_new.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);

        ASSERT_TRUE(one_new == testStruct);
        ASSERT_FALSE(one_new < testStruct);
    }
}

// Test we compare an object with optional BSONObjs correctly
TEST(IDLOneTypeTests, TestOptionalObjectTest) {
    IDLParserErrorContext ctxt("root");

    auto testValue = BSON("Hello"
                          << "World");
    auto testDoc = BSON("value" << testValue << "value2" << testValue << "opt_value" << testValue);

    auto element = testDoc.firstElement();
    ASSERT_EQUALS(element.type(), Object);

    auto testStruct = One_plain_optional_object::parse(ctxt, testDoc);
    assert_same_types<decltype(testStruct.getValue()), const BSONObj&>();

    ASSERT_BSONOBJ_EQ(testStruct.getValue(), testValue);

    One_plain_optional_object testEmptyStruct;
    One_plain_optional_object testEmptyStruct2;

    // Make sure we match the operator semantics for std::optional
    ASSERT_TRUE(testEmptyStruct == testEmptyStruct2);
    ASSERT_FALSE(testEmptyStruct != testEmptyStruct2);
    ASSERT_FALSE(testEmptyStruct < testEmptyStruct2);

    ASSERT_FALSE(testEmptyStruct == testStruct);
    ASSERT_TRUE(testEmptyStruct != testStruct);
    ASSERT_TRUE(testEmptyStruct < testStruct);
    ASSERT_FALSE(testStruct < testEmptyStruct);

    ASSERT_TRUE(testStruct == testStruct);
    ASSERT_FALSE(testStruct != testStruct);
    ASSERT_FALSE(testStruct < testStruct);
}

// Test if a given value for a given bson document parses successfully or fails if the bson types
// mismatch.
template <typename ParserT, BSONType Parser_bson_type, typename TestT, BSONType Test_bson_type>
void TestParse(TestT test_value) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON("value" << test_value);

    auto element = testDoc.firstElement();
    ASSERT_EQUALS(element.type(), Test_bson_type);

    if (Parser_bson_type != Test_bson_type) {
        ASSERT_THROWS(ParserT::parse(ctxt, testDoc), AssertionException);
    } else {
        (void)ParserT::parse(ctxt, testDoc);
    }
}

// Test each of types either fail or succeeded based on the parser's bson type
template <typename ParserT, BSONType Parser_bson_type>
void TestParsers() {
    TestParse<ParserT, Parser_bson_type, StringData, String>("test_value");
    TestParse<ParserT, Parser_bson_type, std::int32_t, NumberInt>(123);
    TestParse<ParserT, Parser_bson_type, std::int64_t, NumberLong>(456);
    TestParse<ParserT, Parser_bson_type, double, NumberDouble>(3.14159);
    TestParse<ParserT, Parser_bson_type, bool, Bool>(true);
    TestParse<ParserT, Parser_bson_type, OID, jstOID>(OID::max());
    TestParse<ParserT, Parser_bson_type, Date_t, Date>(Date_t::now());
    TestParse<ParserT, Parser_bson_type, Timestamp, bsonTimestamp>(Timestamp::max());
}

// Negative: document with wrong types for required field
TEST(IDLOneTypeTests, TestNegativeWrongTypes) {
    TestParsers<One_string, String>();
    TestParsers<One_int, NumberInt>();
    TestParsers<One_long, NumberLong>();
    TestParsers<One_double, NumberDouble>();
    TestParsers<One_bool, Bool>();
    TestParsers<One_objectid, jstOID>();
    TestParsers<One_date, Date>();
    TestParsers<One_timestamp, bsonTimestamp>();
}

// Negative: document with wrong types for required field
TEST(IDLOneTypeTests, TestNegativeRequiredNullTypes) {
    TestParse<One_string, String, NullLabeler, jstNULL>(BSONNULL);
    TestParse<One_int, NumberInt, NullLabeler, jstNULL>(BSONNULL);
    TestParse<One_long, NumberLong, NullLabeler, jstNULL>(BSONNULL);
    TestParse<One_double, NumberDouble, NullLabeler, jstNULL>(BSONNULL);
    TestParse<One_bool, Bool, NullLabeler, jstNULL>(BSONNULL);
    TestParse<One_objectid, jstOID, NullLabeler, jstNULL>(BSONNULL);
    TestParse<One_date, Date, NullLabeler, jstNULL>(BSONNULL);
    TestParse<One_timestamp, bsonTimestamp, NullLabeler, jstNULL>(BSONNULL);
}

// Negative: document with wrong types for required field
TEST(IDLOneTypeTests, TestNegativeRequiredUndefinedTypes) {
    TestParse<One_string, String, UndefinedLabeler, Undefined>(BSONUndefined);
    TestParse<One_int, NumberInt, UndefinedLabeler, Undefined>(BSONUndefined);
    TestParse<One_long, NumberLong, UndefinedLabeler, Undefined>(BSONUndefined);
    TestParse<One_double, NumberDouble, UndefinedLabeler, Undefined>(BSONUndefined);
    TestParse<One_bool, Bool, UndefinedLabeler, Undefined>(BSONUndefined);
    TestParse<One_objectid, jstOID, UndefinedLabeler, Undefined>(BSONUndefined);
    TestParse<One_date, Date, UndefinedLabeler, Undefined>(BSONUndefined);
    TestParse<One_timestamp, bsonTimestamp, UndefinedLabeler, Undefined>(BSONUndefined);
}


// Mixed: test a type that accepts multiple bson types
TEST(IDLOneTypeTests, TestSafeInt32) {
    TestParse<One_safeint32, NumberInt, StringData, String>("test_value");
    TestParse<One_safeint32, NumberInt, std::int32_t, NumberInt>(123);
    TestParse<One_safeint32, NumberLong, std::int64_t, NumberLong>(456);
    TestParse<One_safeint32, NumberDouble, double, NumberDouble>(3.14159);
    TestParse<One_safeint32, NumberInt, bool, Bool>(true);
    TestParse<One_safeint32, NumberInt, OID, jstOID>(OID::max());
    TestParse<One_safeint32, NumberInt, Date_t, Date>(Date_t::now());
    TestParse<One_safeint32, NumberInt, Timestamp, bsonTimestamp>(Timestamp::max());
}

// Mixed: test a type that accepts NamespaceString
TEST(IDLOneTypeTests, TestNamespaceString) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON(One_namespacestring::kValueFieldName << "foo.bar");

    auto element = testDoc.firstElement();
    ASSERT_EQUALS(element.type(), String);

    auto testStruct = One_namespacestring::parse(ctxt, testDoc);
    assert_same_types<decltype(testStruct.getValue()), const NamespaceString&>();

    ASSERT_EQUALS(testStruct.getValue(), NamespaceString("foo.bar"));

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        One_namespacestring one_new;
        one_new.setValue(NamespaceString("foo.bar"));
        one_new.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }

    // Negative: invalid namespace
    {
        auto testBadDoc = BSON("value" << StringData("foo\0bar", 7));

        ASSERT_THROWS(One_namespacestring::parse(ctxt, testBadDoc), AssertionException);
    }
}

// Postive: Test any type
TEST(IDLOneTypeTests, TestAnyType) {
    IDLParserErrorContext ctxt("root");

    // Positive: string field
    {
        auto testDoc = BSON("value"
                            << "Foo");
        auto testStruct = One_any_basic_type::parse(ctxt, testDoc);

        BSONObjBuilder builder;
        testStruct.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }

    // Positive: int field
    {
        auto testDoc = BSON("value" << 12);
        auto testStruct = One_any_basic_type::parse(ctxt, testDoc);

        BSONObjBuilder builder;
        testStruct.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }
}

// Postive: Test object type
TEST(IDLOneTypeTests, TestObjectType) {
    IDLParserErrorContext ctxt("root");

    // Positive: object
    {
        auto testDoc = BSON("value" << BSON("value"
                                            << "foo"));
        auto testStruct = One_any_basic_type::parse(ctxt, testDoc);

        BSONObjBuilder builder;
        testStruct.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }
}


// Negative: Test object type
TEST(IDLOneTypeTests, TestObjectTypeNegative) {
    IDLParserErrorContext ctxt("root");

    // Negative: string field
    {
        auto testDoc = BSON("value"
                            << "Foo");
        One_any_basic_type::parse(ctxt, testDoc);
    }

    // Negative: int field
    {
        auto testDoc = BSON("value" << 12);
        One_any_basic_type::parse(ctxt, testDoc);
    }
}

/// Struct tests:
// Positive: strict, 3 required fields
// Negative: strict, ensure extra fields fail
// Negative: strict, duplicate fields
TEST(IDLStructTests, TestStrictStruct) {
    IDLParserErrorContext ctxt("root");

    // Positive: Just 3 required fields
    {
        auto testDoc = BSON("field1" << 12 << "field2" << 123 << "field3" << 1234);
        RequiredStrictField3::parse(ctxt, testDoc);
    }

    // Negative: Missing 1 required field
    {
        auto testDoc = BSON("field2" << 123 << "field3" << 1234);
        ASSERT_THROWS(RequiredStrictField3::parse(ctxt, testDoc), AssertionException);
    }
    {
        auto testDoc = BSON("field1" << 12 << "field3" << 1234);
        ASSERT_THROWS(RequiredStrictField3::parse(ctxt, testDoc), AssertionException);
    }
    {
        auto testDoc = BSON("field1" << 12 << "field2" << 123);
        ASSERT_THROWS(RequiredStrictField3::parse(ctxt, testDoc), AssertionException);
    }

    // Negative: Extra field
    {
        auto testDoc =
            BSON("field1" << 12 << "field2" << 123 << "field3" << 1234 << "field4" << 1234);
        ASSERT_THROWS(RequiredStrictField3::parse(ctxt, testDoc), AssertionException);
    }

    // Negative: Duplicate field
    {
        auto testDoc =
            BSON("field1" << 12 << "field2" << 123 << "field3" << 1234 << "field2" << 12345);
        ASSERT_THROWS(RequiredStrictField3::parse(ctxt, testDoc), AssertionException);
    }
}
// Positive: non-strict, ensure extra fields work
// Negative: non-strict, duplicate fields
TEST(IDLStructTests, TestNonStrictStruct) {
    IDLParserErrorContext ctxt("root");

    // Positive: Just 3 required fields
    {
        auto testDoc =
            BSON(RequiredNonStrictField3::kCppField1FieldName << 12 << "2" << 123 << "3" << 1234);
        auto testStruct = RequiredNonStrictField3::parse(ctxt, testDoc);

        assert_same_types<decltype(testStruct.getCppField1()), std::int32_t>();
        assert_same_types<decltype(testStruct.getCppField2()), std::int32_t>();
        assert_same_types<decltype(testStruct.getCppField3()), std::int32_t>();
    }

    // Negative: Missing 1 required field
    {
        auto testDoc = BSON("2" << 123 << "3" << 1234);
        ASSERT_THROWS(RequiredNonStrictField3::parse(ctxt, testDoc), AssertionException);
    }
    {
        auto testDoc = BSON("1" << 12 << "3" << 1234);
        ASSERT_THROWS(RequiredNonStrictField3::parse(ctxt, testDoc), AssertionException);
    }
    {
        auto testDoc = BSON("1" << 12 << "2" << 123);
        ASSERT_THROWS(RequiredNonStrictField3::parse(ctxt, testDoc), AssertionException);
    }

    // Positive: Extra field
    {
        auto testDoc = BSON("1" << 12 << "2" << 123 << "3" << 1234 << "field4" << 1234);
        RequiredNonStrictField3::parse(ctxt, testDoc);
    }

    // Negative: Duplicate field
    {
        auto testDoc = BSON("1" << 12 << "2" << 123 << "3" << 1234 << "2" << 12345);
        ASSERT_THROWS(RequiredNonStrictField3::parse(ctxt, testDoc), AssertionException);
    }

    // Negative: Duplicate extra field
    {
        auto testDoc =
            BSON("field4" << 1234 << "1" << 12 << "2" << 123 << "3" << 1234 << "field4" << 1234);
        ASSERT_THROWS(RequiredNonStrictField3::parse(ctxt, testDoc), AssertionException);
    }
}

/// Struct default comparison tests
TEST(IDLCompareTests, TestAllFields) {
    IDLParserErrorContext ctxt("root");

    // Positive: equality works
    {
        CompareAllField3 origStruct;
        origStruct.setField1(12);
        origStruct.setField2(123);
        origStruct.setField3(1234);

        auto testDoc = BSON("field1" << 12 << "field2" << 123 << "field3" << 1234);
        auto parsedStruct = CompareAllField3::parse(ctxt, testDoc);

        // Avoid ASSET_<RelOp> to avoid operator <<
        ASSERT_TRUE(origStruct == parsedStruct);
        ASSERT_FALSE(origStruct != parsedStruct);
        ASSERT_FALSE(origStruct < parsedStruct);
        ASSERT_FALSE(parsedStruct < origStruct);
    }

    // Positive: not equality works in field 3
    {
        CompareAllField3 origStruct;
        origStruct.setField1(12);
        origStruct.setField2(123);
        origStruct.setField3(12345);

        auto testDoc = BSON("field1" << 12 << "field2" << 123 << "field3" << 1234);
        auto parsedStruct = CompareAllField3::parse(ctxt, testDoc);

        // Avoid ASSET_<RelOp> to avoid operator <<
        ASSERT_FALSE(origStruct == parsedStruct);
        ASSERT_TRUE(origStruct != parsedStruct);
        ASSERT_FALSE(origStruct < parsedStruct);
        ASSERT_TRUE(parsedStruct < origStruct);
    }
}


/// Struct partial comparison tests
TEST(IDLCompareTests, TestSomeFields) {
    IDLParserErrorContext ctxt("root");

    // Positive: partial equality works when field 2 is different
    {
        CompareSomeField3 origStruct;
        origStruct.setField1(12);
        origStruct.setField2(12345);
        origStruct.setField3(1234);

        auto testDoc = BSON("field1" << 12 << "field2" << 123 << "field3" << 1234);
        auto parsedStruct = CompareSomeField3::parse(ctxt, testDoc);

        // Avoid ASSET_<RelOp> to avoid operator <<
        ASSERT_TRUE(origStruct == parsedStruct);
        ASSERT_FALSE(origStruct != parsedStruct);
        ASSERT_FALSE(origStruct < parsedStruct);
        ASSERT_FALSE(parsedStruct < origStruct);
    }

    // Positive: partial equality works when field 3 is different
    {
        CompareSomeField3 origStruct;
        origStruct.setField1(12);
        origStruct.setField2(1);
        origStruct.setField3(12345);

        auto testDoc = BSON("field1" << 12 << "field2" << 123 << "field3" << 1234);
        auto parsedStruct = CompareSomeField3::parse(ctxt, testDoc);

        // Avoid ASSET_<RelOp> to avoid operator <<
        ASSERT_FALSE(origStruct == parsedStruct);
        ASSERT_TRUE(origStruct != parsedStruct);
        ASSERT_FALSE(origStruct < parsedStruct);
        ASSERT_TRUE(parsedStruct < origStruct);
    }

    // Positive: partial equality works when field 1 is different
    {
        CompareSomeField3 origStruct;
        origStruct.setField1(123);
        origStruct.setField2(1);
        origStruct.setField3(1234);

        auto testDoc = BSON("field1" << 12 << "field2" << 123 << "field3" << 1234);
        auto parsedStruct = CompareSomeField3::parse(ctxt, testDoc);

        // Avoid ASSET_<RelOp> to avoid operator <<
        ASSERT_FALSE(origStruct == parsedStruct);
        ASSERT_TRUE(origStruct != parsedStruct);
        ASSERT_FALSE(origStruct < parsedStruct);
        ASSERT_TRUE(parsedStruct < origStruct);
    }
}

/// Field tests
// Positive: check ignored field is ignored
TEST(IDLFieldTests, TestStrictStructIgnoredField) {
    IDLParserErrorContext ctxt("root");

    // Positive: Test ignored field is ignored
    {
        auto testDoc = BSON("required_field" << 12 << "ignored_field" << 123);
        IgnoredField::parse(ctxt, testDoc);
    }

    // Positive: Test ignored field is not required
    {
        auto testDoc = BSON("required_field" << 12);
        IgnoredField::parse(ctxt, testDoc);
    }
}

// Negative: check duplicate ignored fields fail
TEST(IDLFieldTests, TestStrictDuplicateIgnoredFields) {
    IDLParserErrorContext ctxt("root");

    // Negative: Test duplicate ignored fields fail
    {
        auto testDoc =
            BSON("required_field" << 12 << "ignored_field" << 123 << "ignored_field" << 456);
        ASSERT_THROWS(IgnoredField::parse(ctxt, testDoc), AssertionException);
    }
}


// First test: test an empty document and the default value
// Second test: test a non-empty document and that we do not get the default value
#define TEST_DEFAULT_VALUES(field_name, default_value, new_value)   \
    {                                                               \
        auto testDoc = BSONObj();                                   \
        auto testStruct = Default_values::parse(ctxt, testDoc);     \
        ASSERT_EQUALS(testStruct.get##field_name(), default_value); \
    }                                                               \
    {                                                               \
        auto testDoc = BSON(#field_name << new_value);              \
        auto testStruct = Default_values::parse(ctxt, testDoc);     \
        ASSERT_EQUALS(testStruct.get##field_name(), new_value);     \
    }

// Mixed: struct strict, and ignored field works
TEST(IDLFieldTests, TestDefaultFields) {
    IDLParserErrorContext ctxt("root");

    TEST_DEFAULT_VALUES(V_string, "a default", "foo");
    TEST_DEFAULT_VALUES(V_int, 42, 3);
    TEST_DEFAULT_VALUES(V_long, 423, 4LL);
    TEST_DEFAULT_VALUES(V_double, 3.14159, 2.8);
    TEST_DEFAULT_VALUES(V_bool, true, false);
}

// Positive: struct strict, and optional field works
TEST(IDLFieldTests, TestOptionalFields) {
    IDLParserErrorContext ctxt("root");


    // Positive: Test document with only string field
    {
        auto testDoc = BSON("field1"
                            << "Foo");
        auto testStruct = Optional_field::parse(ctxt, testDoc);

        assert_same_types<decltype(testStruct.getField2()), const boost::optional<std::int32_t>>();
        assert_same_types<decltype(testStruct.getField1()),
                          const boost::optional<mongo::StringData>>();
        assert_same_types<decltype(testStruct.getField3()),
                          const boost::optional<mongo::BSONObj>&>();
        assert_same_types<decltype(testStruct.getField4()),
                          const boost::optional<ConstDataRange>>();
        assert_same_types<decltype(testStruct.getField5()),
                          const boost::optional<std::array<std::uint8_t, 16>>>();

        ASSERT_EQUALS("Foo", testStruct.getField1().get());
        ASSERT_FALSE(testStruct.getField2().is_initialized());
    }

    // Positive: Serialize struct with only string field
    {
        BSONObjBuilder builder;
        Optional_field testStruct;
        auto field1 = boost::optional<StringData>("Foo");
        testStruct.setField1(field1);
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        auto testDoc = BSON("field1"
                            << "Foo");
        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test document with only int field
    {
        auto testDoc = BSON("field2" << 123);
        auto testStruct = Optional_field::parse(ctxt, testDoc);
        ASSERT_FALSE(testStruct.getField1().is_initialized());
        ASSERT_EQUALS(123, testStruct.getField2().get());
    }

    // Positive: Serialize struct with only int field
    {
        BSONObjBuilder builder;
        Optional_field testStruct;
        testStruct.setField2(123);
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        auto testDoc = BSON("field2" << 123);
        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }
}

template <typename TestT>
void TestWeakType(TestT test_value) {
    IDLParserErrorContext ctxt("root");
    auto testDoc =
        BSON("field1" << test_value << "field2" << test_value << "field3" << test_value << "field4"
                      << test_value
                      << "field5"
                      << test_value);
    auto testStruct = Optional_field::parse(ctxt, testDoc);

    ASSERT_FALSE(testStruct.getField1().is_initialized());
    ASSERT_FALSE(testStruct.getField2().is_initialized());
    ASSERT_FALSE(testStruct.getField3().is_initialized());
    ASSERT_FALSE(testStruct.getField4().is_initialized());
    ASSERT_FALSE(testStruct.getField5().is_initialized());
}

// Positive: struct strict, and optional field works
TEST(IDLFieldTests, TestOptionalFieldsWithNullAndUndefined) {

    TestWeakType<NullLabeler>(BSONNULL);

    TestWeakType<UndefinedLabeler>(BSONUndefined);
}

// Positive: Test a nested struct
TEST(IDLNestedStruct, TestDuplicateTypes) {
    IDLParserErrorContext ctxt("root");


    // Positive: Test document
    auto testDoc = BSON(

        "field1" << BSON("field1" << 1 << "field2" << 2 << "field3" << 3) <<

        "field3" << BSON("field1" << 4 << "field2" << 5 << "field3" << 6));
    auto testStruct = NestedWithDuplicateTypes::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()), RequiredStrictField3&>();
    assert_same_types<decltype(testStruct.getField2()),
                      const boost::optional<RequiredNonStrictField3>&>();
    assert_same_types<decltype(testStruct.getField3()), RequiredStrictField3&>();

    ASSERT_EQUALS(1, testStruct.getField1().getField1());
    ASSERT_EQUALS(2, testStruct.getField1().getField2());
    ASSERT_EQUALS(3, testStruct.getField1().getField3());

    ASSERT_FALSE(testStruct.getField2());

    ASSERT_EQUALS(4, testStruct.getField3().getField1());
    ASSERT_EQUALS(5, testStruct.getField3().getField2());
    ASSERT_EQUALS(6, testStruct.getField3().getField3());

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        NestedWithDuplicateTypes nested_structs;
        RequiredStrictField3 f1;
        f1.setField1(1);
        f1.setField2(2);
        f1.setField3(3);
        nested_structs.setField1(f1);
        RequiredStrictField3 f3;
        f3.setField1(4);
        f3.setField2(5);
        f3.setField3(6);
        nested_structs.setField3(f3);
        nested_structs.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }
}

// Positive: Arrays of simple types
TEST(IDLArrayTests, TestSimpleArrays) {
    IDLParserErrorContext ctxt("root");

    // Positive: Test document
    uint8_t array1[] = {1, 2, 3};
    uint8_t array2[] = {4, 6, 8};

    uint8_t array15[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t array16[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    auto testDoc = BSON("field1" << BSON_ARRAY("Foo"
                                               << "Bar"
                                               << "???")
                                 << "field2"
                                 << BSON_ARRAY(1 << 2 << 3)
                                 << "field3"
                                 << BSON_ARRAY(1.2 << 3.4 << 5.6)
                                 << "field4"
                                 << BSON_ARRAY(BSONBinData(array1, 3, BinDataGeneral)
                                               << BSONBinData(array2, 3, BinDataGeneral))
                                 << "field5"
                                 << BSON_ARRAY(BSONBinData(array15, 16, newUUID)
                                               << BSONBinData(array16, 16, newUUID)));
    auto testStruct = Simple_array_fields::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()), const std::vector<mongo::StringData>>();
    assert_same_types<decltype(testStruct.getField2()), const std::vector<std::int32_t>&>();
    assert_same_types<decltype(testStruct.getField3()), const std::vector<double>&>();
    assert_same_types<decltype(testStruct.getField4()), const std::vector<ConstDataRange>>();
    assert_same_types<decltype(testStruct.getField5()),
                      const std::vector<std::array<std::uint8_t, 16>>&>();

    std::vector<StringData> field1{"Foo", "Bar", "???"};
    ASSERT_TRUE(field1 == testStruct.getField1());
    std::vector<std::int32_t> field2{1, 2, 3};
    ASSERT_TRUE(field2 == testStruct.getField2());
    std::vector<double> field3{1.2, 3.4, 5.6};
    ASSERT_TRUE(field3 == testStruct.getField3());

    std::vector<std::vector<uint8_t>> field4{{1, 2, 3}, {4, 6, 8}};
    ASSERT_TRUE(isEquals(testStruct.getField4(), field4));

    std::vector<std::array<uint8_t, 16>> field5{
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
        {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};
    ASSERT_TRUE(isEquals(testStruct.getField5(), field5));

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        Simple_array_fields array_fields;
        array_fields.setField1(field1);
        array_fields.setField2(field2);
        array_fields.setField3(field3);
        array_fields.setField4(transformVector(field4));
        array_fields.setField5(field5);
        array_fields.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }
}

// Positive: Optional Arrays
TEST(IDLArrayTests, TestSimpleOptionalArrays) {
    IDLParserErrorContext ctxt("root");

    // Positive: Test document
    auto testDoc = BSON("field1" << BSON_ARRAY("Foo"
                                               << "Bar"
                                               << "???")
                                 << "field2"
                                 << BSON_ARRAY(1 << 2 << 3)
                                 << "field3"
                                 << BSON_ARRAY(1.2 << 3.4 << 5.6)

                            );
    auto testStruct = Optional_array_fields::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()),
                      const boost::optional<std::vector<mongo::StringData>>>();
    assert_same_types<decltype(testStruct.getField2()),
                      const boost::optional<std::vector<std::int32_t>>&>();
    assert_same_types<decltype(testStruct.getField3()),
                      const boost::optional<std::vector<double>>&>();
    assert_same_types<decltype(testStruct.getField4()),
                      const boost::optional<std::vector<ConstDataRange>>>();
    assert_same_types<decltype(testStruct.getField5()),
                      const boost::optional<std::vector<std::array<std::uint8_t, 16>>>&>();

    std::vector<StringData> field1{"Foo", "Bar", "???"};
    ASSERT_TRUE(field1 == testStruct.getField1().get());
    std::vector<std::int32_t> field2{1, 2, 3};
    ASSERT_TRUE(field2 == testStruct.getField2().get());
    std::vector<double> field3{1.2, 3.4, 5.6};
    ASSERT_TRUE(field3 == testStruct.getField3().get());

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        Optional_array_fields array_fields;
        array_fields.setField1(field1);
        array_fields.setField2(field2);
        array_fields.setField3(field3);
        array_fields.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }
}

// Negative: Test mixed type arrays
TEST(IDLArrayTests, TestBadArrays) {
    IDLParserErrorContext ctxt("root");

    // Negative: Test not an array
    {
        auto testDoc = BSON("field1" << 123);

        ASSERT_THROWS(Simple_int_array::parse(ctxt, testDoc), AssertionException);
    }

    // Negative: Test array with mixed types
    {
        auto testDoc = BSON("field1" << BSON_ARRAY(1.2 << 3.4 << 5.6));

        ASSERT_THROWS(Simple_int_array::parse(ctxt, testDoc), AssertionException);
    }
}

// Positive: Test arrays with good field names but made with BSONObjBuilder
TEST(IDLArrayTests, TestGoodArrays) {
    IDLParserErrorContext ctxt("root");

    {
        BSONObjBuilder builder;
        {
            BSONObjBuilder subBuilder(builder.subarrayStart("field1"));
            subBuilder.append("0", 1);
            subBuilder.append("1", 2);
        }

        auto testDoc = builder.obj();

        Simple_int_array::parse(ctxt, testDoc);
    }
}

// Negative: Test arrays with bad field names
TEST(IDLArrayTests, TestBadArrayFieldNames) {
    IDLParserErrorContext ctxt("root");

    // Negative: string fields
    {
        BSONObjBuilder builder;
        {
            BSONObjBuilder subBuilder(builder.subarrayStart("field1"));
            subBuilder.append("0", 1);
            subBuilder.append("foo", 2);
        }
        auto testDoc = builder.obj();

        ASSERT_THROWS(Simple_int_array::parse(ctxt, testDoc), AssertionException);
    }

    // Negative: bad start
    {
        BSONObjBuilder builder;
        {
            BSONObjBuilder subBuilder(builder.subarrayStart("field1"));
            subBuilder.append("1", 1);
            subBuilder.append("2", 2);
        }
        auto testDoc = builder.obj();

        ASSERT_THROWS(Simple_int_array::parse(ctxt, testDoc), AssertionException);
    }

    // Negative: non-sequentially increasing
    {
        BSONObjBuilder builder;
        {
            BSONObjBuilder subBuilder(builder.subarrayStart("field1"));
            subBuilder.append("0", 1);
            subBuilder.append("2", 2);
        }
        auto testDoc = builder.obj();

        ASSERT_THROWS(Simple_int_array::parse(ctxt, testDoc), AssertionException);
    }
}

// Postitive: Test arrays with complex types
TEST(IDLArrayTests, TestArraysOfComplexTypes) {
    IDLParserErrorContext ctxt("root");

    // Positive: Test document
    auto testDoc = BSON("field1" << BSON_ARRAY(1 << 2 << 3) << "field2" << BSON_ARRAY("a.b"
                                                                                      << "c.d")
                                 << "field3"
                                 << BSON_ARRAY(1 << "2")
                                 << "field4"
                                 << BSON_ARRAY(BSONObj() << BSONObj())
                                 << "field5"
                                 << BSON_ARRAY(BSONObj() << BSONObj() << BSONObj())
                                 << "field6"
                                 << BSON_ARRAY(BSON("value"
                                                    << "hello")
                                               << BSON("value"
                                                       << "world"))
                                 << "field1o"
                                 << BSON_ARRAY(1 << 2 << 3)
                                 << "field2o"
                                 << BSON_ARRAY("a.b"
                                               << "c.d")
                                 << "field3o"
                                 << BSON_ARRAY(1 << "2")
                                 << "field4o"
                                 << BSON_ARRAY(BSONObj() << BSONObj())
                                 << "field6o"
                                 << BSON_ARRAY(BSON("value"
                                                    << "goodbye")
                                               << BSON("value"
                                                       << "world"))

                            );
    auto testStruct = Complex_array_fields::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()), const std::vector<std::int32_t>&>();
    assert_same_types<decltype(testStruct.getField2()),
                      const std::vector<mongo::NamespaceString>&>();
    assert_same_types<decltype(testStruct.getField3()), const std::vector<mongo::AnyBasicType>&>();
    assert_same_types<decltype(testStruct.getField4()),
                      const std::vector<mongo::ObjectBasicType>&>();
    assert_same_types<decltype(testStruct.getField5()), const std::vector<mongo::BSONObj>&>();
    assert_same_types<decltype(testStruct.getField6()),
                      const std::vector<mongo::idl::import::One_string>&>();

    assert_same_types<decltype(testStruct.getField1o()),
                      const boost::optional<std::vector<std::int32_t>>&>();
    assert_same_types<decltype(testStruct.getField2o()),
                      const boost::optional<std::vector<mongo::NamespaceString>>&>();
    assert_same_types<decltype(testStruct.getField3o()),
                      const boost::optional<std::vector<mongo::AnyBasicType>>&>();
    assert_same_types<decltype(testStruct.getField4o()),
                      const boost::optional<std::vector<mongo::ObjectBasicType>>&>();
    assert_same_types<decltype(testStruct.getField5o()),
                      const boost::optional<std::vector<mongo::BSONObj>>&>();
    assert_same_types<decltype(testStruct.getField6o()),
                      const boost::optional<std::vector<mongo::idl::import::One_string>>&>();

    std::vector<std::int32_t> field1{1, 2, 3};
    ASSERT_TRUE(field1 == testStruct.getField1());
    std::vector<NamespaceString> field2{{"a", "b"}, {"c", "d"}};
    ASSERT_TRUE(field2 == testStruct.getField2());

    ASSERT_EQUALS(testStruct.getField6().size(), 2u);
    ASSERT_EQUALS(testStruct.getField6()[0].getValue(), "hello");
    ASSERT_EQUALS(testStruct.getField6()[1].getValue(), "world");
    ASSERT_EQUALS(testStruct.getField6o().get().size(), 2u);
    ASSERT_EQUALS(testStruct.getField6o().get()[0].getValue(), "goodbye");
    ASSERT_EQUALS(testStruct.getField6o().get()[1].getValue(), "world");
}

template <typename ParserT, BinDataType bindata_type>
void TestBinDataVector() {
    IDLParserErrorContext ctxt("root");

    // Positive: Test document with only a generic bindata field
    uint8_t testData[] = {1, 2, 3};
    auto testDoc = BSON("value" << BSONBinData(testData, 3, bindata_type));
    auto testStruct = ParserT::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getValue()), const ConstDataRange>();

    std::vector<std::uint8_t> expected{1, 2, 3};

    ASSERT_TRUE(isEquals(testStruct.getValue(), expected));

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        ParserT one_new;
        one_new.setValue(expected);
        testStruct.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);

        // Validate the operator == works
        // Use ASSERT instead of ASSERT_EQ to avoid operator<<
        ASSERT(one_new == testStruct);
    }
}

TEST(IDLBinData, TestGeneric) {
    TestBinDataVector<One_bindata, BinDataGeneral>();
}

TEST(IDLBinData, TestFunction) {
    TestBinDataVector<One_function, Function>();
}

template <typename ParserT, BinDataType bindata_type>
void TestBinDataArray() {
    IDLParserErrorContext ctxt("root");

    // Positive: Test document with only a generic bindata field
    uint8_t testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    auto testDoc = BSON("value" << BSONBinData(testData, 16, bindata_type));
    auto testStruct = ParserT::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getValue()), const std::array<uint8_t, 16>>();

    std::array<std::uint8_t, 16> expected{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    ASSERT_TRUE(isEquals(testStruct.getValue(), expected));

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        ParserT one_new;
        one_new.setValue(expected);
        one_new.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }
}

TEST(IDLBinData, TestUUID) {
    TestBinDataArray<One_uuid, newUUID>();
}

TEST(IDLBinData, TestMD5) {
    TestBinDataArray<One_md5, MD5Type>();

    // Negative: Test document with a incorrectly size md5 field
    {
        IDLParserErrorContext ctxt("root");

        uint8_t testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        auto testDoc = BSON("value" << BSONBinData(testData, 15, MD5Type));
        ASSERT_THROWS(One_md5::parse(ctxt, testDoc), AssertionException);
    }
}

// Test if a given value for a given bson document parses successfully or fails if the bson types
// mismatch.
template <typename ParserT, BinDataType Parser_bindata_type, BinDataType Test_bindata_type>
void TestBinDataParse() {
    IDLParserErrorContext ctxt("root");

    uint8_t testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    auto testDoc = BSON("value" << BSONBinData(testData, 16, Test_bindata_type));

    auto element = testDoc.firstElement();
    ASSERT_EQUALS(element.type(), BinData);
    ASSERT_EQUALS(element.binDataType(), Test_bindata_type);

    if (Parser_bindata_type != Test_bindata_type) {
        ASSERT_THROWS(ParserT::parse(ctxt, testDoc), AssertionException);
    } else {
        (void)ParserT::parse(ctxt, testDoc);
    }
}

template <typename ParserT, BinDataType Parser_bindata_type>
void TestBinDataParser() {
    TestBinDataParse<ParserT, Parser_bindata_type, BinDataGeneral>();
    TestBinDataParse<ParserT, Parser_bindata_type, Function>();
    TestBinDataParse<ParserT, Parser_bindata_type, MD5Type>();
    TestBinDataParse<ParserT, Parser_bindata_type, newUUID>();
}

TEST(IDLBinData, TestParse) {
    TestBinDataParser<One_bindata, BinDataGeneral>();
    TestBinDataParser<One_function, Function>();
    TestBinDataParser<One_uuid, newUUID>();
    TestBinDataParser<One_md5, MD5Type>();
    TestBinDataParser<One_UUID, newUUID>();
}

// Mixed: test a type that accepts a custom bindata type
TEST(IDLBinData, TestCustomType) {
    IDLParserErrorContext ctxt("root");

    uint8_t testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    auto testDoc = BSON("value" << BSONBinData(testData, 14, BinDataGeneral));

    auto element = testDoc.firstElement();
    ASSERT_EQUALS(element.type(), BinData);
    ASSERT_EQUALS(element.binDataType(), BinDataGeneral);

    auto testStruct = One_bindata_custom::parse(ctxt, testDoc);
    std::vector<std::uint8_t> testVector = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    ASSERT_TRUE(testStruct.getValue().getVector() == testVector);

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        One_bindata_custom one_new;
        one_new.setValue(testVector);
        one_new.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }
}

// Positive: test a type that accepts a custom UUID type
TEST(IDLBinData, TestUUIDclass) {
    IDLParserErrorContext ctxt("root");

    auto uuid = UUID::gen();
    auto testDoc = BSON("value" << uuid);

    auto element = testDoc.firstElement();
    ASSERT_EQUALS(element.type(), BinData);
    ASSERT_EQUALS(element.binDataType(), newUUID);

    auto testStruct = One_UUID::parse(ctxt, testDoc);
    ASSERT_TRUE(testStruct.getValue() == uuid);

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        One_UUID one_new;
        one_new.setValue(uuid);
        one_new.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }
}

/**
 * A simple class that derives from an IDL generated class
 */
class ClassDerivedFromStruct : public DerivedBaseStruct {
public:
    static ClassDerivedFromStruct parseFromBSON(const IDLParserErrorContext& ctxt,
                                                const BSONObj& bsonObject) {
        ClassDerivedFromStruct o;
        o.parseProtected(ctxt, bsonObject);
        o._done = true;
        return o;
    }

    bool aRandomAdditionalMethod() {
        return true;
    }

    bool getDone() const {
        return _done;
    }

private:
    bool _done = false;
};

// Positive: demonstrate a class derived from an IDL parser.
TEST(IDLCustomType, TestDerivedParser) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON("field1" << 3 << "field2" << 5);

    auto testStruct = ClassDerivedFromStruct::parseFromBSON(ctxt, testDoc);
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getField2(), 5);

    ASSERT_EQUALS(testStruct.getDone(), true);

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        ClassDerivedFromStruct one_new;
        one_new.setField1(3);
        one_new.setField2(5);
        one_new.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }
}

// Chained type testing
// Check each of types
// Check for round-tripping of fields and documents

// Positive: demonstrate a class struct chained types
TEST(IDLChainedType, TestChainedType) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON("field1"
                        << "abc"
                        << "field2"
                        << 5);

    auto testStruct = Chained_struct_only::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getChainedType()), const mongo::ChainedType&>();
    assert_same_types<decltype(testStruct.getAnotherChainedType()),
                      const mongo::AnotherChainedType&>();

    ASSERT_EQUALS(testStruct.getChainedType().getField1(), "abc");
    ASSERT_EQUALS(testStruct.getAnotherChainedType().getField2(), 5);

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        Chained_struct_only one_new;
        ChainedType ct;
        ct.setField1("abc");
        one_new.setChainedType(ct);
        AnotherChainedType act;
        act.setField2(5);
        one_new.setAnotherChainedType(act);
        one_new.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }
}

// Positive: demonstrate a struct with chained types ignoring extra fields
TEST(IDLChainedType, TestExtraFields) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON("field1"
                        << "abc"
                        << "field2"
                        << 5
                        << "field3"
                        << 123456);

    auto testStruct = Chained_struct_only::parse(ctxt, testDoc);
    ASSERT_EQUALS(testStruct.getChainedType().getField1(), "abc");
    ASSERT_EQUALS(testStruct.getAnotherChainedType().getField2(), 5);
}


// Negative: demonstrate a struct with chained types with duplicate fields
TEST(IDLChainedType, TestDuplicateFields) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON("field1"
                        << "abc"
                        << "field2"
                        << 5
                        << "field2"
                        << 123456);

    ASSERT_THROWS(Chained_struct_only::parse(ctxt, testDoc), AssertionException);
}


// Positive: demonstrate a struct with chained structs
TEST(IDLChainedType, TestChainedStruct) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON("anyField" << 123.456 << "objectField" << BSON("random"
                                                                       << "pair")
                                   << "field3"
                                   << "abc");

    auto testStruct = Chained_struct_mixed::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getChained_any_basic_type()), Chained_any_basic_type&>();
    assert_same_types<decltype(testStruct.getChainedObjectBasicType()),
                      Chained_object_basic_type&>();

    ASSERT_EQUALS(testStruct.getField3(), "abc");

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }
}

// Negative: demonstrate a struct with chained structs and extra fields
TEST(IDLChainedType, TestChainedStructWithExtraFields) {
    IDLParserErrorContext ctxt("root");

    // Extra field
    {
        auto testDoc = BSON("field3"
                            << "abc"
                            << "anyField"
                            << 123.456
                            << "objectField"
                            << BSON("random"
                                    << "pair")
                            << "extraField"
                            << 787);
        ASSERT_THROWS(Chained_struct_mixed::parse(ctxt, testDoc), AssertionException);
    }


    // Duplicate any
    {
        auto testDoc = BSON("field3"
                            << "abc"
                            << "anyField"
                            << 123.456
                            << "objectField"
                            << BSON("random"
                                    << "pair")
                            << "anyField"
                            << 787);
        ASSERT_THROWS(Chained_struct_mixed::parse(ctxt, testDoc), AssertionException);
    }

    // Duplicate object
    {
        auto testDoc = BSON("objectField" << BSON("fake"
                                                  << "thing")
                                          << "field3"
                                          << "abc"
                                          << "anyField"
                                          << 123.456
                                          << "objectField"
                                          << BSON("random"
                                                  << "pair"));
        ASSERT_THROWS(Chained_struct_mixed::parse(ctxt, testDoc), AssertionException);
    }

    // Duplicate field3
    {
        auto testDoc = BSON("field3"
                            << "abc"
                            << "anyField"
                            << 123.456
                            << "objectField"
                            << BSON("random"
                                    << "pair")
                            << "field3"
                            << "def");
        ASSERT_THROWS(Chained_struct_mixed::parse(ctxt, testDoc), AssertionException);
    }
}


// Positive: demonstrate a struct with chained structs and types
TEST(IDLChainedType, TestChainedMixedStruct) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON("field1"
                        << "abc"
                        << "field2"
                        << 5
                        << "stringField"
                        << "def"
                        << "field3"
                        << 456);

    auto testStruct = Chained_struct_type_mixed::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getChained_type()), const mongo::ChainedType&>();
    assert_same_types<decltype(testStruct.getAnotherChainedType()),
                      const mongo::AnotherChainedType&>();

    ASSERT_EQUALS(testStruct.getChained_type().getField1(), "abc");
    ASSERT_EQUALS(testStruct.getAnotherChainedType().getField2(), 5);
    ASSERT_EQUALS(testStruct.getChainedStringBasicType().getStringField(), "def");
    ASSERT_EQUALS(testStruct.getField3(), 456);

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        Chained_struct_type_mixed one_new;
        ChainedType ct;
        ct.setField1("abc");
        one_new.setChained_type(ct);
        AnotherChainedType act;
        act.setField2(5);
        one_new.setAnotherChainedType(act);
        one_new.setField3(456);
        Chained_string_basic_type csbt;
        csbt.setStringField("def");
        one_new.setChainedStringBasicType(csbt);
        one_new.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
    }
}
// Positive: demonstrate a class derived from an IDL parser.
TEST(IDLEnum, TestEnum) {

    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON("field1" << 2 << "field2"
                                 << "zero");
    auto testStruct = StructWithEnum::parse(ctxt, testDoc);
    ASSERT_TRUE(testStruct.getField1() == IntEnum::c2);
    ASSERT_TRUE(testStruct.getField2() == StringEnumEnum::s0);
    ASSERT_TRUE(testStruct.getFieldDefault() == StringEnumEnum::s1);

    assert_same_types<decltype(testStruct.getField1()), IntEnum>();
    assert_same_types<decltype(testStruct.getField1o()), const boost::optional<IntEnum>>();
    assert_same_types<decltype(testStruct.getField2()), StringEnumEnum>();
    assert_same_types<decltype(testStruct.getField2o()), const boost::optional<StringEnumEnum>>();
    assert_same_types<decltype(testStruct.getFieldDefault()), StringEnumEnum>();

    auto testSerializedDoc = BSON("field1" << 2 << "field2"
                                           << "zero"
                                           << "fieldDefault"
                                           << "one");


    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(&builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testSerializedDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        StructWithEnum one_new;
        one_new.setField1(IntEnum::c2);
        one_new.setField2(StringEnumEnum::s0);
        one_new.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testSerializedDoc, serializedDoc);
    }
}


// Negative: test bad values
TEST(IDLEnum, TestIntEnumNegative) {
    IDLParserErrorContext ctxt("root");

    //  Test string
    {
        auto testDoc = BSON("value"
                            << "2");
        ASSERT_THROWS(One_int_enum::parse(ctxt, testDoc), AssertionException);
    }

    // Test a value out of range
    {
        auto testDoc = BSON("value" << 4);
        ASSERT_THROWS(One_int_enum::parse(ctxt, testDoc), AssertionException);
    }

    // Test a negative number
    {
        auto testDoc = BSON("value" << -1);
        ASSERT_THROWS(One_int_enum::parse(ctxt, testDoc), AssertionException);
    }
}

TEST(IDLEnum, TestStringEnumNegative) {
    IDLParserErrorContext ctxt("root");

    //  Test int
    {
        auto testDoc = BSON("value" << 2);
        ASSERT_THROWS(One_string_enum::parse(ctxt, testDoc), AssertionException);
    }

    // Test a value out of range
    {
        auto testDoc = BSON("value"
                            << "foo");
        ASSERT_THROWS(One_string_enum::parse(ctxt, testDoc), AssertionException);
    }
}

OpMsgRequest makeOMR(BSONObj obj) {
    OpMsgRequest request;
    request.body = obj;
    return request;
}

// Positive: demonstrate a command with concatenate with db
TEST(IDLCommand, TestConcatentateWithDb) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON(BasicConcatenateWithDbCommand::kCommandName << "coll1"
                                                                    << "field1"
                                                                    << 3
                                                                    << "field2"
                                                                    << "five"
                                                                    << "$db"
                                                                    << "db");

    auto testStruct = BasicConcatenateWithDbCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getField2(), "five");
    ASSERT_EQUALS(testStruct.getNamespace(), NamespaceString("db.coll1"));

    assert_same_types<decltype(testStruct.getNamespace()), const NamespaceString&>();

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        OpMsgRequest reply = testStruct.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }

    // Positive: Test we can serialize from nothing the same document except for $db
    {
        auto testDocWithoutDb = BSON(BasicConcatenateWithDbCommand::kCommandName << "coll1"
                                                                                 << "field1"
                                                                                 << 3
                                                                                 << "field2"
                                                                                 << "five");

        BSONObjBuilder builder;
        BasicConcatenateWithDbCommand one_new(NamespaceString("db.coll1"));
        one_new.setField1(3);
        one_new.setField2("five");
        one_new.serialize(BSONObj(), &builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDocWithoutDb, serializedDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        BasicConcatenateWithDbCommand one_new(NamespaceString("db.coll1"));
        one_new.setField1(3);
        one_new.setField2("five");
        OpMsgRequest reply = one_new.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }
}

TEST(IDLCommand, TestConcatentateWithDbSymbol) {
    IDLParserErrorContext ctxt("root");

    // Postive - symbol???
    {
        auto testDoc =
            BSON("BasicConcatenateWithDbCommand" << BSONSymbol("coll1") << "field1" << 3 << "field2"
                                                 << "five"
                                                 << "$db"
                                                 << "db");
        auto testStruct = BasicConcatenateWithDbCommand::parse(ctxt, makeOMR(testDoc));
        ASSERT_EQUALS(testStruct.getNamespace(), NamespaceString("db.coll1"));
    }
}


TEST(IDLCommand, TestConcatentateWithDbNegative) {
    IDLParserErrorContext ctxt("root");

    // Negative - duplicate namespace field
    {
        auto testDoc = BSON("BasicConcatenateWithDbCommand" << 1 << "field1" << 3
                                                            << "BasicConcatenateWithDbCommand"
                                                            << 1
                                                            << "field2"
                                                            << "five");
        ASSERT_THROWS(BasicConcatenateWithDbCommand::parse(ctxt, makeOMR(testDoc)),
                      AssertionException);
    }

    // Negative -  namespace field wrong order
    {
        auto testDoc = BSON("field1" << 3 << "BasicConcatenateWithDbCommand" << 1 << "field2"
                                     << "five");
        ASSERT_THROWS(BasicConcatenateWithDbCommand::parse(ctxt, makeOMR(testDoc)),
                      AssertionException);
    }

    // Negative -  namespace missing
    {
        auto testDoc = BSON("field1" << 3 << "field2"
                                     << "five");
        ASSERT_THROWS(BasicConcatenateWithDbCommand::parse(ctxt, makeOMR(testDoc)),
                      AssertionException);
    }

    // Negative - wrong type
    {
        auto testDoc = BSON("BasicConcatenateWithDbCommand" << 1 << "field1" << 3 << "field2"
                                                            << "five");
        ASSERT_THROWS(BasicConcatenateWithDbCommand::parse(ctxt, makeOMR(testDoc)),
                      AssertionException);
    }

    // Negative - bad ns with embedded null
    {
        StringData sd1("db\0foo", 6);
        auto testDoc = BSON("BasicConcatenateWithDbCommand" << sd1 << "field1" << 3 << "field2"
                                                            << "five");
        ASSERT_THROWS(BasicConcatenateWithDbCommand::parse(ctxt, makeOMR(testDoc)),
                      AssertionException);
    }
}

// Positive: demonstrate a command with concatenate with db or uuid - test NSS
TEST(IDLCommand, TestConcatentateWithDbOrUUID_TestNSS) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON(BasicConcatenateWithDbOrUUIDCommand::kCommandName << "coll1"
                                                                          << "field1"
                                                                          << 3
                                                                          << "field2"
                                                                          << "five"
                                                                          << "$db"
                                                                          << "db");

    auto testStruct = BasicConcatenateWithDbOrUUIDCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getField2(), "five");
    ASSERT_EQUALS(testStruct.getNamespaceOrUUID().nss().get(), NamespaceString("db.coll1"));

    assert_same_types<decltype(testStruct.getNamespaceOrUUID()), const NamespaceStringOrUUID&>();

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        OpMsgRequest reply = testStruct.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }

    // Positive: Test we can serialize from nothing the same document except for $db
    {
        auto testDocWithoutDb = BSON(BasicConcatenateWithDbOrUUIDCommand::kCommandName << "coll1"
                                                                                       << "field1"
                                                                                       << 3
                                                                                       << "field2"
                                                                                       << "five");

        BSONObjBuilder builder;
        BasicConcatenateWithDbOrUUIDCommand one_new(NamespaceString("db.coll1"));
        one_new.setField1(3);
        one_new.setField2("five");
        one_new.serialize(BSONObj(), &builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDocWithoutDb, serializedDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        BasicConcatenateWithDbOrUUIDCommand one_new(NamespaceString("db.coll1"));
        one_new.setField1(3);
        one_new.setField2("five");
        OpMsgRequest reply = one_new.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }
}


// Positive: demonstrate a command with concatenate with db or uuid - test UUID
TEST(IDLCommand, TestConcatentateWithDbOrUUID_TestUUID) {
    IDLParserErrorContext ctxt("root");

    UUID uuid = UUID::gen();

    auto testDoc =
        BSON(BasicConcatenateWithDbOrUUIDCommand::kCommandName << uuid << "field1" << 3 << "field2"
                                                               << "five"
                                                               << "$db"
                                                               << "db");

    auto testStruct = BasicConcatenateWithDbOrUUIDCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getField2(), "five");
    ASSERT_EQUALS(testStruct.getNamespaceOrUUID().uuid().get(), uuid);

    assert_same_types<decltype(testStruct.getNamespaceOrUUID()), const NamespaceStringOrUUID&>();

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        OpMsgRequest reply = testStruct.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }

    // Positive: Test we can serialize from nothing the same document except for $db
    {
        auto testDocWithoutDb = BSON(
            BasicConcatenateWithDbOrUUIDCommand::kCommandName << uuid << "field1" << 3 << "field2"
                                                              << "five");

        BSONObjBuilder builder;
        BasicConcatenateWithDbOrUUIDCommand one_new(NamespaceStringOrUUID("db", uuid));
        one_new.setField1(3);
        one_new.setField2("five");
        one_new.serialize(BSONObj(), &builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDocWithoutDb, serializedDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        BasicConcatenateWithDbOrUUIDCommand one_new(NamespaceStringOrUUID("db", uuid));
        one_new.setField1(3);
        one_new.setField2("five");
        OpMsgRequest reply = one_new.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }
}


TEST(IDLCommand, TestConcatentateWithDbOrUUIDNegative) {
    IDLParserErrorContext ctxt("root");

    // Negative - duplicate namespace field
    {
        auto testDoc =
            BSON("BasicConcatenateWithDbOrUUIDCommand" << 1 << "field1" << 3
                                                       << "BasicConcatenateWithDbOrUUIDCommand"
                                                       << 1
                                                       << "field2"
                                                       << "five");
        ASSERT_THROWS(BasicConcatenateWithDbOrUUIDCommand::parse(ctxt, makeOMR(testDoc)),
                      AssertionException);
    }

    // Negative -  namespace field wrong order
    {
        auto testDoc = BSON("field1" << 3 << "BasicConcatenateWithDbOrUUIDCommand" << 1 << "field2"
                                     << "five");
        ASSERT_THROWS(BasicConcatenateWithDbOrUUIDCommand::parse(ctxt, makeOMR(testDoc)),
                      AssertionException);
    }

    // Negative -  namespace missing
    {
        auto testDoc = BSON("field1" << 3 << "field2"
                                     << "five");
        ASSERT_THROWS(BasicConcatenateWithDbOrUUIDCommand::parse(ctxt, makeOMR(testDoc)),
                      AssertionException);
    }

    // Negative - wrong type
    {
        auto testDoc = BSON("BasicConcatenateWithDbOrUUIDCommand" << 1 << "field1" << 3 << "field2"
                                                                  << "five");
        ASSERT_THROWS(BasicConcatenateWithDbOrUUIDCommand::parse(ctxt, makeOMR(testDoc)),
                      AssertionException);
    }

    // Negative - bad ns with embedded null
    {
        StringData sd1("db\0foo", 6);
        auto testDoc =
            BSON("BasicConcatenateWithDbOrUUIDCommand" << sd1 << "field1" << 3 << "field2"
                                                       << "five");
        ASSERT_THROWS(BasicConcatenateWithDbOrUUIDCommand::parse(ctxt, makeOMR(testDoc)),
                      AssertionException);
    }
}


// Positive: demonstrate a command with concatenate with db
TEST(IDLCommand, TestIgnore) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON("BasicIgnoredCommand" << 1 << "field1" << 3 << "field2"
                                              << "five");

    auto testDocWithDB = appendDB(testDoc, "admin");

    auto testStruct = BasicIgnoredCommand::parse(ctxt, makeOMR(testDocWithDB));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getField2(), "five");

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(BSONObj(), &builder);
        auto loopbackDoc = builder.obj();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        BasicIgnoredCommand one_new;
        one_new.setField1(3);
        one_new.setField2("five");
        one_new.setDbName("admin");
        OpMsgRequest reply = one_new.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDocWithDB, reply.body);
    }
}


TEST(IDLCommand, TestIgnoredNegative) {
    IDLParserErrorContext ctxt("root");

    // Negative - duplicate namespace field
    {
        auto testDoc = BSON(
            "BasicIgnoredCommand" << 1 << "field1" << 3 << "BasicIgnoredCommand" << 1 << "field2"
                                  << "five");
        ASSERT_THROWS(BasicIgnoredCommand::parse(ctxt, makeOMR(testDoc)), AssertionException);
    }

    // Negative -  namespace field wrong order
    {
        auto testDoc = BSON("field1" << 3 << "BasicIgnoredCommand" << 1 << "field2"
                                     << "five");
        ASSERT_THROWS(BasicIgnoredCommand::parse(ctxt, makeOMR(testDoc)), AssertionException);
    }

    // Negative -  namespace missing
    {
        auto testDoc = BSON("field1" << 3 << "field2"
                                     << "five");
        ASSERT_THROWS(BasicIgnoredCommand::parse(ctxt, makeOMR(testDoc)), AssertionException);
    }
}

// Positive: Test a command read and written to OpMsgRequest works
TEST(IDLDocSequence, TestBasic) {
    IDLParserErrorContext ctxt("root");

    auto testTempDoc = BSON("DocSequenceCommand"
                            << "coll1"
                            << "field1"
                            << 3
                            << "field2"
                            << "five"
                            << "$db"
                            << "db"
                            << "structs"
                            << BSON_ARRAY(BSON("value"
                                               << "hello")
                                          << BSON("value"
                                                  << "world"))
                            << "objects"
                            << BSON_ARRAY(BSON("foo" << 1)));

    OpMsgRequest request;
    request.body = testTempDoc;

    auto testStruct = DocSequenceCommand::parse(ctxt, request);
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getField2(), "five");
    ASSERT_EQUALS(testStruct.getNamespace(), NamespaceString("db.coll1"));

    ASSERT_EQUALS(2UL, testStruct.getStructs().size());
    ASSERT_EQUALS("hello", testStruct.getStructs()[0].getValue());
    ASSERT_EQUALS("world", testStruct.getStructs()[1].getValue());

    assert_same_types<decltype(testStruct.getNamespace()), const NamespaceString&>();

    // Positive: Test we can round trip to a document sequence from the just parsed document
    {
        OpMsgRequest loopbackRequest = testStruct.serialize(BSONObj());

        assertOpMsgEquals(request, loopbackRequest);
        ASSERT_EQUALS(loopbackRequest.sequences.size(), 2UL);
    }

    // Positive: Test we can roundtrip just the body from the just parsed document
    {
        BSONObjBuilder builder;
        testStruct.serialize(BSONObj(), &builder);

        auto testTempDocWithoutDB = testTempDoc.removeField("$db");

        ASSERT_BSONOBJ_EQ(testTempDocWithoutDB, builder.obj());
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        DocSequenceCommand one_new(NamespaceString("db.coll1"));
        one_new.setField1(3);
        one_new.setField2("five");

        std::vector<One_string> strings;
        One_string one_string;
        one_string.setValue("hello");
        strings.push_back(one_string);
        One_string one_string2;
        one_string2.setValue("world");
        strings.push_back(one_string2);
        one_new.setStructs(strings);

        std::vector<BSONObj> objects;
        objects.push_back(BSON("foo" << 1));
        one_new.setObjects(objects);

        OpMsgRequest serializeRequest = one_new.serialize(BSONObj());

        assertOpMsgEquals(request, serializeRequest);
    }
}

// Negative: Test a OpMsgRequest read without $db
TEST(IDLDocSequence, TestMissingDB) {
    IDLParserErrorContext ctxt("root");

    auto testTempDoc = BSON("DocSequenceCommand"
                            << "coll1"
                            << "field1"
                            << 3
                            << "field2"
                            << "five"
                            << "structs"
                            << BSON_ARRAY(BSON("value"
                                               << "hello"))
                            << "objects"
                            << BSON_ARRAY(BSON("foo" << 1)));

    OpMsgRequest request;
    request.body = testTempDoc;

    ASSERT_THROWS(DocSequenceCommand::parse(ctxt, request), AssertionException);
}

// Positive: Test a command read and written to OpMsgRequest with content in DocumentSequence works
template <typename TestT>
void TestDocSequence(StringData name) {
    IDLParserErrorContext ctxt("root");

    auto testTempDoc = BSON(name << "coll1"
                                 << "field1"
                                 << 3
                                 << "field2"
                                 << "five");

    OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
    request.sequences.push_back({"structs",
                                 {BSON("value"
                                       << "hello"),
                                  BSON("value"
                                       << "world")}});
    request.sequences.push_back({"objects", {BSON("foo" << 1)}});

    auto testStruct = TestT::parse(ctxt, request);
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getField2(), "five");
    ASSERT_EQUALS(testStruct.getNamespace(), NamespaceString("db.coll1"));

    ASSERT_EQUALS(2UL, testStruct.getStructs().size());
    ASSERT_EQUALS("hello", testStruct.getStructs()[0].getValue());
    ASSERT_EQUALS("world", testStruct.getStructs()[1].getValue());

    auto opmsg = testStruct.serialize(BSONObj());
    ASSERT_EQUALS(2UL, opmsg.sequences.size());

    assertOpMsgEquals(opmsg, request);
    assertOpMsgEqualsExact(opmsg, request);
}

// Positive: Test a command read and written to OpMsgRequest with content in DocumentSequence works
TEST(IDLDocSequence, TestDocSequence) {
    TestDocSequence<DocSequenceCommand>("DocSequenceCommand");
    TestDocSequence<DocSequenceCommandNonStrict>("DocSequenceCommandNonStrict");
}

// Negative: Bad Doc Sequences
template <typename TestT>
void TestBadDocSequences(StringData name, bool extraFieldAllowed) {
    IDLParserErrorContext ctxt("root");

    auto testTempDoc = BSON(name << "coll1"
                                 << "field1"
                                 << 3
                                 << "field2"
                                 << "five");

    // Negative: Duplicate fields in doc sequence
    {
        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"structs",
                                     {BSON("value"
                                           << "hello"),
                                      BSON("value"
                                           << "world")}});
        request.sequences.push_back({"structs", {BSON("foo" << 1)}});

        ASSERT_THROWS(TestT::parse(ctxt, request), AssertionException);
    }

    // Negative: Extra field in document sequence
    {
        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"structs",
                                     {BSON("value"
                                           << "hello"),
                                      BSON("value"
                                           << "world")}});
        request.sequences.push_back({"objects", {BSON("foo" << 1)}});
        request.sequences.push_back({"extra", {BSON("foo" << 1)}});

        if (!extraFieldAllowed) {
            ASSERT_THROWS(TestT::parse(ctxt, request), AssertionException);
        } else {
            /*void*/ TestT::parse(ctxt, request);
        }
    }

    // Negative: Missing field in both document sequence and body
    {
        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"objects", {BSON("foo" << 1)}});

        ASSERT_THROWS(TestT::parse(ctxt, request), AssertionException);
    }

    // Negative: Missing field in both document sequence and body
    {
        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"structs",
                                     {BSON("value"
                                           << "hello"),
                                      BSON("value"
                                           << "world")}});

        ASSERT_THROWS(TestT::parse(ctxt, request), AssertionException);
    }
}

// Negative: Bad Doc Sequences
TEST(IDLDocSequence, TestBadDocSequences) {
    TestBadDocSequences<DocSequenceCommand>("DocSequenceCommand", false);
    TestBadDocSequences<DocSequenceCommandNonStrict>("DocSequenceCommandNonStrict", true);
}

// Negative: Duplicate field across body and document sequence
template <typename TestT>
void TestDuplicateDocSequences(StringData name) {
    IDLParserErrorContext ctxt("root");

    // Negative: Duplicate fields in doc sequence and body
    {
        auto testTempDoc = BSON(name << "coll1"
                                     << "field1"
                                     << 3
                                     << "field2"
                                     << "five"
                                     << "structs"
                                     << BSON_ARRAY(BSON("value"
                                                        << "hello")
                                                   << BSON("value"
                                                           << "world"))
                                     << "objects"
                                     << BSON_ARRAY(BSON("foo" << 1)));

        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"structs",
                                     {BSON("value"
                                           << "hello"),
                                      BSON("value"
                                           << "world")}});

        ASSERT_THROWS(DocSequenceCommand::parse(ctxt, request), AssertionException);
    }

    // Negative: Duplicate fields in doc sequence and body
    {
        auto testTempDoc = BSON(name << "coll1"
                                     << "field1"
                                     << 3
                                     << "field2"
                                     << "five"
                                     << "structs"
                                     << BSON_ARRAY(BSON("value"
                                                        << "hello")
                                                   << BSON("value"
                                                           << "world"))
                                     << "objects"
                                     << BSON_ARRAY(BSON("foo" << 1)));

        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"objects", {BSON("foo" << 1)}});

        ASSERT_THROWS(DocSequenceCommand::parse(ctxt, request), AssertionException);
    }
}

// Negative: Duplicate field across body and document sequence
TEST(IDLDocSequence, TestDuplicateDocSequences) {
    TestDuplicateDocSequences<DocSequenceCommand>("DocSequenceCommand");
    TestDuplicateDocSequences<DocSequenceCommandNonStrict>("DocSequenceCommandNonStrict");
}

// Positive: Test empty document sequence
TEST(IDLDocSequence, TestEmptySequence) {
    IDLParserErrorContext ctxt("root");

    // Negative: Duplicate fields in doc sequence and body
    {
        auto testTempDoc = BSON("DocSequenceCommand"
                                << "coll1"
                                << "field1"
                                << 3
                                << "field2"
                                << "five"
                                << "structs"
                                << BSON_ARRAY(BSON("value"
                                                   << "hello")
                                              << BSON("value"
                                                      << "world"))
                                << "objects"
                                << BSON_ARRAY(BSON("foo" << 1)));

        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"structs", {}});

        ASSERT_THROWS(DocSequenceCommand::parse(ctxt, request), AssertionException);
    }

    // Positive: Empty document sequence
    {
        auto testTempDoc = BSON("DocSequenceCommand"
                                << "coll1"
                                << "field1"
                                << 3
                                << "field2"
                                << "five"
                                << "objects"
                                << BSON_ARRAY(BSON("foo" << 1)));

        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"structs", {}});

        auto testStruct = DocSequenceCommand::parse(ctxt, request);

        ASSERT_EQUALS(0UL, testStruct.getStructs().size());
    }
}

// Positive: Test all the OpMsg well known fields are ignored
TEST(IDLDocSequence, TestWellKnownFieldsAreIgnored) {
    IDLParserErrorContext ctxt("root");

    auto knownFields = {"$audit",
                        "$client",
                        "$configServerState",
                        "$oplogQueryData",
                        "$queryOptions",
                        "$readPreference",
                        "$replData",
                        "$clusterTime",
                        "maxTimeMS",
                        "readConcern",
                        "shardVersion",
                        "tracking_info",
                        "writeConcern"};

    for (auto knownField : knownFields) {
        auto testTempDoc = BSON("DocSequenceCommand"
                                << "coll1"
                                << "field1"
                                << 3
                                << "field2"
                                << "five"
                                << knownField
                                << "extra"
                                << "structs"
                                << BSON_ARRAY(BSON("value"
                                                   << "hello")
                                              << BSON("value"
                                                      << "world"))
                                << "objects"
                                << BSON_ARRAY(BSON("foo" << 1)));


        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);

        // Validate it can be parsed as a OpMsgRequest.
        {
            auto testStruct = DocSequenceCommand::parse(ctxt, request);
            ASSERT_EQUALS(2UL, testStruct.getStructs().size());
        }

        // Validate it can be parsed as just a BSON document.
        {
            auto testStruct = DocSequenceCommand::parse(ctxt, request.body);
            ASSERT_EQUALS(2UL, testStruct.getStructs().size());
        }
    }
}

// Positive: Test all the OpMsg well known fields are passed through except $db.
TEST(IDLDocSequence, TestWellKnownFieldsPassthrough) {
    IDLParserErrorContext ctxt("root");

    auto knownFields = {"$audit",
                        "$client",
                        "$configServerState",
                        "$oplogQueryData",
                        "$queryOptions",
                        "$readPreference",
                        "$replData",
                        "$clusterTime",
                        "maxTimeMS",
                        "readConcern",
                        "shardVersion",
                        "tracking_info",
                        "writeConcern"};

    for (auto knownField : knownFields) {
        auto testTempDoc = BSON("DocSequenceCommand"
                                << "coll1"
                                << "field1"
                                << 3
                                << "field2"
                                << "five"
                                << "$db"
                                << "db"
                                << knownField
                                << "extra"
                                << "structs"
                                << BSON_ARRAY(BSON("value"
                                                   << "hello")
                                              << BSON("value"
                                                      << "world"))
                                << "objects"
                                << BSON_ARRAY(BSON("foo" << 1)));

        OpMsgRequest request;
        request.body = testTempDoc;
        auto testStruct = DocSequenceCommand::parse(ctxt, request);
        ASSERT_EQUALS(2UL, testStruct.getStructs().size());

        auto reply = testStruct.serialize(testTempDoc);
        assertOpMsgEquals(request, reply);
    }
}

// Postive: Extra Fields in non-strict parser
TEST(IDLDocSequence, TestNonStrict) {
    IDLParserErrorContext ctxt("root");

    // Positive: Extra field in document sequence
    {
        auto testTempDoc = BSON("DocSequenceCommandNonStrict"
                                << "coll1"
                                << "field1"
                                << 3
                                << "field2"
                                << "five");

        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"structs",
                                     {BSON("value"
                                           << "hello"),
                                      BSON("value"
                                           << "world")}});
        request.sequences.push_back({"objects", {BSON("foo" << 1)}});
        request.sequences.push_back({"extra", {BSON("foo" << 1)}});

        auto testStruct = DocSequenceCommandNonStrict::parse(ctxt, request);
        ASSERT_EQUALS(2UL, testStruct.getStructs().size());
    }

    // Positive: Extra field in body
    {
        auto testTempDoc = BSON("DocSequenceCommandNonStrict"
                                << "coll1"
                                << "field1"
                                << 3
                                << "field2"
                                << "five"
                                << "extra"
                                << 1);

        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"structs",
                                     {BSON("value"
                                           << "hello"),
                                      BSON("value"
                                           << "world")}});
        request.sequences.push_back({"objects", {BSON("foo" << 1)}});

        auto testStruct = DocSequenceCommandNonStrict::parse(ctxt, request);
        ASSERT_EQUALS(2UL, testStruct.getStructs().size());
    }
}

// Postive: Test a Command known field does not propagate from passthrough to the final BSON if it
// is included as a field in the command.
TEST(IDLCommand, TestKnownFieldDuplicate) {
    IDLParserErrorContext ctxt("root");

    auto testPassthrough = BSON("$db"
                                << "foo"
                                << "maxTimeMS"
                                << 6
                                << "$client"
                                << "foo");

    auto testDoc = BSON("KnownFieldCommand"
                        << "coll1"
                        << "$db"
                        << "db"
                        << "field1"
                        << 28
                        << "maxTimeMS"
                        << 42);

    auto testStruct = KnownFieldCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(28, testStruct.getField1());
    ASSERT_EQUALS(42, testStruct.getMaxTimeMS());

    // OpMsg request serializes original '$db' out because it is part of the OP_MSG request
    auto expectedOpMsgDoc = BSON("KnownFieldCommand"
                                 << "coll1"

                                 << "field1"
                                 << 28
                                 << "maxTimeMS"
                                 << 42
                                 << "$db"
                                 << "db"

                                 << "$client"
                                 << "foo");

    ASSERT_BSONOBJ_EQ(expectedOpMsgDoc, testStruct.serialize(testPassthrough).body);

    // BSON serialize does not round-trip '$db' because it can passed in passthrough data
    auto expectedBSONDoc = BSON("KnownFieldCommand"
                                << "coll1"

                                << "field1"
                                << 28
                                << "maxTimeMS"
                                << 42
                                << "$db"
                                << "foo"

                                << "$client"
                                << "foo");

    ASSERT_BSONOBJ_EQ(expectedBSONDoc, testStruct.toBSON(testPassthrough));
}


// Positive: Test an inline nested chain struct works
TEST(IDLChainedStruct, TestInline) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON("stringField"
                        << "bar"
                        << "field3"
                        << "foo");

    auto testStruct = Chained_struct_inline::parse(ctxt, testDoc);
    ASSERT_EQUALS(testStruct.getChained_string_inline_basic_type().getStringField(), "bar");
    ASSERT_EQUALS(testStruct.getField3(), "foo");

    assert_same_types<decltype(testStruct.getChained_string_inline_basic_type().getStringField()),
                      const StringData>();
    assert_same_types<decltype(testStruct.getField3()), const StringData>();

    // Positive: Test we can round trip to a document from the just parsed document
    {
        BSONObj loopbackDoc = testStruct.toBSON();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        Chained_struct_inline one_new;
        one_new.setField3("foo");

        Chained_string_inline_basic_type f1;
        f1.setStringField("bar");
        one_new.setChained_string_inline_basic_type(f1);

        BSONObj loopbackDoc = one_new.toBSON();

        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }
}

TEST(IDLValidatedField, Int_basic_ranges) {
    // Explicitly call setters.
    Int_basic_ranges obj0;
    obj0.setPositive_int(42);
    ASSERT_THROWS(obj0.setPositive_int(0), AssertionException);
    ASSERT_THROWS(obj0.setPositive_int(-42), AssertionException);

    ASSERT_THROWS(obj0.setNegative_int(42), AssertionException);
    ASSERT_THROWS(obj0.setNegative_int(0), AssertionException);
    obj0.setNegative_int(-42);

    obj0.setNon_negative_int(42);
    obj0.setNon_negative_int(0);
    ASSERT_THROWS(obj0.setNon_negative_int(-42), AssertionException);

    ASSERT_THROWS(obj0.setNon_positive_int(42), AssertionException);
    obj0.setNon_positive_int(0);
    obj0.setNon_positive_int(-42);

    ASSERT_THROWS(obj0.setByte_range_int(-1), AssertionException);
    obj0.setByte_range_int(0);
    obj0.setByte_range_int(127);
    obj0.setByte_range_int(128);
    obj0.setByte_range_int(255);
    ASSERT_THROWS(obj0.setByte_range_int(256), AssertionException);

    // IDL ints *are* int32_t, so no number we can pass to the func will actually fail.
    obj0.setRange_int(std::numeric_limits<std::int32_t>::min() + 1);
    obj0.setRange_int(-65536);
    obj0.setRange_int(0);
    obj0.setRange_int(65536);
    obj0.setRange_int(std::numeric_limits<std::int32_t>::max());

    // Positive case parsing.
    const auto tryPass = [](std::int32_t pos,
                            std::int32_t neg,
                            std::int32_t nonneg,
                            std::int32_t nonpos,
                            std::int32_t byte_range,
                            std::int32_t int_range) {
        IDLParserErrorContext ctxt("root");
        auto doc =
            BSON("positive_int" << pos << "negative_int" << neg << "non_negative_int" << nonneg
                                << "non_positive_int"
                                << nonpos
                                << "byte_range_int"
                                << byte_range
                                << "range_int"
                                << int_range);
        auto obj = Int_basic_ranges::parse(ctxt, doc);
        ASSERT_EQUALS(obj.getPositive_int(), pos);
        ASSERT_EQUALS(obj.getNegative_int(), neg);
        ASSERT_EQUALS(obj.getNon_negative_int(), nonneg);
        ASSERT_EQUALS(obj.getNon_positive_int(), nonpos);
        ASSERT_EQUALS(obj.getByte_range_int(), byte_range);
        ASSERT_EQUALS(obj.getRange_int(), int_range);
    };

    // Negative case parsing.
    const auto tryFail = [](std::int32_t pos,
                            std::int32_t neg,
                            std::int32_t nonneg,
                            std::int32_t nonpos,
                            std::int32_t byte_range,
                            std::int32_t int_range) {
        IDLParserErrorContext ctxt("root");
        auto doc =
            BSON("positive_int" << pos << "negative_int" << neg << "non_negative_int" << nonneg
                                << "non_positive_int"
                                << nonpos
                                << "byte_range_int"
                                << byte_range
                                << "range_int"
                                << int_range);
        ASSERT_THROWS(Int_basic_ranges::parse(ctxt, doc), AssertionException);
    };

    tryPass(1, -1, 0, 0, 128, 65537);
    tryFail(0, -1, 0, 0, 128, 65537);
    tryFail(1, 0, 0, 0, 128, 65537);
    tryFail(1, -1, -1, 0, 128, 65537);
    tryFail(1, -1, 0, 1, 128, 65537);
    tryFail(1, -1, 0, 0, 256, 65537);
    tryFail(0, 0, -1, 1, 257, 0);

    tryPass(1000, -1000, 1, -1, 127, 0x7FFFFFFF);
}

TEST(IDLValidatedField, Double_basic_ranges) {
    // Explicitly call setters.
    Double_basic_ranges obj0;
    obj0.setPositive_double(42.0);
    obj0.setPositive_double(0.000000000001);
    ASSERT_THROWS(obj0.setPositive_double(0.0), AssertionException);
    ASSERT_THROWS(obj0.setPositive_double(-42.0), AssertionException);

    ASSERT_THROWS(obj0.setNegative_double(42.0), AssertionException);
    ASSERT_THROWS(obj0.setNegative_double(0.0), AssertionException);
    obj0.setNegative_double(-0.000000000001);
    obj0.setNegative_double(-42.0);

    obj0.setNon_negative_double(42.0);
    obj0.setNon_negative_double(0.0);
    ASSERT_THROWS(obj0.setNon_negative_double(-42.0), AssertionException);

    ASSERT_THROWS(obj0.setNon_positive_double(42.0), AssertionException);
    obj0.setNon_positive_double(0.0);
    obj0.setNon_positive_double(-42.0);

    ASSERT_THROWS(obj0.setRange_double(-12345678901234600000.0), AssertionException);
    obj0.setRange_double(-12345678901234500000.0);
    obj0.setRange_double(-3000000000.0);
    obj0.setRange_double(0);
    obj0.setRange_double(3000000000);
    obj0.setRange_double(12345678901234500000.0);
    ASSERT_THROWS(obj0.setRange_double(12345678901234600000.0), AssertionException);

    // Positive case parsing.
    const auto tryPass =
        [](double pos, double neg, double nonneg, double nonpos, double double_range) {
            IDLParserErrorContext ctxt("root");
            auto doc =
                BSON("positive_double" << pos << "negative_double" << neg << "non_negative_double"
                                       << nonneg
                                       << "non_positive_double"
                                       << nonpos
                                       << "range_double"
                                       << double_range);
            auto obj = Double_basic_ranges::parse(ctxt, doc);
            ASSERT_EQUALS(obj.getPositive_double(), pos);
            ASSERT_EQUALS(obj.getNegative_double(), neg);
            ASSERT_EQUALS(obj.getNon_negative_double(), nonneg);
            ASSERT_EQUALS(obj.getNon_positive_double(), nonpos);
            ASSERT_EQUALS(obj.getRange_double(), double_range);
        };

    // Negative case parsing.
    const auto tryFail =
        [](double pos, double neg, double nonneg, double nonpos, double double_range) {
            IDLParserErrorContext ctxt("root");
            auto doc =
                BSON("positive_double" << pos << "negative_double" << neg << "non_negative_double"
                                       << nonneg
                                       << "non_positive_double"
                                       << nonpos
                                       << "range_double"
                                       << double_range);
            ASSERT_THROWS(Double_basic_ranges::parse(ctxt, doc), AssertionException);
        };

    tryPass(1, -1, 0, 0, 123456789012345);
    tryFail(0, -1, 0, 0, 123456789012345);
    tryFail(1, 0, 0, 0, 123456789012345);
    tryFail(1, -1, -1, 0, 123456789012345);
    tryFail(1, -1, 0, 1, 123456789012345);
    tryFail(1, -1, 0, -1, 12345678901234600000.0);
    tryPass(0.00000000001, -0.00000000001, 0.0, 0.0, 1.23456789012345);
}

TEST(IDLValidatedField, Callback_validators) {
    // Explicitly call setters.
    Callback_validators obj0;
    obj0.setInt_even(42);
    ASSERT_THROWS(obj0.setInt_even(7), AssertionException);
    obj0.setInt_even(0);
    ASSERT_THROWS(obj0.setInt_even(-7), AssertionException);
    obj0.setInt_even(-42);

    ASSERT_THROWS(obj0.setDouble_nearly_int(3.141592), AssertionException);
    ASSERT_THROWS(obj0.setDouble_nearly_int(-2.71828), AssertionException);
    obj0.setDouble_nearly_int(0.0);
    obj0.setDouble_nearly_int(1.0);
    obj0.setDouble_nearly_int(1.05);
    obj0.setDouble_nearly_int(-123456789.01234500000);

    ASSERT_THROWS(obj0.setString_starts_with_x("whiskey"), AssertionException);
    obj0.setString_starts_with_x("x-ray");
    ASSERT_THROWS(obj0.setString_starts_with_x("yankee"), AssertionException);

    // Positive case parsing.
    const auto tryPass =
        [](std::int32_t int_even, double double_nearly_int, StringData string_starts_with_x) {
            IDLParserErrorContext ctxt("root");
            auto doc = BSON("int_even" << int_even << "double_nearly_int" << double_nearly_int
                                       << "string_starts_with_x"
                                       << string_starts_with_x);
            auto obj = Callback_validators::parse(ctxt, doc);
            ASSERT_EQUALS(obj.getInt_even(), int_even);
            ASSERT_EQUALS(obj.getDouble_nearly_int(), double_nearly_int);
            ASSERT_EQUALS(obj.getString_starts_with_x(), string_starts_with_x);
        };

    // Negative case parsing.
    const auto tryFail =
        [](std::int32_t int_even, double double_nearly_int, StringData string_starts_with_x) {
            IDLParserErrorContext ctxt("root");
            auto doc = BSON("int_even" << int_even << "double_nearly_int" << double_nearly_int
                                       << "string_starts_with_x"
                                       << string_starts_with_x);
            ASSERT_THROWS(Callback_validators::parse(ctxt, doc), AssertionException);
        };

    tryPass(42, 123456789.01, "x-ray");
    tryFail(43, 123456789.01, "x-ray");
    tryFail(42, 123456789.11, "x-ray");
    tryFail(42, 123456789.01, "uniform");

    Unusual_callback_validators obj1;
    obj1.setInt_even(42);
    ASSERT_THROWS(obj1.setInt_even(7), AssertionException);
    obj1.setArray_of_int({42});
    ASSERT_THROWS(obj1.setArray_of_int({7}), AssertionException);
    obj1.setOne_int(One_int(42));
    ASSERT_THROWS(obj1.setOne_int(One_int(7)), AssertionException);
}

// Positive: verify a command a string arg
TEST(IDLTypeCommand, TestString) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON(CommandTypeStringCommand::kCommandName << "foo"
                                                               << "field1"
                                                               << 3
                                                               << "$db"
                                                               << "db");

    auto testStruct = CommandTypeStringCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getCommandParameter(), "foo");

    assert_same_types<decltype(testStruct.getCommandParameter()), const StringData>();

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        OpMsgRequest reply = testStruct.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }

    // Positive: Test we can serialize from nothing the same document except for $db
    {
        auto testDocWithoutDb = BSON(CommandTypeStringCommand::kCommandName << "foo"
                                                                            << "field1"
                                                                            << 3);

        BSONObjBuilder builder;
        CommandTypeStringCommand one_new("foo");
        one_new.setField1(3);
        one_new.setDbName("db");
        one_new.serialize(BSONObj(), &builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDocWithoutDb, serializedDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        CommandTypeStringCommand one_new("foo");
        one_new.setField1(3);
        one_new.setDbName("db");
        OpMsgRequest reply = one_new.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }
}

// Positive: verify a command can take an array of object
TEST(IDLTypeCommand, TestArrayObject) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON(CommandTypeArrayObjectCommand::kCommandName << BSON_ARRAY(BSON("sample"
                                                                                       << "doc"))
                                                                    << "$db"
                                                                    << "db");

    auto testStruct = CommandTypeArrayObjectCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getCommandParameter().size(), 1UL);

    assert_same_types<decltype(testStruct.getCommandParameter()),
                      const std::vector<mongo::BSONObj>&>();

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        OpMsgRequest reply = testStruct.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        std::vector<BSONObj> vec;
        vec.emplace_back(BSON("sample"
                              << "doc"));
        CommandTypeArrayObjectCommand one_new(vec);
        one_new.setDbName("db");
        OpMsgRequest reply = one_new.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }
}

// Positive: verify a command can take a struct
TEST(IDLTypeCommand, TestStruct) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON(CommandTypeStructCommand::kCommandName << BSON("value"
                                                                       << "sample")
                                                               << "$db"
                                                               << "db");

    auto testStruct = CommandTypeStructCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getCommandParameter().getValue(), "sample");

    assert_same_types<decltype(testStruct.getCommandParameter()),
                      mongo::idl::import::One_string&>();

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        OpMsgRequest reply = testStruct.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        One_string os;
        os.setValue("sample");
        CommandTypeStructCommand one_new(os);
        one_new.setDbName("db");
        OpMsgRequest reply = one_new.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }
}

// Positive: verify a command can take an array of structs
TEST(IDLTypeCommand, TestStructArray) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON(CommandTypeArrayStructCommand::kCommandName << BSON_ARRAY(BSON("value"
                                                                                       << "sample"))
                                                                    << "$db"
                                                                    << "db");

    auto testStruct = CommandTypeArrayStructCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getCommandParameter().size(), 1UL);

    assert_same_types<decltype(testStruct.getCommandParameter()),
                      const std::vector<mongo::idl::import::One_string>&>();

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        OpMsgRequest reply = testStruct.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        std::vector<One_string> vec;
        One_string os;
        os.setValue("sample");
        vec.push_back(os);
        CommandTypeArrayStructCommand one_new(vec);
        one_new.setDbName("db");
        OpMsgRequest reply = one_new.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }
}

// Positive: verify a command a string arg and alternate C++ name
TEST(IDLTypeCommand, TestUnderscoreCommand) {
    IDLParserErrorContext ctxt("root");

    auto testDoc = BSON(WellNamedCommand::kCommandName << "foo"
                                                       << "field1"
                                                       << 3
                                                       << "$db"
                                                       << "db");

    auto testStruct = WellNamedCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getCommandParameter(), "foo");

    assert_same_types<decltype(testStruct.getCommandParameter()), const StringData>();

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        OpMsgRequest reply = testStruct.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }

    // Positive: Test we can serialize from nothing the same document except for $db
    {
        auto testDocWithoutDb = BSON(WellNamedCommand::kCommandName << "foo"
                                                                    << "field1"
                                                                    << 3);

        BSONObjBuilder builder;
        WellNamedCommand one_new("foo");
        one_new.setField1(3);
        one_new.setDbName("db");
        one_new.serialize(BSONObj(), &builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDocWithoutDb, serializedDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        WellNamedCommand one_new("foo");
        one_new.setField1(3);
        one_new.setDbName("db");
        OpMsgRequest reply = one_new.serialize(BSONObj());

        ASSERT_BSONOBJ_EQ(testDoc, reply.body);
    }
}

}  // namespace
}  // namespace mongo
