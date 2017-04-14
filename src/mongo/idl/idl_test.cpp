/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/unittest_gen.h"
#include "mongo/unittest/unittest.h"

using namespace mongo::idl::test;

namespace mongo {

// Use a seperate function to get better error messages when types do not match.
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

    // Positive: Test we can serialize from nothing the same document
    {
        BSONObjBuilder builder;
        ParserT one_new;
        one_new.setValue(test_value);
        one_new.serialize(&builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, serializedDoc);
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
    }
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
        ASSERT_THROWS(ParserT::parse(ctxt, testDoc), UserException);
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

    auto testDoc = BSON("value"
                        << "foo.bar");

    auto element = testDoc.firstElement();
    ASSERT_EQUALS(element.type(), String);

    auto testStruct = One_namespacestring::parse(ctxt, testDoc);
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

        ASSERT_THROWS(One_namespacestring::parse(ctxt, testBadDoc), UserException);
    }
}

// Postive: Test any type
TEST(IDLOneTypeTests, TestAnyType) {
    IDLParserErrorContext ctxt("root");

    // Positive: string field
    {
        auto testDoc = BSON("value"
                            << "Foo");
        One_any_basic_type::parse(ctxt, testDoc);
    }

    // Positive: int field
    {
        auto testDoc = BSON("value" << 12);
        One_any_basic_type::parse(ctxt, testDoc);
    }
}

// Postive: Test object type
TEST(IDLOneTypeTests, TestObjectType) {
    IDLParserErrorContext ctxt("root");

    // Positive: object
    {
        auto testDoc = BSON("value" << BSON("value"
                                            << "foo"));
        One_any_basic_type::parse(ctxt, testDoc);
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
        ASSERT_THROWS(RequiredStrictField3::parse(ctxt, testDoc), UserException);
    }
    {
        auto testDoc = BSON("field1" << 12 << "field3" << 1234);
        ASSERT_THROWS(RequiredStrictField3::parse(ctxt, testDoc), UserException);
    }
    {
        auto testDoc = BSON("field1" << 12 << "field2" << 123);
        ASSERT_THROWS(RequiredStrictField3::parse(ctxt, testDoc), UserException);
    }

    // Negative: Extra field
    {
        auto testDoc =
            BSON("field1" << 12 << "field2" << 123 << "field3" << 1234 << "field4" << 1234);
        ASSERT_THROWS(RequiredStrictField3::parse(ctxt, testDoc), UserException);
    }

    // Negative: Duplicate field
    {
        auto testDoc =
            BSON("field1" << 12 << "field2" << 123 << "field3" << 1234 << "field2" << 12345);
        ASSERT_THROWS(RequiredStrictField3::parse(ctxt, testDoc), UserException);
    }
}
// Positive: non-strict, ensure extra fields work
// Negative: non-strict, duplicate fields
TEST(IDLStructTests, TestNonStrictStruct) {
    IDLParserErrorContext ctxt("root");

    // Positive: Just 3 required fields
    {
        auto testDoc = BSON("1" << 12 << "2" << 123 << "3" << 1234);
        auto testStruct = RequiredNonStrictField3::parse(ctxt, testDoc);

        assert_same_types<decltype(testStruct.getField1()), std::int32_t>();
        assert_same_types<decltype(testStruct.getField2()), std::int32_t>();
        assert_same_types<decltype(testStruct.getField3()), std::int32_t>();
    }

    // Negative: Missing 1 required field
    {
        auto testDoc = BSON("2" << 123 << "3" << 1234);
        ASSERT_THROWS(RequiredNonStrictField3::parse(ctxt, testDoc), UserException);
    }
    {
        auto testDoc = BSON("1" << 12 << "3" << 1234);
        ASSERT_THROWS(RequiredNonStrictField3::parse(ctxt, testDoc), UserException);
    }
    {
        auto testDoc = BSON("1" << 12 << "2" << 123);
        ASSERT_THROWS(RequiredNonStrictField3::parse(ctxt, testDoc), UserException);
    }

    // Positive: Extra field
    {
        auto testDoc = BSON("1" << 12 << "2" << 123 << "3" << 1234 << "field4" << 1234);
        RequiredNonStrictField3::parse(ctxt, testDoc);
    }

    // Negative: Duplicate field
    {
        auto testDoc = BSON("1" << 12 << "2" << 123 << "3" << 1234 << "2" << 12345);
        ASSERT_THROWS(RequiredNonStrictField3::parse(ctxt, testDoc), UserException);
    }

    // Negative: Duplicate extra field
    {
        auto testDoc =
            BSON("field4" << 1234 << "1" << 12 << "2" << 123 << "3" << 1234 << "field4" << 1234);
        ASSERT_THROWS(RequiredNonStrictField3::parse(ctxt, testDoc), UserException);
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
                          const boost::optional<mongo::BSONObj>>();

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


// Positive: Test a nested struct
TEST(IDLNestedStruct, TestDuplicatTypes) {
    IDLParserErrorContext ctxt("root");


    // Positive: Test document
    auto testDoc = BSON(

        "field1" << BSON("field1" << 1 << "field2" << 2 << "field3" << 3) <<

        "field3" << BSON("field1" << 4 << "field2" << 5 << "field3" << 6));
    auto testStruct = NestedWithDuplicateTypes::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()), const RequiredStrictField3&>();
    assert_same_types<decltype(testStruct.getField2()),
                      const boost::optional<RequiredNonStrictField3>>();
    assert_same_types<decltype(testStruct.getField3()), const RequiredStrictField3&>();

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
    auto testDoc = BSON("field1" << BSON_ARRAY("Foo"
                                               << "Bar"
                                               << "???")
                                 << "field2"
                                 << BSON_ARRAY(1 << 2 << 3)
                                 << "field3"
                                 << BSON_ARRAY(1.2 << 3.4 << 5.6)

                            );
    auto testStruct = Simple_array_fields::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()), const std::vector<mongo::StringData>>();
    assert_same_types<decltype(testStruct.getField2()), const std::vector<std::int32_t>&>();
    assert_same_types<decltype(testStruct.getField3()), const std::vector<double>&>();

    std::vector<StringData> field1{"Foo", "Bar", "???"};
    ASSERT_TRUE(field1 == testStruct.getField1());
    std::vector<std::int32_t> field2{1, 2, 3};
    ASSERT_TRUE(field2 == testStruct.getField2());
    std::vector<double> field3{1.2, 3.4, 5.6};
    ASSERT_TRUE(field3 == testStruct.getField3());

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
                      const boost::optional<std::vector<std::int32_t>>>();
    assert_same_types<decltype(testStruct.getField3()),
                      const boost::optional<std::vector<double>>>();

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

        ASSERT_THROWS(Simple_int_array::parse(ctxt, testDoc), UserException);
    }

    // Negative: Test array with mixed types
    {
        auto testDoc = BSON("field1" << BSON_ARRAY(1.2 << 3.4 << 5.6));

        ASSERT_THROWS(Simple_int_array::parse(ctxt, testDoc), UserException);
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

        ASSERT_THROWS(Simple_int_array::parse(ctxt, testDoc), UserException);
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

        ASSERT_THROWS(Simple_int_array::parse(ctxt, testDoc), UserException);
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

        ASSERT_THROWS(Simple_int_array::parse(ctxt, testDoc), UserException);
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
                                 << "field1o"
                                 << BSON_ARRAY(1 << 2 << 3)
                                 << "field2o"
                                 << BSON_ARRAY("a.b"
                                               << "c.d")
                                 << "field3o"
                                 << BSON_ARRAY(1 << "2")
                                 << "field4o"
                                 << BSON_ARRAY(BSONObj() << BSONObj()));
    auto testStruct = Complex_array_fields::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()), const std::vector<std::int64_t>&>();
    assert_same_types<decltype(testStruct.getField2()),
                      const std::vector<mongo::NamespaceString>&>();
    assert_same_types<decltype(testStruct.getField3()), const std::vector<mongo::AnyBasicType>&>();
    assert_same_types<decltype(testStruct.getField4()),
                      const std::vector<mongo::ObjectBasicType>&>();
    assert_same_types<decltype(testStruct.getField5()), const std::vector<mongo::BSONObj>&>();

    assert_same_types<decltype(testStruct.getField1o()),
                      const boost::optional<std::vector<std::int64_t>>>();
    assert_same_types<decltype(testStruct.getField2o()),
                      const boost::optional<std::vector<mongo::NamespaceString>>>();
    assert_same_types<decltype(testStruct.getField3o()),
                      const boost::optional<std::vector<mongo::AnyBasicType>>>();
    assert_same_types<decltype(testStruct.getField4o()),
                      const boost::optional<std::vector<mongo::ObjectBasicType>>>();
    assert_same_types<decltype(testStruct.getField5o()),
                      const boost::optional<std::vector<mongo::BSONObj>>>();

    std::vector<std::int64_t> field1{1, 2, 3};
    ASSERT_TRUE(field1 == testStruct.getField1());
    std::vector<NamespaceString> field2{{"a", "b"}, {"c", "d"}};
    ASSERT_TRUE(field2 == testStruct.getField2());
}

/**
 * A simple class that derives from an IDL generated class
 */
class ClassDerivedFromStruct : public DerivedBaseStruct {
public:
    static ClassDerivedFromStruct parse(const IDLParserErrorContext& ctxt,
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

    auto testStruct = ClassDerivedFromStruct::parse(ctxt, testDoc);
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

}  // namespace mongo
