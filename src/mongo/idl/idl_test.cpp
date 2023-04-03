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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/authorization_contract.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/write_concern_options_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/idl/unittest_gen.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

using namespace mongo::idl::test;
using namespace mongo::idl::import;

namespace mongo {

void mongo::idl::test::checkValuesEqual(StructWithValidator* structToValidate) {
    uassert(
        6253512, "Values not equal", structToValidate->getFirst() == structToValidate->getSecond());
}

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

template <typename T>
BSONObj serializeCmd(const T& cmd) {
    auto reply = cmd.serialize({});
    return reply.body;
}

// Use a separate function to get better error messages when types do not match.
template <typename T1, typename T2>
void assert_same_types() {
    MONGO_STATIC_ASSERT(std::is_same<T1, T2>::value);
}

template <typename ParserT, typename TestT, BSONType Test_bson_type>
void TestLoopback(TestT test_value) {
    auto testDoc = BSON("value" << test_value);
    auto element = testDoc.firstElement();
    ASSERT_EQUALS(element.type(), Test_bson_type);

    auto testStruct = ParserT::parse(IDLParserContext{"test"}, testDoc);
    assert_same_types<decltype(testStruct.getValue()), TestT>();

    // We need to use a different unittest macro for comparing obj/array.
    constexpr bool isObjectTest = std::is_same_v<TestT, const BSONObj&>;
    constexpr bool isArrayTest = std::is_same_v<TestT, const BSONArray&>;
    if constexpr (isObjectTest || isArrayTest) {
        ASSERT_BSONOBJ_EQ(testStruct.getValue(), test_value);
    } else {
        ASSERT_EQUALS(testStruct.getValue(), test_value);
    }

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
        if constexpr (!isArrayTest) {
            // BSONArray comparison not currently implemented.
            ASSERT_TRUE(one_new == testStruct);
        }

        if constexpr (isObjectTest) {
            // Only One_plain_object implements comparison ops
            ASSERT_FALSE(one_new < testStruct);
        }
    }
}

/// Type tests:
// Positive: Test we can serialize the type out and back again
TEST(IDLOneTypeTests, TestLoopbackTest) {
    TestLoopback<One_string, StringData, String>("test_value");
    TestLoopback<One_int, std::int32_t, NumberInt>(123);
    TestLoopback<One_long, std::int64_t, NumberLong>(456);
    TestLoopback<One_double, double, NumberDouble>(3.14159);
    TestLoopback<One_bool, bool, Bool>(true);
    TestLoopback<One_objectid, const OID&, jstOID>(OID::max());
    TestLoopback<One_date, const Date_t&, Date>(Date_t::now());
    TestLoopback<One_timestamp, const Timestamp&, bsonTimestamp>(Timestamp::max());
    TestLoopback<One_plain_object, const BSONObj&, Object>(BSON("Hello"
                                                                << "World"));
    TestLoopback<One_plain_array, const BSONArray&, Array>(BSON_ARRAY("Hello"
                                                                      << "World"));
}

// Test we compare an object with optional BSONObjs correctly
TEST(IDLOneTypeTests, TestOptionalObjectTest) {
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

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
TEST(IDLOneTypeTests, TestSafeInt64) {
    TestParse<One_safeint64, NumberInt, StringData, String>("test_value");
    TestParse<One_safeint64, NumberInt, std::int32_t, NumberInt>(123);
    TestParse<One_safeint64, NumberLong, std::int64_t, NumberLong>(456);
    TestParse<One_safeint64, NumberDouble, double, NumberDouble>(3.14159);
    TestParse<One_safeint64, NumberInt, bool, Bool>(true);
    TestParse<One_safeint64, NumberInt, OID, jstOID>(OID::max());
    TestParse<One_safeint64, NumberInt, Date_t, Date>(Date_t::now());
    TestParse<One_safeint64, NumberInt, Timestamp, bsonTimestamp>(Timestamp::max());
}

// Mixed: test a type that accepts NamespaceString
TEST(IDLOneTypeTests, TestNamespaceString) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON(One_namespacestring::kValueFieldName << "foo.bar");

    auto element = testDoc.firstElement();
    ASSERT_EQUALS(element.type(), String);

    auto testStruct = One_namespacestring::parse(ctxt, testDoc);
    assert_same_types<decltype(testStruct.getValue()), const NamespaceString&>();

    ASSERT_EQUALS(testStruct.getValue(), NamespaceString::createNamespaceString_forTest("foo.bar"));

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
        one_new.setValue(NamespaceString::createNamespaceString_forTest("foo.bar"));
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

// Positive: Test base64 encoded strings.
TEST(IDLOneTypeTests, TestBase64StringPositive) {
    auto doc = BSON("basic"
                    << "ABCD+/0="
                    << "url"
                    << "1234-_0");
    auto parsed = Two_base64string::parse(IDLParserContext{"base64"}, doc);
    ASSERT_EQ(parsed.getBasic(), "\x00\x10\x83\xFB\xFD"_sd);
    ASSERT_EQ(parsed.getUrl(), "\xD7m\xF8\xFB\xFD"_sd);

    BSONObjBuilder builder;
    parsed.serialize(&builder);
    ASSERT_BSONOBJ_EQ(doc, builder.obj());
}

// Negative: Test base64 encoded strings.
TEST(IDLOneTypeTests, TestBase64StringNegative) {
    {
        // No terminator on basic.
        auto doc = BSON("basic"
                        << "ABCD+/0"
                        << "url"
                        << "1234-_0");
        ASSERT_THROWS_CODE_AND_WHAT(Two_base64string::parse(IDLParserContext{"base64"}, doc),
                                    AssertionException,
                                    10270,
                                    "invalid base64");
    }

    {
        // Invalid chars in basic.
        auto doc = BSON("basic"
                        << "ABCD+_0="
                        << "url"
                        << "1234-_0");
        ASSERT_THROWS_CODE_AND_WHAT(Two_base64string::parse(IDLParserContext{"base64"}, doc),
                                    AssertionException,
                                    40537,
                                    "Invalid base64 character");
    }

    {
        // Invalid chars in url
        auto doc = BSON("basic"
                        << "ABCD+/0="
                        << "url"
                        << "1234-/0");
        ASSERT_THROWS_CODE_AND_WHAT(Two_base64string::parse(IDLParserContext{"base64"}, doc),
                                    AssertionException,
                                    40537,
                                    "Invalid base64 character");
    }
}


// BSONElement::exactNumberLong() provides different errors on windows
#ifdef _WIN32
constexpr auto kNANRepr = "-nan(ind)"_sd;
#else
constexpr auto kNANRepr = "nan"_sd;
#endif

TEST(IDLStructTests, DurationParse) {
    IDLParserContext ctxt("duration");

    auto justAMinuteDoc = BSON("secs" << 60);
    auto justAMinute = Struct_with_durations::parse(ctxt, justAMinuteDoc);
    ASSERT_EQ(justAMinute.getSecs().get(), Seconds{60});
    ASSERT_EQ(justAMinute.getSecs().get(), Minutes{1});

    auto floatDurationDoc = BSON("secs" << 123.0);
    auto floatDuration = Struct_with_durations::parse(ctxt, floatDurationDoc);
    ASSERT_EQ(floatDuration.getSecs().get(), Seconds{123});

    auto halfSecondDoc = BSON("secs" << 234.5);
    ASSERT_THROWS_CODE_AND_WHAT(Struct_with_durations::parse(ctxt, halfSecondDoc),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "Expected an integer: secs: 234.5");

    auto invalidDurationDoc = BSON("secs"
                                   << "bob");
    ASSERT_THROWS_CODE_AND_WHAT(Struct_with_durations::parse(ctxt, invalidDurationDoc),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "Duration value must be numeric, got: string");

    auto notADurationDoc = BSON("secs" << NAN);
    ASSERT_THROWS_CODE_AND_WHAT(
        Struct_with_durations::parse(ctxt, notADurationDoc),
        AssertionException,
        ErrorCodes::FailedToParse,
        fmt::format("Expected an integer, but found NaN in: secs: {}.0", kNANRepr));

    auto endOfTimeDoc = BSON("secs" << std::numeric_limits<double>::infinity());
    ASSERT_THROWS_CODE_AND_WHAT(Struct_with_durations::parse(ctxt, endOfTimeDoc),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "Cannot represent as a 64-bit integer: secs: inf.0");
}

TEST(IDLStructTests, DurationSerialize) {
    Struct_with_durations allDay;
    allDay.setSecs(boost::make_optional(duration_cast<Seconds>(Days{1})));

    BSONObjBuilder builder;
    allDay.serialize(&builder);
    auto obj = builder.obj();

    auto intervalElem = obj["secs"_sd];
    ASSERT_EQ(intervalElem.numberLong(), 86400);
}

TEST(IDLStructTests, EpochsParse) {
    IDLParserContext ctxt("epoch");

    auto sameTimeDoc = BSON("unix" << 1234567890LL << "ecma" << 1234567890000LL);
    auto sameTime = Struct_with_epochs::parse(ctxt, sameTimeDoc);
    ASSERT_EQ(sameTime.getUnix(), sameTime.getEcma());
    ASSERT_EQ(sameTime.getUnix().toDurationSinceEpoch(), Seconds{1234567890});
    ASSERT_EQ(sameTime.getEcma().toDurationSinceEpoch(), Seconds{1234567890});

    auto floatTimeDoc = BSON("unix" << 123.0 << "ecma" << 234000.0);
    auto floatTime = Struct_with_epochs::parse(ctxt, floatTimeDoc);
    ASSERT_EQ(floatTime.getUnix().toDurationSinceEpoch(), Seconds{123});
    ASSERT_EQ(floatTime.getEcma().toDurationSinceEpoch(), Seconds{234});

    auto halfTimeDoc = BSON("unix" << 345.6 << "ecma" << 0);
    ASSERT_THROWS_CODE_AND_WHAT(Struct_with_epochs::parse(ctxt, halfTimeDoc),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "Expected an integer: unix: 345.6");

    auto invalidTimeDoc = BSON("unix"
                               << "bob"
                               << "ecma" << 1234567890000LL);
    ASSERT_THROWS_CODE_AND_WHAT(Struct_with_epochs::parse(ctxt, invalidTimeDoc),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "Epoch value must be numeric, got: string");

    auto notATimeDoc = BSON("unix" << 0 << "ecma" << NAN);
    ASSERT_THROWS_CODE_AND_WHAT(
        Struct_with_epochs::parse(ctxt, notATimeDoc),
        AssertionException,
        ErrorCodes::FailedToParse,
        fmt::format("Expected an integer, but found NaN in: ecma: {}.0", kNANRepr));

    auto endOfTimeDoc = BSON("unix" << std::numeric_limits<double>::infinity() << "ecma" << 0);
    ASSERT_THROWS_CODE_AND_WHAT(Struct_with_epochs::parse(ctxt, endOfTimeDoc),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "Cannot represent as a 64-bit integer: unix: inf.0");
}

TEST(IDLStructTests, EpochSerialize) {
    Struct_with_epochs writeVal;
    writeVal.setUnix(Date_t::fromDurationSinceEpoch(Days{1}));
    writeVal.setEcma(Date_t::fromDurationSinceEpoch(Days{2}));
    BSONObjBuilder builder;
    writeVal.serialize(&builder);
    auto obj = builder.obj();

    auto unixElem = obj["unix"];
    auto ecmaElem = obj["ecma"];
    ASSERT_EQ(unixElem.type(), NumberLong);
    ASSERT_EQ(unixElem.numberLong(), 86400);
    ASSERT_EQ(ecmaElem.type(), NumberLong);
    ASSERT_EQ(ecmaElem.numberLong(), 2 * 86400 * 1000);
}

// Postive: Test any type
TEST(IDLOneTypeTests, TestAnyType) {
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

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

// Trait check used in TestLoopbackVariant.
template <typename T>
struct IsVector : std::false_type {};
template <typename T>
struct IsVector<std::vector<T>> : std::true_type {};
template <typename T>
constexpr bool isVector = IsVector<T>::value;

// We don't generate comparison operators like "==" for variants, so test only for BSON equality.
template <typename ParserT, typename TestT, BSONType Test_bson_type>
void TestLoopbackVariant(TestT test_value) {
    IDLParserContext ctxt("root");

    BSONObjBuilder bob;
    if constexpr (idl::hasBSONSerialize<TestT>) {
        // TestT might be an IDL struct type like One_string.
        BSONObjBuilder subObj(bob.subobjStart("value"));
        test_value.serialize(&subObj);
    } else if constexpr (isVector<TestT>) {
        BSONArrayBuilder arrayBuilder(bob.subarrayStart("value"));
        for (const auto& item : test_value) {
            if constexpr (idl::hasBSONSerialize<decltype(item)>) {
                BSONObjBuilder subObjBuilder(arrayBuilder.subobjStart());
                item.serialize(&subObjBuilder);
            } else {
                arrayBuilder.append(item);
            }
        }
    } else if constexpr (std::is_same_v<TestT, UUID>) {
        test_value.appendToBuilder(&bob, "value");
    } else {
        bob.append("value", test_value);
    }

    auto obj = bob.obj();
    auto element = obj.firstElement();
    ASSERT_EQUALS(element.type(), Test_bson_type);

    auto parsed = ParserT::parse(ctxt, obj);
    if constexpr (std::is_same_v<TestT, BSONObj>) {
        ASSERT_BSONOBJ_EQ(stdx::get<TestT>(parsed.getValue()), test_value);
    } else {
        // Use ASSERT instead of ASSERT_EQ to avoid operator<<
        ASSERT(stdx::get<TestT>(parsed.getValue()) == test_value);
    }
    ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());

    // Test setValue.
    ParserT assembled;
    assembled.setValue(test_value);
    ASSERT_BSONOBJ_EQ(obj, assembled.toBSON());

    // Test the constructor.
    ParserT constructed(test_value);
    if constexpr (std::is_same_v<TestT, BSONObj>) {
        ASSERT_BSONOBJ_EQ(stdx::get<TestT>(parsed.getValue()), test_value);
    } else {
        ASSERT(stdx::get<TestT>(parsed.getValue()) == test_value);
    }
    ASSERT_BSONOBJ_EQ(obj, constructed.toBSON());
}

TEST(IDLVariantTests, TestVariantRoundtrip) {
    TestLoopbackVariant<One_variant, int, NumberInt>(1);
    TestLoopbackVariant<One_variant, std::string, String>("test_value");

    TestLoopbackVariant<One_variant_uuid, int, NumberInt>(1);
    TestLoopbackVariant<One_variant_uuid, UUID, BinData>(UUID::gen());

    TestLoopbackVariant<One_variant_compound, std::string, String>("test_value");
    TestLoopbackVariant<One_variant_compound, BSONObj, Object>(BSON("x" << 1));
    TestLoopbackVariant<One_variant_compound, std::vector<std::string>, Array>({});
    TestLoopbackVariant<One_variant_compound, std::vector<std::string>, Array>({"a"});
    TestLoopbackVariant<One_variant_compound, std::vector<std::string>, Array>({"a", "b"});

    TestLoopbackVariant<One_variant_struct, int, NumberInt>(1);
    TestLoopbackVariant<One_variant_struct, One_string, Object>(One_string("test_value"));

    TestLoopbackVariant<One_variant_struct_array, int, NumberInt>(1);
    TestLoopbackVariant<One_variant_struct_array, std::vector<One_string>, Array>(
        std::vector<One_string>());
    TestLoopbackVariant<One_variant_struct_array, std::vector<One_string>, Array>(
        {One_string("a")});
    TestLoopbackVariant<One_variant_struct_array, std::vector<One_string>, Array>(
        {One_string("a"), One_string("b")});
}

TEST(IDLVariantTests, TestVariantSafeInt) {
    TestLoopbackVariant<One_variant_safeInt, std::string, String>("test_value");
    TestLoopbackVariant<One_variant_safeInt, int, NumberInt>(1);

    // safeInt accepts all numbers, but always deserializes and serializes as int32.
    IDLParserContext ctxt("root");
    ASSERT_EQ(stdx::get<std::int32_t>(
                  One_variant_safeInt::parse(ctxt, BSON("value" << Decimal128(1))).getValue()),
              1);
    ASSERT_EQ(
        stdx::get<std::int32_t>(One_variant_safeInt::parse(ctxt, BSON("value" << 1LL)).getValue()),
        1);
    ASSERT_EQ(
        stdx::get<std::int32_t>(One_variant_safeInt::parse(ctxt, BSON("value" << 1.0)).getValue()),
        1);
}

TEST(IDLVariantTests, TestVariantSafeIntArray) {
    using int32vec = std::vector<std::int32_t>;

    TestLoopbackVariant<One_variant_safeInt_array, std::string, String>("test_value");
    TestLoopbackVariant<One_variant_safeInt_array, int32vec, Array>({});
    TestLoopbackVariant<One_variant_safeInt_array, int32vec, Array>({1});
    TestLoopbackVariant<One_variant_safeInt_array, int32vec, Array>({1, 2});

    // Use ASSERT instead of ASSERT_EQ to avoid operator<<
    IDLParserContext ctxt("root");
    ASSERT(stdx::get<int32vec>(
               One_variant_safeInt_array::parse(ctxt, BSON("value" << BSON_ARRAY(Decimal128(1))))
                   .getValue()) == int32vec{1});
    ASSERT(
        stdx::get<int32vec>(
            One_variant_safeInt_array::parse(ctxt, BSON("value" << BSON_ARRAY(1LL))).getValue()) ==
        int32vec{1});
    ASSERT(
        stdx::get<int32vec>(
            One_variant_safeInt_array::parse(ctxt, BSON("value" << BSON_ARRAY(1.0))).getValue()) ==
        int32vec{1});
    ASSERT(
        stdx::get<int32vec>(One_variant_safeInt_array::parse(
                                ctxt, BSON("value" << BSON_ARRAY(1.0 << 2LL << 3 << Decimal128(4))))
                                .getValue()) == (int32vec{1, 2, 3, 4}));
}

TEST(IDLVariantTests, TestVariantTwoStructs) {
    auto obj = BSON("value" << BSON("insert" << 1));
    auto parsed = One_variant_two_structs::parse(IDLParserContext{"root"}, obj);
    ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());
    ASSERT_EQ(stdx::get<Insert_variant_struct>(parsed.getValue()).getInsert(), 1);

    obj = BSON("value" << BSON("update"
                               << "foo"));
    parsed = One_variant_two_structs::parse(IDLParserContext{"root"}, obj);
    ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());
    ASSERT_EQ(stdx::get<Update_variant_struct>(parsed.getValue()).getUpdate(), "foo");
}

TEST(IDLVariantTests, TestVariantTwoArrays) {
    TestLoopbackVariant<One_variant_two_arrays, std::vector<int>, Array>({});
    TestLoopbackVariant<One_variant_two_arrays, std::vector<int>, Array>({1});
    TestLoopbackVariant<One_variant_two_arrays, std::vector<int>, Array>({1, 2});
    TestLoopbackVariant<One_variant_two_arrays, std::vector<std::string>, Array>({"a"});
    TestLoopbackVariant<One_variant_two_arrays, std::vector<std::string>, Array>({"a", "b"});

    // This variant can be array<int> or array<string>. It assumes an empty array is array<int>
    // because that type is declared first in the IDL.
    auto obj = BSON("value" << BSONArray());
    auto parsed = One_variant_two_arrays::parse(IDLParserContext{"root"}, obj);
    ASSERT(stdx::get<std::vector<int>>(parsed.getValue()) == std::vector<int>());
    ASSERT_THROWS(stdx::get<std::vector<std::string>>(parsed.getValue()), stdx::bad_variant_access);

    // Corrupt array: its first key isn't "0".
    BSONObjBuilder bob;
    {
        BSONObjBuilder arrayBob(bob.subarrayStart("value"));
        arrayBob.append("1", "test_value");
    }

    ASSERT_THROWS_CODE(One_variant_two_arrays::parse(IDLParserContext{"root"}, bob.obj()),
                       AssertionException,
                       40423);
}

TEST(IDLVariantTests, TestVariantOptional) {
    {
        auto obj = BSON("value" << 1);
        auto parsed = One_variant_optional::parse(IDLParserContext{"root"}, obj);
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());
        ASSERT_EQ(stdx::get<int>(*parsed.getValue()), 1);
    }

    {
        auto obj = BSON("value"
                        << "test_value");
        auto parsed = One_variant_optional::parse(IDLParserContext{"root"}, obj);
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());
        ASSERT_EQ(stdx::get<std::string>(*parsed.getValue()), "test_value");
    }

    // The optional key is absent.
    auto parsed = One_variant_optional::parse(IDLParserContext{"root"}, BSONObj());
    ASSERT_FALSE(parsed.getValue().has_value());
    ASSERT_BSONOBJ_EQ(BSONObj(), parsed.toBSON());
}

TEST(IDLVariantTests, TestTwoVariants) {
    // Combinations of value0 (int or string) and value1 (object or array<string>). For each, test
    // parse(), toBSON(), getValue0(), getValue1(), and the constructor.
    {
        auto obj = BSON("value0" << 1 << "value1" << BSONObj());
        auto parsed = Two_variants::parse(IDLParserContext{"root"}, obj);
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());
        ASSERT_EQ(stdx::get<int>(parsed.getValue0()), 1);
        ASSERT_BSONOBJ_EQ(stdx::get<BSONObj>(parsed.getValue1()), BSONObj());
        ASSERT_BSONOBJ_EQ(Two_variants(1, BSONObj()).toBSON(), obj);
    }

    {
        auto obj = BSON("value0"
                        << "test_value"
                        << "value1" << BSONObj());
        auto parsed = Two_variants::parse(IDLParserContext{"root"}, obj);
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());
        ASSERT_EQ(stdx::get<std::string>(parsed.getValue0()), "test_value");
        ASSERT_BSONOBJ_EQ(stdx::get<BSONObj>(parsed.getValue1()), BSONObj());
        ASSERT_BSONOBJ_EQ(Two_variants("test_value", BSONObj()).toBSON(), obj);
    }

    {
        auto obj = BSON("value0" << 1 << "value1"
                                 << BSON_ARRAY("x"
                                               << "y"));
        auto parsed = Two_variants::parse(IDLParserContext{"root"}, obj);
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());
        ASSERT_EQ(stdx::get<int>(parsed.getValue0()), 1);
        ASSERT(stdx::get<std::vector<std::string>>(parsed.getValue1()) ==
               (std::vector<std::string>{"x", "y"}));
        ASSERT_BSONOBJ_EQ(Two_variants(1, std::vector<std::string>{"x", "y"}).toBSON(), obj);
    }

    {
        auto obj = BSON("value0"
                        << "test_value"
                        << "value1"
                        << BSON_ARRAY("x"
                                      << "y"));
        auto parsed = Two_variants::parse(IDLParserContext{"root"}, obj);
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());
        ASSERT_EQ(stdx::get<std::string>(parsed.getValue0()), "test_value");
        ASSERT(stdx::get<std::vector<std::string>>(parsed.getValue1()) ==
               (std::vector<std::string>{"x", "y"}));
        ASSERT_BSONOBJ_EQ(Two_variants("test_value", std::vector<std::string>{"x", "y"}).toBSON(),
                          obj);
    }
}

TEST(IDLVariantTests, TestChainedStructVariant) {
    IDLParserContext ctxt("root");
    {
        auto obj = BSON("value"
                        << "x"
                        << "field1"
                        << "y");
        auto parsed = Chained_struct_variant::parse(ctxt, obj);
        ASSERT_EQ(stdx::get<std::string>(parsed.getOne_variant_compound().getValue()), "x");
        ASSERT_EQ(parsed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());

        Chained_struct_variant assembled;
        assembled.setOne_variant_compound(One_variant_compound("x"));
        assembled.setField1("y");
        ASSERT_BSONOBJ_EQ(obj, assembled.toBSON());

        // Test the constructor.
        Chained_struct_variant constructed("y");
        constructed.setOne_variant_compound(One_variant_compound("x"));
        ASSERT_EQ(stdx::get<std::string>(constructed.getOne_variant_compound().getValue()), "x");
        ASSERT_EQ(constructed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, constructed.toBSON());
    }
    {
        auto obj = BSON("value" << BSON_ARRAY("x"
                                              << "y")
                                << "field1"
                                << "y");
        auto parsed = Chained_struct_variant::parse(ctxt, obj);
        ASSERT(stdx::get<std::vector<std::string>>(parsed.getOne_variant_compound().getValue()) ==
               (std::vector<std::string>{"x", "y"}));
        ASSERT_EQ(parsed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());

        Chained_struct_variant assembled;
        assembled.setOne_variant_compound(One_variant_compound(std::vector<std::string>{"x", "y"}));
        assembled.setField1("y");
        ASSERT_BSONOBJ_EQ(obj, assembled.toBSON());

        // Test the constructor.
        Chained_struct_variant constructed("y");
        constructed.setOne_variant_compound(
            One_variant_compound(std::vector<std::string>{"x", "y"}));
        ASSERT(
            stdx::get<std::vector<std::string>>(constructed.getOne_variant_compound().getValue()) ==
            (std::vector<std::string>{"x", "y"}));
        ASSERT_EQ(constructed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, constructed.toBSON());
    }
    {
        auto obj = BSON("value" << BSONObj() << "field1"
                                << "y");
        auto parsed = Chained_struct_variant::parse(ctxt, obj);
        ASSERT_BSONOBJ_EQ(stdx::get<BSONObj>(parsed.getOne_variant_compound().getValue()),
                          BSONObj());
        ASSERT_EQ(parsed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());

        Chained_struct_variant assembled;
        assembled.setOne_variant_compound(One_variant_compound(BSONObj()));
        assembled.setField1("y");
        ASSERT_BSONOBJ_EQ(obj, assembled.toBSON());

        // Test the constructor.
        Chained_struct_variant constructed("y");
        constructed.setOne_variant_compound({BSONObj()});
        ASSERT_BSONOBJ_EQ(stdx::get<BSONObj>(constructed.getOne_variant_compound().getValue()),
                          BSONObj());
        ASSERT_EQ(constructed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, constructed.toBSON());
    }
}

TEST(IDLVariantTests, TestChainedStructVariantInline) {
    IDLParserContext ctxt("root");
    {
        auto obj = BSON("value"
                        << "x"
                        << "field1"
                        << "y");
        auto parsed = Chained_struct_variant_inline::parse(ctxt, obj);
        ASSERT_EQ(stdx::get<std::string>(parsed.getValue()), "x");
        ASSERT_EQ(parsed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());

        Chained_struct_variant_inline assembled;
        assembled.setOne_variant_compound(One_variant_compound("x"));
        assembled.setField1("y");
        ASSERT_BSONOBJ_EQ(obj, assembled.toBSON());

        // Test the constructor.
        Chained_struct_variant_inline constructed("y");
        constructed.setOne_variant_compound(One_variant_compound("x"));
        ASSERT_EQ(stdx::get<std::string>(constructed.getValue()), "x");
        ASSERT_EQ(constructed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, constructed.toBSON());
    }
    {
        auto obj = BSON("value" << BSON_ARRAY("x"
                                              << "y")
                                << "field1"
                                << "y");
        auto parsed = Chained_struct_variant_inline::parse(ctxt, obj);
        ASSERT(stdx::get<std::vector<std::string>>(parsed.getValue()) ==
               (std::vector<std::string>{"x", "y"}));
        ASSERT_EQ(parsed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());

        Chained_struct_variant_inline assembled;
        assembled.setOne_variant_compound(One_variant_compound(std::vector<std::string>{"x", "y"}));
        assembled.setField1("y");
        ASSERT_BSONOBJ_EQ(obj, assembled.toBSON());

        // Test the constructor.
        Chained_struct_variant_inline constructed("y");
        constructed.setOne_variant_compound(
            One_variant_compound(std::vector<std::string>{"x", "y"}));
        ASSERT(stdx::get<std::vector<std::string>>(constructed.getValue()) ==
               (std::vector<std::string>{"x", "y"}));
        ASSERT_EQ(constructed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, constructed.toBSON());
    }
    {
        auto obj = BSON("value" << BSONObj() << "field1"
                                << "y");
        auto parsed = Chained_struct_variant_inline::parse(ctxt, obj);
        ASSERT_BSONOBJ_EQ(stdx::get<BSONObj>(parsed.getValue()), BSONObj());
        ASSERT_EQ(parsed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());

        Chained_struct_variant_inline assembled;
        assembled.setOne_variant_compound(One_variant_compound(BSONObj()));
        assembled.setField1("y");
        ASSERT_BSONOBJ_EQ(obj, assembled.toBSON());

        // Test the constructor.
        Chained_struct_variant_inline constructed("y");
        constructed.setOne_variant_compound({BSONObj()});
        ASSERT_BSONOBJ_EQ(stdx::get<BSONObj>(constructed.getValue()), BSONObj());
        ASSERT_EQ(constructed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, constructed.toBSON());
    }
}

TEST(IDLVariantTests, TestChainedStructVariantStruct) {
    IDLParserContext ctxt("root");
    {
        auto obj = BSON("value" << 1 << "field1"
                                << "y");
        auto parsed = Chained_struct_variant_struct::parse(ctxt, obj);
        ASSERT_EQ(stdx::get<int>(parsed.getOne_variant_struct().getValue()), 1);
        ASSERT_EQ(parsed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());

        Chained_struct_variant_struct assembled;
        assembled.setOne_variant_struct(One_variant_struct(1));
        assembled.setField1("y");
        ASSERT_BSONOBJ_EQ(obj, assembled.toBSON());

        // Test the constructor.
        Chained_struct_variant_struct constructed("y");
        constructed.setOne_variant_struct(One_variant_struct(1));
        ASSERT_EQ(stdx::get<int>(constructed.getOne_variant_struct().getValue()), 1);
        ASSERT_EQ(constructed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, constructed.toBSON());
    }
    {
        auto obj = BSON("value" << BSON("value"
                                        << "x")
                                << "field1"
                                << "y");
        auto parsed = Chained_struct_variant_struct::parse(ctxt, obj);
        ASSERT_EQ(stdx::get<One_string>(parsed.getOne_variant_struct().getValue()).getValue(), "x");
        ASSERT_EQ(parsed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());

        Chained_struct_variant_struct assembled;
        assembled.setOne_variant_struct(One_variant_struct(One_string("x")));
        assembled.setField1("y");
        ASSERT_BSONOBJ_EQ(obj, assembled.toBSON());

        // Test the constructor.
        Chained_struct_variant_struct constructed("y");
        constructed.setOne_variant_struct(One_variant_struct(One_string("x")));
        ASSERT_EQ(stdx::get<One_string>(constructed.getOne_variant_struct().getValue()).getValue(),
                  "x");
        ASSERT_EQ(constructed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, constructed.toBSON());
    }
}

TEST(IDLVariantTests, TestChainedStructVariantStructInline) {
    IDLParserContext ctxt("root");
    {
        auto obj = BSON("value" << 1 << "field1"
                                << "y");
        auto parsed = Chained_struct_variant_struct_inline::parse(ctxt, obj);
        ASSERT_EQ(stdx::get<int>(parsed.getValue()), 1);
        ASSERT_EQ(parsed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());

        Chained_struct_variant_struct_inline assembled;
        assembled.setOne_variant_struct(One_variant_struct(1));
        assembled.setField1("y");
        ASSERT_BSONOBJ_EQ(obj, assembled.toBSON());

        // Test the constructor.
        Chained_struct_variant_struct_inline constructed("y");
        constructed.setOne_variant_struct(One_variant_struct(1));
        ASSERT_EQ(stdx::get<int>(constructed.getValue()), 1);
        ASSERT_EQ(constructed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, constructed.toBSON());
    }
    {
        auto obj = BSON("value" << BSON("value"
                                        << "x")
                                << "field1"
                                << "y");
        auto parsed = Chained_struct_variant_struct_inline::parse(ctxt, obj);
        ASSERT_EQ(stdx::get<One_string>(parsed.getValue()).getValue(), "x");
        ASSERT_EQ(parsed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, parsed.toBSON());

        Chained_struct_variant_struct_inline assembled;
        assembled.setOne_variant_struct(One_variant_struct(One_string("x")));
        assembled.setField1("y");
        ASSERT_BSONOBJ_EQ(obj, assembled.toBSON());

        // Test the constructor.
        Chained_struct_variant_struct_inline constructed("y");
        constructed.setOne_variant_struct(One_variant_struct(One_string("x")));
        ASSERT_EQ(stdx::get<One_string>(constructed.getValue()).getValue(), "x");
        ASSERT_EQ(constructed.getField1(), "y");
        ASSERT_BSONOBJ_EQ(obj, constructed.toBSON());
    }
}

/// Struct tests:
// Positive: strict, 3 required fields
// Negative: strict, ensure extra fields fail
// Negative: strict, duplicate fields
TEST(IDLStructTests, TestStrictStruct) {
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

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

    // Negative: null required field
    {
        auto testDoc = BSON(RequiredNonStrictField3::kCppField1FieldName << 12 << "2" << 123 << "3"
                                                                         << BSONNULL);
        ASSERT_THROWS(RequiredNonStrictField3::parse(ctxt, testDoc), AssertionException);
    }
}

TEST(IDLStructTests, WriteConcernTest) {
    IDLParserContext ctxt("root");
    // Numeric w value
    {
        auto writeConcernDoc = BSON("w" << 1 << "j" << true << "wtimeout" << 5000);
        auto writeConcernStruct = WriteConcernIdl::parse(ctxt, writeConcernDoc);
        BSONObjBuilder builder;
        writeConcernStruct.serialize(&builder);
        ASSERT_BSONOBJ_EQ(builder.obj(), writeConcernDoc);
    }
    // String w value
    {
        auto writeConcernDoc = BSON("w"
                                    << "majority"
                                    << "j" << true << "wtimeout" << 5000);
        auto writeConcernStruct = WriteConcernIdl::parse(ctxt, writeConcernDoc);
        BSONObjBuilder builder;
        writeConcernStruct.serialize(&builder);
        ASSERT_BSONOBJ_EQ(builder.obj(), writeConcernDoc);
    }
    // Ignore options wElectionId, wOpTime, getLastError
    {
        auto writeConcernDoc = BSON("w"
                                    << "majority"
                                    << "j" << true << "wtimeout" << 5000 << "wElectionId" << 12345
                                    << "wOpTime" << 98765 << "getLastError" << true);
        auto writeConcernDocWithoutIgnoredFields = BSON("w"
                                                        << "majority"
                                                        << "j" << true << "wtimeout" << 5000);
        auto writeConcernStruct = WriteConcernIdl::parse(ctxt, writeConcernDoc);
        BSONObjBuilder builder;
        writeConcernStruct.serialize(&builder);
        ASSERT_BSONOBJ_EQ(builder.obj(), writeConcernDocWithoutIgnoredFields);
    }
}

TEST(IDLStructTests, TestValidator) {
    // Parser should assert that the values are equal.
    IDLParserContext ctxt("root");
    auto objToParse = BSON("first" << 1 << "second" << 2);

    ASSERT_THROWS_CODE(StructWithValidator::parse(ctxt, objToParse), AssertionException, 6253512);

    objToParse = BSON("first" << 1 << "second" << 1);
    StructWithValidator::parse(ctxt, objToParse);
}

/// Struct default comparison tests
TEST(IDLCompareTests, TestAllFields) {
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

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

#define TEST_DEFAULT_VALUES_VARIANT(field_name, default_type, default_value, new_type, new_value) \
    {                                                                                             \
        auto testDoc = BSONObj();                                                                 \
        auto testStruct = Default_values::parse(ctxt, testDoc);                                   \
        ASSERT_TRUE(stdx::holds_alternative<default_type>(testStruct.get##field_name()));         \
        ASSERT_EQUALS(stdx::get<default_type>(testStruct.get##field_name()), default_value);      \
    }                                                                                             \
    {                                                                                             \
        auto testDoc = BSON(#field_name << new_value);                                            \
        auto testStruct = Default_values::parse(ctxt, testDoc);                                   \
        ASSERT_TRUE(stdx::holds_alternative<new_type>(testStruct.get##field_name()));             \
        ASSERT_EQUALS(stdx::get<new_type>(testStruct.get##field_name()), new_value);              \
    }

// Mixed: struct strict, and ignored field works
TEST(IDLFieldTests, TestDefaultFields) {
    IDLParserContext ctxt("root");

    TEST_DEFAULT_VALUES(V_string, "a default", "foo");
    TEST_DEFAULT_VALUES(V_int, 42, 3);
    TEST_DEFAULT_VALUES(V_long, 423, 4LL);
    TEST_DEFAULT_VALUES(V_double, 3.14159, 2.8);
    TEST_DEFAULT_VALUES(V_bool, true, false);
    TEST_DEFAULT_VALUES_VARIANT(V_variant_string, std::string, "a default", int, 42);
    TEST_DEFAULT_VALUES_VARIANT(V_variant_int, int, 42, std::string, "a default");
}

// Positive: struct strict, and optional field works
TEST(IDLFieldTests, TestOptionalFields) {
    IDLParserContext ctxt("root");

    // Positive: Test document with only string field
    {
        auto testDoc = BSON("field1"
                            << "Foo");
        auto testStruct = Optional_field::parse(ctxt, testDoc);
        assert_same_types<decltype(testStruct.getField1()), boost::optional<StringData>>();
        assert_same_types<decltype(testStruct.getField2()), boost::optional<int>>();
        assert_same_types<decltype(testStruct.getField3()), const boost::optional<BSONObj>&>();
        assert_same_types<decltype(testStruct.getField4()), boost::optional<ConstDataRange>>();
        assert_same_types<decltype(testStruct.getField5()),
                          boost::optional<std::array<std::uint8_t, 16>>>();

        ASSERT_EQUALS("Foo", testStruct.getField1().value());
        ASSERT_FALSE(testStruct.getField2().has_value());
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
        ASSERT_FALSE(testStruct.getField1().has_value());
        ASSERT_EQUALS(123, testStruct.getField2().value());
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

TEST(IDLFieldTests, TestAlwaysSerializeFields) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON("field1"
                        << "Foo"
                        << "field3" << BSON("a" << 1234));
    auto testStruct = Always_serialize_field::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()), boost::optional<mongo::StringData>>();
    assert_same_types<decltype(testStruct.getField2()), boost::optional<std::int32_t>>();
    assert_same_types<decltype(testStruct.getField3()), const boost::optional<mongo::BSONObj>&>();
    assert_same_types<decltype(testStruct.getField4()), const boost::optional<mongo::BSONObj>&>();
    assert_same_types<decltype(testStruct.getField5()), const boost::optional<mongo::BSONObj>&>();

    ASSERT_EQUALS("Foo", testStruct.getField1().value());
    ASSERT_FALSE(testStruct.getField2().has_value());
    ASSERT_BSONOBJ_EQ(BSON("a" << 1234), testStruct.getField3().value());
    ASSERT_FALSE(testStruct.getField4().has_value());
    ASSERT_FALSE(testStruct.getField5().has_value());

    BSONObjBuilder builder;
    testStruct.serialize(&builder);
    auto loopbackDoc = builder.obj();
    auto docWithNulls = BSON("field1"
                             << "Foo"
                             << "field2" << BSONNULL << "field3" << BSON("a" << 1234) << "field4"
                             << BSONNULL);
    ASSERT_BSONOBJ_EQ(docWithNulls, loopbackDoc);
}

template <typename TestT>
void TestWeakType(TestT test_value) {
    IDLParserContext ctxt("root");
    auto testDoc = BSON("field1" << test_value << "field2" << test_value << "field3" << test_value
                                 << "field4" << test_value << "field5" << test_value);
    auto testStruct = Optional_field::parse(ctxt, testDoc);

    ASSERT_FALSE(testStruct.getField1().has_value());
    ASSERT_FALSE(testStruct.getField2().has_value());
    ASSERT_FALSE(testStruct.getField3().has_value());
    ASSERT_FALSE(testStruct.getField4().has_value());
    ASSERT_FALSE(testStruct.getField5().has_value());
}

// Positive: struct strict, and optional field works
TEST(IDLFieldTests, TestOptionalFieldsWithNullAndUndefined) {

    TestWeakType<NullLabeler>(BSONNULL);

    TestWeakType<UndefinedLabeler>(BSONUndefined);
}

// Positive: Test a nested struct
TEST(IDLNestedStruct, TestDuplicateTypes) {
    IDLParserContext ctxt("root");


    // Positive: Test document
    auto testDoc = BSON(

        "field1" << BSON("field1" << 1 << "field2" << 2 << "field3" << 3) <<

        "field3" << BSON("field1" << 4 << "field2" << 5 << "field3" << 6));
    auto testStruct = NestedWithDuplicateTypes::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()), RequiredStrictField3&>();
    assert_same_types<decltype(testStruct.getField2()),
                      boost::optional<RequiredNonStrictField3>&>();
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
    IDLParserContext ctxt("root");

    // Positive: Test document
    uint8_t array1[] = {1, 2, 3};
    uint8_t array2[] = {4, 6, 8};

    uint8_t array15[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t array16[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    auto testDoc = BSON("field1" << BSON_ARRAY("Foo"
                                               << "Bar"
                                               << "???")
                                 << "field2" << BSON_ARRAY(1 << 2 << 3) << "field3"
                                 << BSON_ARRAY(1.2 << 3.4 << 5.6) << "field4"
                                 << BSON_ARRAY(BSONBinData(array1, 3, BinDataGeneral)
                                               << BSONBinData(array2, 3, BinDataGeneral))
                                 << "field5"
                                 << BSON_ARRAY(BSONBinData(array15, 16, newUUID)
                                               << BSONBinData(array16, 16, newUUID)));
    auto testStruct = Simple_array_fields::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()), std::vector<StringData>>();
    assert_same_types<decltype(testStruct.getField2()), const std::vector<std::int32_t>&>();
    assert_same_types<decltype(testStruct.getField3()), const std::vector<double>&>();
    assert_same_types<decltype(testStruct.getField4()), std::vector<ConstDataRange>>();
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

// Positive: Array of variant
TEST(IDLArrayTests, TestArrayOfVariant) {
    IDLParserContext ctxt("root");
    auto testDoc = BSON("value" << BSON_ARRAY(BSON("insert" << 13) << BSON("update"
                                                                           << "some word")));
    auto parsed = One_array_variant::parse(ctxt, testDoc);
    ASSERT_BSONOBJ_EQ(testDoc, parsed.toBSON());

    ASSERT_EQ(stdx::get<Insert_variant_struct>(parsed.getValue()[0]).getInsert(), 13);
    ASSERT_EQ(stdx::get<Update_variant_struct>(parsed.getValue()[1]).getUpdate(), "some word");

    // Positive: Test we can roundtrip from the just parsed document
    {
        BSONObjBuilder builder;
        parsed.serialize(&builder);
        auto loopbackDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDoc, loopbackDoc);
    }
}

// Positive: Optional Arrays
TEST(IDLArrayTests, TestSimpleOptionalArrays) {
    IDLParserContext ctxt("root");

    // Positive: Test document
    auto testDoc = BSON("field1" << BSON_ARRAY("Foo"
                                               << "Bar"
                                               << "???")
                                 << "field2" << BSON_ARRAY(1 << 2 << 3) << "field3"
                                 << BSON_ARRAY(1.2 << 3.4 << 5.6)

    );
    auto testStruct = Optional_array_fields::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()), boost::optional<std::vector<StringData>>>();
    assert_same_types<decltype(testStruct.getField2()),
                      const boost::optional<std::vector<std::int32_t>>&>();
    assert_same_types<decltype(testStruct.getField3()),
                      const boost::optional<std::vector<double>>&>();
    assert_same_types<decltype(testStruct.getField4()),
                      boost::optional<std::vector<ConstDataRange>>>();
    assert_same_types<decltype(testStruct.getField5()),
                      const boost::optional<std::vector<std::array<std::uint8_t, 16>>>&>();

    std::vector<StringData> field1{"Foo", "Bar", "???"};
    ASSERT_TRUE(field1 == testStruct.getField1().value());
    std::vector<std::int32_t> field2{1, 2, 3};
    ASSERT_TRUE(field2 == testStruct.getField2().value());
    std::vector<double> field3{1.2, 3.4, 5.6};
    ASSERT_TRUE(field3 == testStruct.getField3().value());

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
    IDLParserContext ctxt("root");

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

// Negative: Test arrays with good field names but made with BSONObjBuilder::subobjStart
TEST(IDLArrayTests, TestGoodArraysWithObjectType) {
    IDLParserContext ctxt("root");

    {
        BSONObjBuilder builder;
        {
            BSONObjBuilder subBuilder(builder.subobjStart("field1"));
            subBuilder.append("0", 1);
            subBuilder.append("1", 2);
        }

        auto testDoc = builder.obj();
        ASSERT_THROWS(Simple_int_array::parse(ctxt, testDoc), AssertionException);
    }
}

// Positive: Test arrays with good field names but made with BSONObjBuilder::subarrayStart
TEST(IDLArrayTests, TestGoodArraysWithArrayType) {
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

    // Positive: Test document
    auto testDoc = BSON("field1" << BSON_ARRAY(1 << 2 << 3) << "field2"
                                 << BSON_ARRAY("a.b"
                                               << "c.d")
                                 << "field3" << BSON_ARRAY(1 << "2") << "field4"
                                 << BSON_ARRAY(BSONObj() << BSONObj()) << "field5"
                                 << BSON_ARRAY(BSONObj() << BSONObj() << BSONObj()) << "field6"
                                 << BSON_ARRAY(BSON("value"
                                                    << "hello")
                                               << BSON("value"
                                                       << "world"))
                                 << "field1o" << BSON_ARRAY(1 << 2 << 3) << "field2o"
                                 << BSON_ARRAY("a.b"
                                               << "c.d")
                                 << "field3o" << BSON_ARRAY(1 << "2") << "field4o"
                                 << BSON_ARRAY(BSONObj() << BSONObj()) << "field6o"
                                 << BSON_ARRAY(BSON("value"
                                                    << "goodbye")
                                               << BSON("value"
                                                       << "world"))

    );
    auto testStruct = Complex_array_fields::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getField1()), const std::vector<std::int64_t>&>();
    assert_same_types<decltype(testStruct.getField2()),
                      const std::vector<mongo::NamespaceString>&>();
    assert_same_types<decltype(testStruct.getField3()), const std::vector<mongo::AnyBasicType>&>();
    assert_same_types<decltype(testStruct.getField4()),
                      const std::vector<mongo::ObjectBasicType>&>();
    assert_same_types<decltype(testStruct.getField5()), const std::vector<mongo::BSONObj>&>();
    assert_same_types<decltype(testStruct.getField6()), std::vector<idl::import::One_string>&>();

    assert_same_types<decltype(testStruct.getField1o()),
                      const boost::optional<std::vector<std::int64_t>>&>();
    assert_same_types<decltype(testStruct.getField2o()),
                      const boost::optional<std::vector<mongo::NamespaceString>>&>();
    assert_same_types<decltype(testStruct.getField3o()),
                      const boost::optional<std::vector<mongo::AnyBasicType>>&>();
    assert_same_types<decltype(testStruct.getField4o()),
                      const boost::optional<std::vector<mongo::ObjectBasicType>>&>();
    assert_same_types<decltype(testStruct.getField5o()),
                      const boost::optional<std::vector<mongo::BSONObj>>&>();
    assert_same_types<decltype(testStruct.getField6o()),
                      boost::optional<std::vector<idl::import::One_string>>&>();

    std::vector<std::int64_t> field1{1, 2, 3};
    ASSERT_TRUE(field1 == testStruct.getField1());
    std::vector<NamespaceString> field2{{"a", "b"}, {"c", "d"}};
    ASSERT_TRUE(field2 == testStruct.getField2());

    ASSERT_EQUALS(testStruct.getField6().size(), 2u);
    ASSERT_EQUALS(testStruct.getField6()[0].getValue(), "hello");
    ASSERT_EQUALS(testStruct.getField6()[1].getValue(), "world");
    ASSERT_EQUALS(testStruct.getField6o().value().size(), 2u);
    ASSERT_EQUALS(testStruct.getField6o().value()[0].getValue(), "goodbye");
    ASSERT_EQUALS(testStruct.getField6o().value()[1].getValue(), "world");
}

template <typename ParserT, BinDataType bindata_type>
void TestBinDataVector() {
    IDLParserContext ctxt("root");

    // Positive: Test document with only a generic bindata field
    uint8_t testData[] = {1, 2, 3};
    auto testDoc = BSON("value" << BSONBinData(testData, 3, bindata_type));
    auto testStruct = ParserT::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getValue()), ConstDataRange>();

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
    IDLParserContext ctxt("root");

    // Positive: Test document with only a generic bindata field
    uint8_t testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    auto testDoc = BSON("value" << BSONBinData(testData, 16, bindata_type));
    auto testStruct = ParserT::parse(ctxt, testDoc);

    assert_same_types<decltype(testStruct.getValue()), std::array<uint8_t, 16>>();

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
        IDLParserContext ctxt("root");

        uint8_t testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        auto testDoc = BSON("value" << BSONBinData(testData, 15, MD5Type));
        ASSERT_THROWS(One_md5::parse(ctxt, testDoc), AssertionException);
    }
}

// Test if a given value for a given bson document parses successfully or fails if the bson types
// mismatch.
template <typename ParserT, BinDataType Parser_bindata_type, BinDataType Test_bindata_type>
void TestBinDataParse() {
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

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
    static ClassDerivedFromStruct parseFromBSON(const IDLParserContext& ctxt,
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
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

    auto testDoc = BSON("field1"
                        << "abc"
                        << "field2" << 5);

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
    IDLParserContext ctxt("root");

    auto testDoc = BSON("field1"
                        << "abc"
                        << "field2" << 5 << "field3" << 123456);

    auto testStruct = Chained_struct_only::parse(ctxt, testDoc);
    ASSERT_EQUALS(testStruct.getChainedType().getField1(), "abc");
    ASSERT_EQUALS(testStruct.getAnotherChainedType().getField2(), 5);
}


// Negative: demonstrate a struct with chained types with duplicate fields
TEST(IDLChainedType, TestDuplicateFields) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON("field1"
                        << "abc"
                        << "field2" << 5 << "field2" << 123456);

    ASSERT_THROWS(Chained_struct_only::parse(ctxt, testDoc), AssertionException);
}


// Positive: demonstrate a struct with chained structs
TEST(IDLChainedType, TestChainedStruct) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON("anyField" << 123.456 << "objectField"
                                   << BSON("random"
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

        // Serializer should include fields with default values.
        ASSERT_BSONOBJ_EQ(BSON("anyField" << 123.456 << "objectField"
                                          << BSON("random"
                                                  << "pair")
                                          << "enumField"  // Should be ahead of 'field3'.
                                          << "zero"
                                          << "field3"
                                          << "abc"),
                          loopbackDoc);
    }
}

// Negative: demonstrate a struct with chained structs and extra fields
TEST(IDLChainedType, TestChainedStructWithExtraFields) {
    IDLParserContext ctxt("root");

    // Extra field
    {
        auto testDoc = BSON("field3"
                            << "abc"
                            << "anyField" << 123.456 << "objectField"
                            << BSON("random"
                                    << "pair")
                            << "extraField" << 787);
        ASSERT_THROWS(Chained_struct_mixed::parse(ctxt, testDoc), AssertionException);
    }


    // Duplicate any
    {
        auto testDoc = BSON("field3"
                            << "abc"
                            << "anyField" << 123.456 << "objectField"
                            << BSON("random"
                                    << "pair")
                            << "anyField" << 787);
        ASSERT_THROWS(Chained_struct_mixed::parse(ctxt, testDoc), AssertionException);
    }

    // Duplicate object
    {
        auto testDoc = BSON("objectField" << BSON("fake"
                                                  << "thing")
                                          << "field3"
                                          << "abc"
                                          << "anyField" << 123.456 << "objectField"
                                          << BSON("random"
                                                  << "pair"));
        ASSERT_THROWS(Chained_struct_mixed::parse(ctxt, testDoc), AssertionException);
    }

    // Duplicate field3
    {
        auto testDoc = BSON("field3"
                            << "abc"
                            << "anyField" << 123.456 << "objectField"
                            << BSON("random"
                                    << "pair")
                            << "field3"
                            << "def");
        ASSERT_THROWS(Chained_struct_mixed::parse(ctxt, testDoc), AssertionException);
    }
}


// Positive: demonstrate a struct with chained structs and types
TEST(IDLChainedType, TestChainedMixedStruct) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON("field1"
                        << "abc"
                        << "field2" << 5 << "stringField"
                        << "def"
                        << "field3" << 456);

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

    IDLParserContext ctxt("root");

    auto testDoc = BSON("field1" << 2 << "field2"
                                 << "zero");
    auto testStruct = StructWithEnum::parse(ctxt, testDoc);
    ASSERT_TRUE(testStruct.getField1() == IntEnum::c2);
    ASSERT_TRUE(testStruct.getField2() == StringEnumEnum::s0);
    ASSERT_TRUE(testStruct.getFieldDefault() == StringEnumEnum::s1);

    assert_same_types<decltype(testStruct.getField1()), IntEnum>();
    assert_same_types<decltype(testStruct.getField1o()), boost::optional<IntEnum>>();
    assert_same_types<decltype(testStruct.getField2()), StringEnumEnum>();
    assert_same_types<decltype(testStruct.getField2o()), boost::optional<StringEnumEnum>>();
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
    IDLParserContext ctxt("root");

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
    IDLParserContext ctxt("root");

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

TEST(IDLEnum, ExtraDataEnum) {
    auto s0Data = ExtraDataEnum_get_extra_data(ExtraDataEnumEnum::s0);
    ASSERT_BSONOBJ_EQ(s0Data, BSONObj());

    auto s1Data = ExtraDataEnum_get_extra_data(ExtraDataEnumEnum::s1);
    ASSERT_BSONOBJ_EQ(s1Data, BSONObj());

    auto s2Data = ExtraDataEnum_get_extra_data(ExtraDataEnumEnum::s2);
    auto s2Expected = fromjson(R"json({ foo: [{bar: "baz"}], baz: "\"qu\\\\nx\"" })json");
    ASSERT_BSONOBJ_EQ(s2Data, s2Expected);
}

OpMsgRequest makeOMR(BSONObj obj) {
    OpMsgRequest request;
    request.body = obj;
    return request;
}

OpMsgRequest makeOMRWithTenant(BSONObj obj, TenantId tenant) {
    OpMsgRequest request;
    request.body = obj;

    using VTS = auth::ValidatedTenancyScope;
    request.validatedTenancyScope = VTS(std::move(tenant), VTS::TenantForTestingTag{});
    return request;
}

// Positive: demonstrate a command with concatenate with db
TEST(IDLCommand, TestConcatentateWithDb) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON(BasicConcatenateWithDbCommand::kCommandName << "coll1"
                                                                    << "field1" << 3 << "field2"
                                                                    << "five"
                                                                    << "$db"
                                                                    << "db");

    auto testStruct = BasicConcatenateWithDbCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getField2(), "five");
    ASSERT_EQUALS(testStruct.getNamespace(),
                  NamespaceString::createNamespaceString_forTest("db.coll1"));

    assert_same_types<decltype(testStruct.getNamespace()), const NamespaceString&>();

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));

    // Positive: Test we can serialize from nothing the same document except for $db
    {
        auto testDocWithoutDb =
            BSON(BasicConcatenateWithDbCommand::kCommandName << "coll1"
                                                             << "field1" << 3 << "field2"
                                                             << "five");

        BSONObjBuilder builder;
        BasicConcatenateWithDbCommand one_new(
            NamespaceString::createNamespaceString_forTest("db.coll1"));
        one_new.setField1(3);
        one_new.setField2("five");
        one_new.serialize(BSONObj(), &builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDocWithoutDb, serializedDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BasicConcatenateWithDbCommand one_new(
            NamespaceString::createNamespaceString_forTest("db.coll1"));
        one_new.setField1(3);
        one_new.setField2("five");
        ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));
    }
}

TEST(IDLCommand, TestConcatentateWithDb_WithTenant) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    IDLParserContext ctxt("root");

    const auto tenantId = TenantId(OID::gen());
    const auto prefixedDb = std::string(str::stream() << tenantId.toString() << "_db");

    auto testDoc = BSONObjBuilder{}
                       .append(BasicConcatenateWithDbCommand::kCommandName, "coll1")
                       .append("field1", 3)
                       .append("field2", "five")
                       .append("$db", prefixedDb)
                       .obj();

    auto testStruct =
        BasicConcatenateWithDbCommand::parse(ctxt, makeOMRWithTenant(testDoc, tenantId));
    ASSERT_EQUALS(testStruct.getDbName(), DatabaseName(tenantId, "db"));
    ASSERT_EQUALS(testStruct.getNamespace(),
                  NamespaceString::createNamespaceString_forTest(tenantId, "db.coll1"));

    assert_same_types<decltype(testStruct.getNamespace()), const NamespaceString&>();

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));
}

TEST(IDLCommand, TestConcatentateWithDb_TestConstructor) {
    const auto tenantId = TenantId(OID::gen());
    const DatabaseName dbName(tenantId, "db");

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, "coll1");
    BasicConcatenateWithDbCommand testRequest(nss);
    ASSERT_EQUALS(testRequest.getDbName().tenantId(), dbName.tenantId());
    ASSERT_EQUALS(testRequest.getDbName(), dbName);
}

TEST(IDLCommand, TestConcatentateWithDbSymbol) {
    IDLParserContext ctxt("root");

    // Postive - symbol???
    {
        auto testDoc =
            BSON("BasicConcatenateWithDbCommand" << BSONSymbol("coll1") << "field1" << 3 << "field2"
                                                 << "five"
                                                 << "$db"
                                                 << "db");
        auto testStruct = BasicConcatenateWithDbCommand::parse(ctxt, makeOMR(testDoc));
        ASSERT_EQUALS(testStruct.getNamespace(),
                      NamespaceString::createNamespaceString_forTest("db.coll1"));
    }
}


TEST(IDLCommand, TestConcatentateWithDbNegative) {
    IDLParserContext ctxt("root");

    // Negative - duplicate namespace field
    {
        auto testDoc =
            BSON("BasicConcatenateWithDbCommand" << 1 << "field1" << 3
                                                 << "BasicConcatenateWithDbCommand" << 1 << "field2"
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
    IDLParserContext ctxt("root");

    auto testDoc =
        BSON(BasicConcatenateWithDbOrUUIDCommand::kCommandName << "coll1"
                                                               << "field1" << 3 << "field2"
                                                               << "five"
                                                               << "$db"
                                                               << "db");

    auto testStruct = BasicConcatenateWithDbOrUUIDCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getField2(), "five");
    ASSERT_EQUALS(testStruct.getNamespaceOrUUID().nss().value(),
                  NamespaceString::createNamespaceString_forTest("db.coll1"));

    assert_same_types<decltype(testStruct.getNamespaceOrUUID()), const NamespaceStringOrUUID&>();

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));

    // Positive: Test we can serialize from nothing the same document except for $db
    {
        auto testDocWithoutDb =
            BSON(BasicConcatenateWithDbOrUUIDCommand::kCommandName << "coll1"
                                                                   << "field1" << 3 << "field2"
                                                                   << "five");

        BSONObjBuilder builder;
        BasicConcatenateWithDbOrUUIDCommand one_new(
            NamespaceString::createNamespaceString_forTest("db.coll1"));
        one_new.setField1(3);
        one_new.setField2("five");
        one_new.serialize(BSONObj(), &builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDocWithoutDb, serializedDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        BasicConcatenateWithDbOrUUIDCommand one_new(
            NamespaceString::createNamespaceString_forTest("db.coll1"));
        one_new.setField1(3);
        one_new.setField2("five");
        ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(one_new));
    }
}

TEST(IDLCommand, TestConcatentateWithDbOrUUID_TestNSS_WithTenant) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    IDLParserContext ctxt("root");

    const auto tenantId = TenantId(OID::gen());
    const auto prefixedDb = std::string(str::stream() << tenantId.toString() << "_db");

    auto testDoc = BSONObjBuilder{}
                       .append(BasicConcatenateWithDbOrUUIDCommand::kCommandName, "coll1")
                       .append("field1", 3)
                       .append("field2", "five")
                       .append("$db", prefixedDb)
                       .obj();

    auto testStruct =
        BasicConcatenateWithDbOrUUIDCommand::parse(ctxt, makeOMRWithTenant(testDoc, tenantId));
    ASSERT_EQUALS(testStruct.getDbName(), DatabaseName(tenantId, "db"));
    ASSERT_EQUALS(testStruct.getNamespaceOrUUID().nss().value(),
                  NamespaceString::createNamespaceString_forTest(tenantId, "db.coll1"));

    assert_same_types<decltype(testStruct.getNamespaceOrUUID()), const NamespaceStringOrUUID&>();

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));
}

// Positive: demonstrate a command with concatenate with db or uuid - test UUID
TEST(IDLCommand, TestConcatentateWithDbOrUUID_TestUUID) {
    IDLParserContext ctxt("root");

    UUID uuid = UUID::gen();

    auto testDoc =
        BSON(BasicConcatenateWithDbOrUUIDCommand::kCommandName << uuid << "field1" << 3 << "field2"
                                                               << "five"
                                                               << "$db"
                                                               << "db");

    auto testStruct = BasicConcatenateWithDbOrUUIDCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getField2(), "five");
    ASSERT_EQUALS(testStruct.getNamespaceOrUUID().uuid().value(), uuid);

    assert_same_types<decltype(testStruct.getNamespaceOrUUID()), const NamespaceStringOrUUID&>();

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));

    // Positive: Test we can serialize from nothing the same document except for $db
    {
        auto testDocWithoutDb = BSON(BasicConcatenateWithDbOrUUIDCommand::kCommandName
                                     << uuid << "field1" << 3 << "field2"
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
        BasicConcatenateWithDbOrUUIDCommand one_new(NamespaceStringOrUUID("db", uuid));
        one_new.setField1(3);
        one_new.setField2("five");
        ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(one_new));
    }
}

TEST(IDLCommand, TestConcatentateWithDbOrUUID_TestUUID_WithTenant) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    IDLParserContext ctxt("root");

    UUID uuid = UUID::gen();
    const auto tenantId = TenantId(OID::gen());
    const auto prefixedDb = std::string(str::stream() << tenantId.toString() << "_db");

    auto testDoc =
        BSONObjBuilder{}
            .appendElements(BSON(BasicConcatenateWithDbOrUUIDCommand::kCommandName << uuid))
            .append("field1", 3)
            .append("field2", "five")
            .append("$db", prefixedDb)
            .obj();

    auto testStruct =
        BasicConcatenateWithDbOrUUIDCommand::parse(ctxt, makeOMRWithTenant(testDoc, tenantId));
    ASSERT_EQUALS(testStruct.getDbName(), DatabaseName(tenantId, "db"));
    ASSERT_EQUALS(testStruct.getNamespaceOrUUID().dbName().value(), DatabaseName(tenantId, "db"));

    assert_same_types<decltype(testStruct.getNamespaceOrUUID()), const NamespaceStringOrUUID&>();

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));
}

TEST(IDLCommand, TestConcatentateWithDbOrUUID_TestConstructor) {
    const UUID uuid = UUID::gen();
    const auto tenantId = TenantId(OID::gen());
    const DatabaseName dbName(tenantId, "db");

    const NamespaceStringOrUUID withUUID(dbName, uuid);
    BasicConcatenateWithDbOrUUIDCommand testRequest1(withUUID);
    ASSERT_EQUALS(testRequest1.getDbName().tenantId(), dbName.tenantId());
    ASSERT_EQUALS(testRequest1.getDbName(), dbName);

    const NamespaceStringOrUUID withNss(
        NamespaceString::createNamespaceString_forTest(dbName, "coll1"));
    BasicConcatenateWithDbOrUUIDCommand testRequest2(withNss);
    ASSERT_EQUALS(testRequest2.getDbName().tenantId(), dbName.tenantId());
    ASSERT_EQUALS(testRequest2.getDbName(), dbName);
}

TEST(IDLCommand, TestConcatentateWithDbOrUUIDNegative) {
    IDLParserContext ctxt("root");

    // Negative - duplicate namespace field
    {
        auto testDoc =
            BSON("BasicConcatenateWithDbOrUUIDCommand"
                 << 1 << "field1" << 3 << "BasicConcatenateWithDbOrUUIDCommand" << 1 << "field2"
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
    IDLParserContext ctxt("root");

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
        BasicIgnoredCommand one_new;
        one_new.setField1(3);
        one_new.setField2("five");
        one_new.setDbName(DatabaseName(boost::none, "admin"));
        ASSERT_BSONOBJ_EQ(testDocWithDB, serializeCmd(one_new));
    }
}


TEST(IDLCommand, TestIgnoredNegative) {
    IDLParserContext ctxt("root");

    // Negative - duplicate namespace field
    {
        auto testDoc = BSON("BasicIgnoredCommand" << 1 << "field1" << 3 << "BasicIgnoredCommand"
                                                  << 1 << "field2"
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

// We don't generate comparison operators like "==" for variants, so test only for BSON equality.
template <typename CommandT, typename TestT, BSONType Test_bson_type>
void TestLoopbackCommandTypeVariant(TestT test_value) {
    IDLParserContext ctxt("root");

    BSONObjBuilder bob;
    if constexpr (idl::hasBSONSerialize<TestT>) {
        // TestT might be an IDL struct type like One_string.
        BSONObjBuilder subObj(bob.subobjStart(CommandT::kCommandParameterFieldName));
        test_value.serialize(&subObj);
    } else if constexpr (std::is_same_v<TestT, UUID>) {
        test_value.appendToBuilder(&bob, CommandT::kCommandParameterFieldName);
    } else {
        bob.append(CommandT::kCommandParameterFieldName, test_value);
    }

    bob.append("$db", "db");
    auto obj = bob.obj();
    auto element = obj.firstElement();
    ASSERT_EQUALS(element.type(), Test_bson_type);

    auto parsed = CommandT::parse(ctxt, obj);
    if constexpr (std::is_same_v<TestT, BSONObj>) {
        ASSERT_BSONOBJ_EQ(stdx::get<TestT>(parsed.getValue()), test_value);
    } else {
        // Use ASSERT instead of ASSERT_EQ to avoid operator<<
        ASSERT(stdx::get<TestT>(parsed.getCommandParameter()) == test_value);
    }
    ASSERT_BSONOBJ_EQ(obj, serializeCmd(parsed));

    // Test the constructor.
    CommandT constructed(test_value);
    constructed.setDbName(DatabaseName(boost::none, "db"));
    if constexpr (std::is_same_v<TestT, BSONObj>) {
        ASSERT_BSONOBJ_EQ(stdx::get<TestT>(parsed.getValue()), test_value);
    } else {
        ASSERT(stdx::get<TestT>(parsed.getCommandParameter()) == test_value);
    }
    ASSERT_BSONOBJ_EQ(obj, serializeCmd(constructed));
}

TEST(IDLCommand, TestCommandTypeVariant) {
    TestLoopbackCommandTypeVariant<CommandTypeVariantCommand, int, NumberInt>(1);
    TestLoopbackCommandTypeVariant<CommandTypeVariantCommand, std::string, String>("test_value");
    TestLoopbackCommandTypeVariant<CommandTypeVariantCommand, std::vector<std::string>, Array>(
        {"x", "y"});

    TestLoopbackCommandTypeVariant<CommandTypeVariantUUIDCommand, int, NumberInt>(1);
    TestLoopbackCommandTypeVariant<CommandTypeVariantUUIDCommand, UUID, BinData>(UUID::gen());

    TestLoopbackCommandTypeVariant<CommandTypeVariantStructCommand, bool, Bool>(true);
    TestLoopbackCommandTypeVariant<CommandTypeVariantStructCommand, One_string, Object>(
        One_string("test_value"));
}

TEST(IDLDocSequence, TestBasic) {
    IDLParserContext ctxt("root");

    auto testTempDoc = BSON("DocSequenceCommand"
                            << "coll1"
                            << "field1" << 3 << "field2"
                            << "five"
                            << "$db"
                            << "db"
                            << "structs"
                            << BSON_ARRAY(BSON("value"
                                               << "hello")
                                          << BSON("value"
                                                  << "world"))
                            << "objects" << BSON_ARRAY(BSON("foo" << 1)));

    OpMsgRequest request;
    request.body = testTempDoc;

    auto testStruct = DocSequenceCommand::parse(ctxt, request);
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getField2(), "five");
    ASSERT_EQUALS(testStruct.getNamespace(),
                  NamespaceString::createNamespaceString_forTest("db.coll1"));

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
        DocSequenceCommand one_new(NamespaceString::createNamespaceString_forTest("db.coll1"));
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
    IDLParserContext ctxt("root");

    auto testTempDoc = BSON("DocSequenceCommand"
                            << "coll1"
                            << "field1" << 3 << "field2"
                            << "five"
                            << "structs"
                            << BSON_ARRAY(BSON("value"
                                               << "hello"))
                            << "objects" << BSON_ARRAY(BSON("foo" << 1)));

    OpMsgRequest request;
    request.body = testTempDoc;

    ASSERT_THROWS(DocSequenceCommand::parse(ctxt, request), AssertionException);
}

// Positive: Test a command read and written to OpMsgRequest with content in DocumentSequence works
template <typename TestT>
void TestDocSequence(StringData name) {
    IDLParserContext ctxt("root");

    auto testTempDoc = BSON(name << "coll1"
                                 << "field1" << 3 << "field2"
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
    ASSERT_EQUALS(testStruct.getNamespace(),
                  NamespaceString::createNamespaceString_forTest("db.coll1"));

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
    IDLParserContext ctxt("root");

    auto testTempDoc = BSON(name << "coll1"
                                 << "field1" << 3 << "field2"
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
    IDLParserContext ctxt("root");

    // Negative: Duplicate fields in doc sequence and body
    {
        auto testTempDoc = BSON(name << "coll1"
                                     << "field1" << 3 << "field2"
                                     << "five"
                                     << "structs"
                                     << BSON_ARRAY(BSON("value"
                                                        << "hello")
                                                   << BSON("value"
                                                           << "world"))
                                     << "objects" << BSON_ARRAY(BSON("foo" << 1)));

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
                                     << "field1" << 3 << "field2"
                                     << "five"
                                     << "structs"
                                     << BSON_ARRAY(BSON("value"
                                                        << "hello")
                                                   << BSON("value"
                                                           << "world"))
                                     << "objects" << BSON_ARRAY(BSON("foo" << 1)));

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
    IDLParserContext ctxt("root");

    // Negative: Duplicate fields in doc sequence and body
    {
        auto testTempDoc = BSON("DocSequenceCommand"
                                << "coll1"
                                << "field1" << 3 << "field2"
                                << "five"
                                << "structs"
                                << BSON_ARRAY(BSON("value"
                                                   << "hello")
                                              << BSON("value"
                                                      << "world"))
                                << "objects" << BSON_ARRAY(BSON("foo" << 1)));

        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"structs", {}});

        ASSERT_THROWS(DocSequenceCommand::parse(ctxt, request), AssertionException);
    }

    // Positive: Empty document sequence
    {
        auto testTempDoc = BSON("DocSequenceCommand"
                                << "coll1"
                                << "field1" << 3 << "field2"
                                << "five"
                                << "objects" << BSON_ARRAY(BSON("foo" << 1)));

        OpMsgRequest request = OpMsgRequest::fromDBAndBody("db", testTempDoc);
        request.sequences.push_back({"structs", {}});

        auto testStruct = DocSequenceCommand::parse(ctxt, request);

        ASSERT_EQUALS(0UL, testStruct.getStructs().size());
    }
}

// Positive: Test all the OpMsg well known fields are ignored
TEST(IDLDocSequence, TestWellKnownFieldsAreIgnored) {
    IDLParserContext ctxt("root");

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
                                << "field1" << 3 << "field2"
                                << "five" << knownField << "extra"
                                << "structs"
                                << BSON_ARRAY(BSON("value"
                                                   << "hello")
                                              << BSON("value"
                                                      << "world"))
                                << "objects" << BSON_ARRAY(BSON("foo" << 1)));


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
    IDLParserContext ctxt("root");

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
                                << "field1" << 3 << "field2"
                                << "five"
                                << "$db"
                                << "db" << knownField << "extra"
                                << "structs"
                                << BSON_ARRAY(BSON("value"
                                                   << "hello")
                                              << BSON("value"
                                                      << "world"))
                                << "objects" << BSON_ARRAY(BSON("foo" << 1)));

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
    IDLParserContext ctxt("root");

    // Positive: Extra field in document sequence
    {
        auto testTempDoc = BSON("DocSequenceCommandNonStrict"
                                << "coll1"
                                << "field1" << 3 << "field2"
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
                                << "field1" << 3 << "field2"
                                << "five"
                                << "extra" << 1);

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

TEST(IDLDocSequence, TestArrayVariant) {
    IDLParserContext ctxt("root");

    auto testTempDoc = BSON("DocSequenceCommandArrayVariant"
                            << "coll1"
                            << "$db"
                            << "db"
                            << "structs"
                            << BSON_ARRAY(BSON("update"
                                               << "foo")
                                          << BSON("insert" << 12)));

    OpMsgRequest request;
    request.body = testTempDoc;

    auto testStruct = DocSequenceCommandArrayVariant::parse(ctxt, request);
    ASSERT_EQUALS(2UL, testStruct.getStructs().size());
    ASSERT_EQ(stdx::get<Update_variant_struct>(testStruct.getStructs()[0]).getUpdate(), "foo");
    ASSERT_EQ(stdx::get<Insert_variant_struct>(testStruct.getStructs()[1]).getInsert(), 12);

    assert_same_types<decltype(testStruct.getNamespace()), const NamespaceString&>();

    // Positive: Test we can round trip to a document sequence from the just parsed document
    {
        OpMsgRequest loopbackRequest = testStruct.serialize(BSONObj());

        assertOpMsgEquals(request, loopbackRequest);
        ASSERT_EQUALS(loopbackRequest.sequences.size(), 1UL);  // just "structs"
        ASSERT_EQUALS(loopbackRequest.sequences[0].name, "structs");
        ASSERT_EQUALS(loopbackRequest.sequences[0].objs.size(), 2);

        // Verify doc sequence part of DocSequenceCommandArrayVariant::parseProtected too.
        testStruct = DocSequenceCommandArrayVariant::parse(ctxt, loopbackRequest);
        ASSERT_EQUALS(2UL, testStruct.getStructs().size());
        ASSERT_EQ(stdx::get<Update_variant_struct>(testStruct.getStructs()[0]).getUpdate(), "foo");
        ASSERT_EQ(stdx::get<Insert_variant_struct>(testStruct.getStructs()[1]).getInsert(), 12);
    }
}

// Postive: Test a Command known field does not propagate from passthrough to the final BSON if it
// is included as a field in the command.
TEST(IDLCommand, TestKnownFieldDuplicate) {
    IDLParserContext ctxt("root");

    auto testPassthrough = BSON("$db"
                                << "foo"
                                << "maxTimeMS" << 6 << "$client"
                                << "foo");

    auto testDoc = BSON("KnownFieldCommand"
                        << "coll1"
                        << "$db"
                        << "db"
                        << "field1" << 28 << "maxTimeMS" << 42);

    auto testStruct = KnownFieldCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(28, testStruct.getField1());
    ASSERT_EQUALS(42, testStruct.getMaxTimeMS());

    // OpMsg request serializes original '$db' out because it is part of the OP_MSG request
    auto expectedOpMsgDoc = BSON("KnownFieldCommand"
                                 << "coll1"

                                 << "field1" << 28 << "maxTimeMS" << 42 << "$db"
                                 << "db"

                                 << "$client"
                                 << "foo");

    ASSERT_BSONOBJ_EQ(expectedOpMsgDoc, testStruct.serialize(testPassthrough).body);

    // BSON serialize does not round-trip '$db' because it can passed in passthrough data
    auto expectedBSONDoc = BSON("KnownFieldCommand"
                                << "coll1"

                                << "field1" << 28 << "maxTimeMS" << 42 << "$db"
                                << "foo"

                                << "$client"
                                << "foo");

    ASSERT_BSONOBJ_EQ(expectedBSONDoc, testStruct.toBSON(testPassthrough));
}


// Positive: Test an inline nested chain struct works
TEST(IDLChainedStruct, TestInline) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON("stringField"
                        << "bar"
                        << "field3"
                        << "foo");

    auto testStruct = Chained_struct_inline::parse(ctxt, testDoc);
    ASSERT_EQUALS(testStruct.getChained_string_inline_basic_type().getStringField(), "bar");
    ASSERT_EQUALS(testStruct.getField3(), "foo");

    assert_same_types<decltype(testStruct.getChained_string_inline_basic_type().getStringField()),
                      StringData>();
    assert_same_types<decltype(testStruct.getField3()), StringData>();

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
        IDLParserContext ctxt("root");
        auto doc = BSON("positive_int" << pos << "negative_int" << neg << "non_negative_int"
                                       << nonneg << "non_positive_int" << nonpos << "byte_range_int"
                                       << byte_range << "range_int" << int_range);
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
        IDLParserContext ctxt("root");
        auto doc = BSON("positive_int" << pos << "negative_int" << neg << "non_negative_int"
                                       << nonneg << "non_positive_int" << nonpos << "byte_range_int"
                                       << byte_range << "range_int" << int_range);
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
            IDLParserContext ctxt("root");
            auto doc = BSON("positive_double"
                            << pos << "negative_double" << neg << "non_negative_double" << nonneg
                            << "non_positive_double" << nonpos << "range_double" << double_range);
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
            IDLParserContext ctxt("root");
            auto doc = BSON("positive_double"
                            << pos << "negative_double" << neg << "non_negative_double" << nonneg
                            << "non_positive_double" << nonpos << "range_double" << double_range);
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
            IDLParserContext ctxt("root");
            auto doc = BSON("int_even" << int_even << "double_nearly_int" << double_nearly_int
                                       << "string_starts_with_x" << string_starts_with_x);
            auto obj = Callback_validators::parse(ctxt, doc);
            ASSERT_EQUALS(obj.getInt_even(), int_even);
            ASSERT_EQUALS(obj.getDouble_nearly_int(), double_nearly_int);
            ASSERT_EQUALS(obj.getString_starts_with_x(), string_starts_with_x);
        };

    // Negative case parsing.
    const auto tryFail =
        [](std::int32_t int_even, double double_nearly_int, StringData string_starts_with_x) {
            IDLParserContext ctxt("root");
            auto doc = BSON("int_even" << int_even << "double_nearly_int" << double_nearly_int
                                       << "string_starts_with_x" << string_starts_with_x);
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

// Test validation of integer array
TEST(IDLValidatedArray, IntArrayValidation) {

    const auto tryPass = [](std::vector<std::int32_t> int_even) {
        IDLParserContext ctxt("root");
        auto doc = BSON("int_even" << int_even);
        auto obj = Int_array_validators::parse(ctxt, doc);

        ASSERT_EQUALS(obj.getInt_even().size(), int_even.size());
        for (size_t i = 0; i < int_even.size(); ++i) {
            ASSERT_EQ(obj.getInt_even()[i], int_even[i]);
        }
    };

    tryPass({2, 4, 6, 10, 100, 200, 2456});
    tryPass({});
    tryPass({344});

    const auto tryFail = [](std::vector<std::int32_t> int_uneven) {
        IDLParserContext ctxt("root");
        auto doc = BSON("int_even" << int_uneven);
        ASSERT_THROWS(Int_array_validators::parse(ctxt, doc), AssertionException);
    };

    tryFail({1, 3, 5, 7});
    tryFail({9, 35, 4});
    tryFail({90, 22, 33});
    tryFail({122, 44, 101, 64});
}

// Test validation of string array
TEST(IDLValidatedArray, StringArrayValidation) {

    const auto tryPass = [](std::vector<std::string> valid) {
        IDLParserContext ctxt("root");
        auto doc = BSON("caps_strings" << valid);
        auto obj = String_array_validators::parse(ctxt, doc);

        ASSERT_EQUALS(obj.getCaps_strings().size(), valid.size());
        for (size_t i = 0; i < valid.size(); ++i) {
            ASSERT_EQ(obj.getCaps_strings()[i], valid[i]);
        }
    };

    tryPass({"HELLO"});
    tryPass({});
    tryPass({"ABC", "DEF", "XYZ"});

    const auto tryFail = [](std::vector<std::string> invalid) {
        IDLParserContext ctxt("root");
        auto doc = BSON("caps_strings" << invalid);
        ASSERT_THROWS(String_array_validators::parse(ctxt, doc), AssertionException);
    };

    tryFail({"hello"});
    tryFail({"AB1"});
    tryFail({"MONGO", "car", "QWERTY"});
    tryFail({"SLD", "SLA", "KS D"});
    tryFail({"S3LD", "SLA", "ED"});
}

// Positive: verify a command a string arg
TEST(IDLTypeCommand, TestString) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON(CommandTypeStringCommand::kCommandName << "foo"
                                                               << "field1" << 3 << "$db"
                                                               << "db");

    auto testStruct = CommandTypeStringCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getCommandParameter(), "foo");

    assert_same_types<decltype(testStruct.getCommandParameter()), StringData>();

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));

    // Positive: Test we can serialize from nothing the same document except for $db
    {
        auto testDocWithoutDb = BSON(CommandTypeStringCommand::kCommandName << "foo"
                                                                            << "field1" << 3);

        BSONObjBuilder builder;
        CommandTypeStringCommand one_new("foo");
        one_new.setField1(3);
        one_new.setDbName(DatabaseName(boost::none, "db"));
        one_new.serialize(BSONObj(), &builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDocWithoutDb, serializedDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        CommandTypeStringCommand one_new("foo");
        one_new.setField1(3);
        one_new.setDbName(DatabaseName(boost::none, "db"));
        OpMsgRequest reply = one_new.serialize(BSONObj());
        ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(one_new));
    }
}

// Positive: verify a command can take an array of object
TEST(IDLTypeCommand, TestArrayObject) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON(CommandTypeArrayObjectCommand::kCommandName << BSON_ARRAY(BSON("sample"
                                                                                       << "doc"))
                                                                    << "$db"
                                                                    << "db");

    auto testStruct = CommandTypeArrayObjectCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getCommandParameter().size(), 1UL);

    assert_same_types<decltype(testStruct.getCommandParameter()),
                      const std::vector<mongo::BSONObj>&>();

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));

    // Positive: Test we can serialize from nothing the same document
    {
        std::vector<BSONObj> vec;
        vec.emplace_back(BSON("sample"
                              << "doc"));
        CommandTypeArrayObjectCommand one_new(vec);
        one_new.setDbName(DatabaseName(boost::none, "db"));
        ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(one_new));
    }
}

// Positive: verify a command can take a struct
TEST(IDLTypeCommand, TestStruct) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON(CommandTypeStructCommand::kCommandName << BSON("value"
                                                                       << "sample")
                                                               << "$db"
                                                               << "db");

    auto testStruct = CommandTypeStructCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getCommandParameter().getValue(), "sample");

    assert_same_types<decltype(testStruct.getCommandParameter()),
                      mongo::idl::import::One_string&>();

    // Negative: Command with struct parameter should disallow 'undefined' input.
    {
        auto invalidDoc = BSON(CommandTypeStructCommand::kCommandName << BSONUndefined << "$db"
                                                                      << "db");
        ASSERT_THROWS(CommandTypeStructCommand::parse(ctxt, makeOMR(invalidDoc)),
                      AssertionException);
    }

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));

    // Positive: Test we can serialize from nothing the same document
    {
        One_string os;
        os.setValue("sample");
        CommandTypeStructCommand one_new(os);
        one_new.setDbName(DatabaseName(boost::none, "db"));
        ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(one_new));
    }
}

// Positive: verify a command can take an array of structs
TEST(IDLTypeCommand, TestStructArray) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON(CommandTypeArrayStructCommand::kCommandName << BSON_ARRAY(BSON("value"
                                                                                       << "sample"))
                                                                    << "$db"
                                                                    << "db");

    auto testStruct = CommandTypeArrayStructCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getCommandParameter().size(), 1UL);

    assert_same_types<decltype(testStruct.getCommandParameter()),
                      std::vector<mongo::idl::import::One_string>&>();

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));

    // Positive: Test we can serialize from nothing the same document
    {
        std::vector<One_string> vec;
        One_string os;
        os.setValue("sample");
        vec.push_back(os);
        CommandTypeArrayStructCommand one_new(vec);
        one_new.setDbName(DatabaseName(boost::none, "db"));
        ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(one_new));
    }
}

// Positive: verify a command a string arg and alternate C++ name
TEST(IDLTypeCommand, TestUnderscoreCommand) {
    IDLParserContext ctxt("root");

    auto testDoc = BSON(WellNamedCommand::kCommandName << "foo"
                                                       << "field1" << 3 << "$db"
                                                       << "db");

    auto testStruct = WellNamedCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getCommandParameter(), "foo");

    assert_same_types<decltype(testStruct.getCommandParameter()), StringData>();

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));

    // Positive: Test we can serialize from nothing the same document except for $db
    {
        auto testDocWithoutDb = BSON(WellNamedCommand::kCommandName << "foo"
                                                                    << "field1" << 3);

        BSONObjBuilder builder;
        WellNamedCommand one_new("foo");
        one_new.setField1(3);
        one_new.setDbName(DatabaseName(boost::none, "db"));
        one_new.serialize(BSONObj(), &builder);

        auto serializedDoc = builder.obj();
        ASSERT_BSONOBJ_EQ(testDocWithoutDb, serializedDoc);
    }

    // Positive: Test we can serialize from nothing the same document
    {
        WellNamedCommand one_new("foo");
        one_new.setField1(3);
        one_new.setDbName(DatabaseName(boost::none, "db"));
        ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(one_new));
    }
}

TEST(IDLTypeCommand, TestErrorReplyStruct) {
    // Correctly parse all required fields.
    {
        IDLParserContext ctxt("root");

        auto errorDoc = BSON("ok" << 0.0 << "code" << 123456 << "codeName"
                                  << "blah blah"
                                  << "errmsg"
                                  << "This is an error Message"
                                  << "errorLabels"
                                  << BSON_ARRAY("label1"
                                                << "label2"));
        auto errorReply = ErrorReply::parse(ctxt, errorDoc);
        ASSERT_BSONOBJ_EQ(errorReply.toBSON(), errorDoc);
    }
    // Non-strictness: ensure we parse even if input has extra fields.
    {
        IDLParserContext ctxt("root");

        auto errorDoc = BSON("a"
                             << "b"
                             << "ok" << 0.0 << "code" << 123456 << "codeName"
                             << "blah blah"
                             << "errmsg"
                             << "This is an error Message");
        auto errorReply = ErrorReply::parse(ctxt, errorDoc);
        ASSERT_BSONOBJ_EQ(errorReply.toBSON(),
                          BSON("ok" << 0.0 << "code" << 123456 << "codeName"
                                    << "blah blah"
                                    << "errmsg"
                                    << "This is an error Message"));
    }
    // Ensure that we fail to parse if any required fields are missing.
    {
        IDLParserContext ctxt("root");

        auto missingOk = BSON("code" << 123456 << "codeName"
                                     << "blah blah"
                                     << "errmsg"
                                     << "This is an error Message");
        auto missingCode = BSON("ok" << 0.0 << "codeName"
                                     << "blah blah"
                                     << "errmsg"
                                     << "This is an error Message");
        auto missingCodeName = BSON("ok" << 0.0 << "code" << 123456 << "errmsg"
                                         << "This is an error Message");
        auto missingErrmsg = BSON("ok" << 0.0 << "code" << 123456 << "codeName"
                                       << "blah blah");
        ASSERT_THROWS(ErrorReply::parse(ctxt, missingOk), AssertionException);
        ASSERT_THROWS(ErrorReply::parse(ctxt, missingCode), AssertionException);
        ASSERT_THROWS(ErrorReply::parse(ctxt, missingCodeName), AssertionException);
        ASSERT_THROWS(ErrorReply::parse(ctxt, missingErrmsg), AssertionException);
    }
}

TEST(IDLTypeCommand, TestCommandWithIDLAnyTypeField) {
    IDLParserContext ctxt("root");
    std::vector<BSONObj> differentTypeObjs = {
        BSON(CommandWithAnyTypeMember::kCommandName << 1 << "anyTypeField"
                                                    << "string literal"
                                                    << "$db"
                                                    << "db"),
        BSON(CommandWithAnyTypeMember::kCommandName << 1 << "anyTypeField" << 1234 << "$db"
                                                    << "db"),
        BSON(CommandWithAnyTypeMember::kCommandName << 1 << "anyTypeField" << 1234.5 << "$db"
                                                    << "db"),
        BSON(CommandWithAnyTypeMember::kCommandName << 1 << "anyTypeField" << OID::max() << "$db"
                                                    << "db"),
        BSON(CommandWithAnyTypeMember::kCommandName << 1 << "anyTypeField" << Date_t::now() << "$db"
                                                    << "db"),
        BSON(CommandWithAnyTypeMember::kCommandName << 1 << "anyTypeField"
                                                    << BSON("a"
                                                            << "b")
                                                    << "$db"
                                                    << "db"),
        BSON(CommandWithAnyTypeMember::kCommandName << 1 << "anyTypeField"
                                                    << BSON_ARRAY("a"
                                                                  << "b")
                                                    << "$db"
                                                    << "db"),
        BSON(CommandWithAnyTypeMember::kCommandName << 1 << "anyTypeField" << jstNULL << "$db"
                                                    << "db")};
    for (auto&& obj : differentTypeObjs) {
        auto parsed = CommandWithAnyTypeMember::parse(ctxt, obj);
        ASSERT_BSONELT_EQ(parsed.getAnyTypeField().getElement(), obj["anyTypeField"]);
    }
}

TEST(IDLCommand, BasicNamespaceConstGetterCommand_TestNonConstGetterGeneration) {
    IDLParserContext ctxt("root");
    const auto uuid = UUID::gen();
    auto testDoc =
        BSON(BasicNamespaceConstGetterCommand::kCommandName << uuid << "field1" << 3 << "$db"
                                                            << "db");

    auto testStruct = BasicNamespaceConstGetterCommand::parse(ctxt, makeOMR(testDoc));
    ASSERT_EQUALS(testStruct.getField1(), 3);
    ASSERT_EQUALS(testStruct.getNamespaceOrUUID().uuid().value(), uuid);

    // Verify that both const and non-const getters are generated.
    assert_same_types<
        decltype(std::declval<BasicNamespaceConstGetterCommand>().getNamespaceOrUUID()),
        NamespaceStringOrUUID&>();
    assert_same_types<
        decltype(std::declval<const BasicNamespaceConstGetterCommand>().getNamespaceOrUUID()),
        const NamespaceStringOrUUID&>();

    // Test we can roundtrip from the just parsed document.
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));

    // Test mutable getter modifies the command object.
    {
        auto& nssOrUuid = testStruct.getNamespaceOrUUID();
        const auto nss = NamespaceString::createNamespaceString_forTest("test.coll");
        nssOrUuid.setNss(nss);
        nssOrUuid.preferNssForSerialization();

        BSONObjBuilder builder;
        testStruct.serialize(BSONObj(), &builder);

        // Verify that nss was used for serialization over uuid.
        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON(BasicNamespaceConstGetterCommand::kCommandName << "coll"
                                                                              << "field1" << 3));
    }
}

TEST(IDLTypeCommand, TestCommandWithIDLAnyTypeOwnedField) {
    IDLParserContext ctxt("root");

    auto parsed = CommandWithAnyTypeOwnedMember::parse(
        ctxt,
        BSON(CommandWithAnyTypeOwnedMember::kCommandName << 1 << "anyTypeField"
                                                         << "string literal"
                                                         << "$db"
                                                         << "db"));
    ASSERT_EQ(parsed.getAnyTypeField().getElement().type(), String);
    ASSERT_EQ(parsed.getAnyTypeField().getElement().str(), "string literal");

    parsed = CommandWithAnyTypeOwnedMember::parse(ctxt,
                                                  BSON(CommandWithAnyTypeOwnedMember::kCommandName
                                                       << 1 << "anyTypeField" << 1234 << "$db"
                                                       << "db"));
    ASSERT_EQ(parsed.getAnyTypeField().getElement().type(), NumberInt);
    ASSERT_EQ(parsed.getAnyTypeField().getElement().numberInt(), 1234);

    parsed = CommandWithAnyTypeOwnedMember::parse(ctxt,
                                                  BSON(CommandWithAnyTypeOwnedMember::kCommandName
                                                       << 1 << "anyTypeField" << 1234.5 << "$db"
                                                       << "db"));
    ASSERT_EQ(parsed.getAnyTypeField().getElement().type(), NumberDouble);
    ASSERT_EQ(parsed.getAnyTypeField().getElement().numberDouble(), 1234.5);

    parsed = CommandWithAnyTypeOwnedMember::parse(ctxt,
                                                  BSON(CommandWithAnyTypeOwnedMember::kCommandName
                                                       << 1 << "anyTypeField" << OID::max() << "$db"
                                                       << "db"));
    ASSERT_EQ(parsed.getAnyTypeField().getElement().type(), jstOID);
    ASSERT_EQ(parsed.getAnyTypeField().getElement().OID(), OID::max());

    parsed = CommandWithAnyTypeOwnedMember::parse(ctxt,
                                                  BSON(CommandWithAnyTypeOwnedMember::kCommandName
                                                       << 1 << "anyTypeField"
                                                       << BSON("a"
                                                               << "b")
                                                       << "$db"
                                                       << "db"));
    ASSERT_EQ(parsed.getAnyTypeField().getElement().type(), Object);
    ASSERT_BSONOBJ_EQ(parsed.getAnyTypeField().getElement().Obj(),
                      BSON("a"
                           << "b"));

    parsed = CommandWithAnyTypeOwnedMember::parse(ctxt,
                                                  BSON(CommandWithAnyTypeOwnedMember::kCommandName
                                                       << 1 << "anyTypeField"
                                                       << BSON_ARRAY("a"
                                                                     << "b")
                                                       << "$db"
                                                       << "db"));
    ASSERT_EQ(parsed.getAnyTypeField().getElement().type(), Array);
    ASSERT_BSONELT_EQ(parsed.getAnyTypeField().getElement(),
                      BSON("anyTypeField" << BSON_ARRAY("a"
                                                        << "b"))["anyTypeField"]);
}

TEST(IDLTypeCommand, ReplyTypeKnowsItIsReplyAtCompileTime) {
    Reply_type_struct reply;
    static_assert(reply.getIsCommandReply());
    StructWithEnum nonReply;
    static_assert(!nonReply.getIsCommandReply());
}

TEST(IDLTypeCommand, ReplyTypeCanParseWithGenericFields) {
    // $clusterTime is not a field of Rely_type_struct, but is
    // a field that could be part of any reply.
    StringData genericField = "$clusterTime"_sd;
    // This field is not part of Reply_type_struct and is also
    // not a generic field.
    StringData nonGenericField = "xyz123"_sd;
    IDLParserContext ctxt("root");
    // This contains only fields part of Reply_type_struct and generic fields
    auto bsonValidReply = BSON("reply_field" << 42 << genericField << 1);
    auto parsed = CommandWithReplyType::Reply::parse(ctxt, bsonValidReply);
    ASSERT(parsed.getIsCommandReply());
    ASSERT_EQ(parsed.getReply_field(), 42);

    // This contains a field not part of Reply_type struct, so shouldn't parse
    auto bsonInvalidReply = BSON("reply_field" << 42 << nonGenericField << 1);
    ASSERT_THROWS(CommandWithReplyType::Reply::parse(ctxt, bsonInvalidReply), DBException);
}

TEST(IDLCommand,
     TestCommandTypeNamespaceCommand_WithMultitenancySupportOnFeatureFlagRequireTenantIDOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    IDLParserContext ctxt("root");

    auto testDoc = BSON(CommandTypeNamespaceCommand::kCommandName << "db.coll1"
                                                                  << "field1" << 3 << "$db"
                                                                  << "admin");

    const auto tenantId = TenantId(OID::gen());
    auto testStruct =
        CommandTypeNamespaceCommand::parse(ctxt, makeOMRWithTenant(testDoc, tenantId));
    ASSERT_EQUALS(testStruct.getDbName(), DatabaseName(tenantId, "admin"));
    ASSERT_EQUALS(testStruct.getCommandParameter(),
                  NamespaceString::createNamespaceString_forTest(tenantId, "db.coll1"));
    assert_same_types<decltype(testStruct.getCommandParameter()), const NamespaceString&>();
    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));
}

TEST(IDLCommand, TestCommandTypeNamespaceCommand_WithMultitenancySupportOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);

    IDLParserContext ctxt("root");

    const auto tenantId = TenantId(OID::gen());
    const auto nssWithPrefixedTenantId =
        std::string(str::stream() << tenantId.toString() << "_db.coll1");
    const auto prefixedAdminDb = std::string(str::stream() << tenantId.toString() << "_admin");

    auto testDoc = BSON(CommandTypeNamespaceCommand::kCommandName
                        << nssWithPrefixedTenantId << "field1" << 3 << "$db" << prefixedAdminDb);

    auto testStruct = CommandTypeNamespaceCommand::parse(ctxt, makeOMR(testDoc));

    ASSERT_EQUALS(testStruct.getDbName(), DatabaseName(tenantId, "admin"));
    // Deserialize called from parse correctly sets the tenantId field.
    ASSERT_EQUALS(testStruct.getCommandParameter(),
                  NamespaceString::createNamespaceString_forTest(tenantId, "db.coll1"));
    assert_same_types<decltype(testStruct.getCommandParameter()), const NamespaceString&>();
    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));
}

TEST(IDLTypeCommand, TestCommandWithNamespaceMember_WithTenant) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    IDLParserContext ctxt("root");
    const char* ns1 = "db.coll1";
    const char* ns2 = "a.b";
    const char* ns3 = "c.d";

    auto testDoc = BSONObjBuilder{}
                       .append("CommandWithNamespaceMember", 1)
                       .append("field1", ns1)
                       .append("field2", BSON_ARRAY(ns2 << ns3))
                       .append("$db", "admin")
                       .obj();

    const auto tenantId = TenantId(OID::gen());
    auto testStruct = CommandWithNamespaceMember::parse(ctxt, makeOMRWithTenant(testDoc, tenantId));

    assert_same_types<decltype(testStruct.getField1()), const NamespaceString&>();
    assert_same_types<decltype(testStruct.getField2()),
                      const std::vector<mongo::NamespaceString>&>();

    ASSERT_EQUALS(testStruct.getField1(),
                  NamespaceString::createNamespaceString_forTest(tenantId, ns1));
    std::vector<NamespaceString> field2{
        NamespaceString::createNamespaceString_forTest(tenantId, ns2),
        NamespaceString::createNamespaceString_forTest(tenantId, ns3)};
    ASSERT_TRUE(field2 == testStruct.getField2());

    // Positive: Test we can roundtrip from the just parsed document
    ASSERT_BSONOBJ_EQ(testDoc, serializeCmd(testStruct));
}

TEST(IDLTypeCommand, TestCommandWithNamespaceStruct_WithTenant) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    IDLParserContext ctxt("root");
    const char* ns1 = "db.coll1";
    const char* ns2 = "a.b";
    const char* ns3 = "c.d";

    auto nsInfoStructBSON = [&](const char* ns) {
        BSONObjBuilder builder;
        builder.append("ns", ns);
        return builder.obj();
    };
    auto testDoc = BSONObjBuilder{}
                       .append("CommandWithNamespaceStruct", 1)
                       .append("field1", nsInfoStructBSON(ns1))
                       .append("$db", "admin")
                       .append("field2", BSON_ARRAY(nsInfoStructBSON(ns2) << nsInfoStructBSON(ns3)))
                       .obj();

    const auto tenantId = TenantId(OID::gen());
    auto testStruct = CommandWithNamespaceStruct::parse(ctxt, makeOMRWithTenant(testDoc, tenantId));

    assert_same_types<decltype(testStruct.getField1()), NamespaceInfoStruct&>();
    assert_same_types<decltype(testStruct.getField2()), std::vector<NamespaceInfoStruct>&>();

    ASSERT_EQUALS(testStruct.getField1().getNs(),
                  NamespaceString::createNamespaceString_forTest(tenantId, ns1));
    std::vector<NamespaceString> field2Nss{
        NamespaceString::createNamespaceString_forTest(tenantId, ns2),
        NamespaceString::createNamespaceString_forTest(tenantId, ns3)};
    std::vector<NamespaceInfoStruct>& field2 = testStruct.getField2();
    ASSERT_TRUE(field2Nss[0] == field2[0].getNs());
    ASSERT_TRUE(field2Nss[1] == field2[1].getNs());

    // Positive: Test we can round trip to a document sequence from the just parsed document
    {
        OpMsgRequest loopbackRequest = testStruct.serialize(BSONObj());
        OpMsgRequest request = makeOMR(testDoc);

        assertOpMsgEquals(request, loopbackRequest);
        ASSERT_EQUALS(loopbackRequest.sequences.size(), 1UL);
        ASSERT_EQUALS(loopbackRequest.sequences[0].objs.size(), 2UL);
    }
}

TEST(IDLParserContext, TestConstructorWithPredecessorAndDifferentTenant) {
    // Negative: Test the child IDLParserContext cannot has different tenant id from its
    // predecessor.
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);

    const auto tenantId = TenantId(OID::gen());
    const auto otherTenantId = TenantId(OID::gen());
    IDLParserContext ctxt("root", false, tenantId);

    auto nsInfoStructBSON = [&](const char* ns) {
        BSONObjBuilder builder;
        builder.append("ns", ns);
        return builder.obj();
    };
    auto testDoc =
        BSONObjBuilder{}
            .append("CommandWithNamespaceStruct", 1)
            .append("field1", nsInfoStructBSON("db.coll1"))
            .append("$db", "admin")
            .append("field2", BSON_ARRAY(nsInfoStructBSON("a.b") << nsInfoStructBSON("c.d")))
            .obj();
    ASSERT_THROWS_CODE(
        CommandWithNamespaceStruct::parse(ctxt, makeOMRWithTenant(testDoc, otherTenantId)),
        DBException,
        8423379);
}

TEST(IDLTypeCommand, TestCommandWithBypassAndNamespaceMember_Parse) {
    for (bool multitenancySupport : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancySupport);
        for (bool featureFlag : {false, true}) {
            RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                       featureFlag);
            IDLParserContext ctxt("root");

            const char* ns1 = "db.coll1";
            const char* ns2 = "a.b";
            const char* ns3 = "c.d";
            auto nsInfoStructBSON = [&](const char* ns) {
                BSONObjBuilder builder;
                builder.append("ns", ns);
                return builder.obj();
            };
            auto bypassStructBSON = [&]() {
                BSONObjBuilder builder;
                builder.append("field1", nsInfoStructBSON(ns1));
                builder.append("field2",
                               BSON_ARRAY(nsInfoStructBSON(ns2) << nsInfoStructBSON(ns3)));
                return builder.obj();
            };

            auto testDoc = BSONObjBuilder{}
                               .append("CommandWithBypassAndNamespaceMember", 1)
                               .append("field1", bypassStructBSON())
                               .append("$db", "admin")
                               .obj();

            OpMsgRequest request;
            if (multitenancySupport) {
                const auto tenantId = TenantId(OID::gen());
                request = makeOMRWithTenant(testDoc, tenantId);
            } else
                request.body = testDoc;

            auto testStruct = CommandWithBypassAndNamespaceStruct::parse(ctxt, request);

            auto serializationContextCommand = testStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextCommand._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextCommand._callerType,
                          SerializationContext::CallerType::Request);

            auto bypassStruct = testStruct.getField1();
            auto serializationContextBypass = bypassStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextBypass._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextBypass._callerType,
                          SerializationContext::CallerType::Request);


            auto nsInfoStruct = bypassStruct.getField1();
            auto serializationContextNsInfo = nsInfoStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextNsInfo._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextNsInfo._callerType,
                          SerializationContext::CallerType::Request);


            auto nsInfoArray = bypassStruct.getField2();
            for (const auto& nsInfo : nsInfoArray) {
                auto serializationContextNsInfoArr = nsInfo.getSerializationContext();
                ASSERT_EQUALS(serializationContextNsInfoArr._source,
                              SerializationContext::Source::Command);
                ASSERT_EQUALS(serializationContextNsInfoArr._callerType,
                              SerializationContext::CallerType::Request);
            }
        }
    }
}

TEST(IDLTypeCommand, TestStructWithBypassAndNamespaceMember_Parse) {
    for (bool multitenancySupport : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancySupport);
        for (bool featureFlag : {false, true}) {
            RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                       featureFlag);
            boost::optional<TenantId> tenantId =
                multitenancySupport ? boost::make_optional(TenantId(OID::gen())) : boost::none;
            IDLParserContext ctxt("root", false, tenantId);

            const char* ns1 = "db.coll1";
            const char* ns2 = "a.b";
            const char* ns3 = "c.d";
            auto nsInfoStructBSON = [&](const char* ns) {
                BSONObjBuilder builder;
                builder.append("ns", ns);
                return builder.obj();
            };

            auto bypassStructBSON = [&]() {
                BSONObjBuilder builder;
                builder.append("field1", nsInfoStructBSON(ns1));
                builder.append("field2",
                               BSON_ARRAY(nsInfoStructBSON(ns2) << nsInfoStructBSON(ns3)));
                return builder.obj();
            };

            auto testDoc = bypassStructBSON();
            auto testStruct = BypassStruct::parse(ctxt, testDoc);

            auto serializationContextBypass = testStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextBypass._source,
                          SerializationContext::Source::Default);
            ASSERT_EQUALS(serializationContextBypass._callerType,
                          SerializationContext::CallerType::None);

            auto nsInfoStruct = testStruct.getField1();
            auto serializationContextNsInfo = nsInfoStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextNsInfo._source,
                          SerializationContext::Source::Default);
            ASSERT_EQUALS(serializationContextNsInfo._callerType,
                          SerializationContext::CallerType::None);

            auto nsInfoArray = testStruct.getField2();
            for (const auto& nsInfo : nsInfoArray) {
                auto serializationContextNsInfoArr = nsInfo.getSerializationContext();
                ASSERT_EQUALS(serializationContextNsInfoArr._source,
                              SerializationContext::Source::Default);
                ASSERT_EQUALS(serializationContextNsInfoArr._callerType,
                              SerializationContext::CallerType::None);
            }
        }
    }
}

TEST(IDLTypeCommand, TestStructWithBypassReplyAndNamespaceMember_Parse) {
    for (bool multitenancySupport : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancySupport);
        for (bool featureFlag : {false, true}) {
            RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                       featureFlag);
            boost::optional<TenantId> tenantId =
                multitenancySupport ? boost::make_optional(TenantId(OID::gen())) : boost::none;
            IDLParserContext ctxt("root", false, tenantId);

            const char* ns1 = "db.coll1";
            const char* ns2 = "a.b";
            const char* ns3 = "c.d";
            auto nsInfoStructBSON = [&](const char* ns) {
                BSONObjBuilder builder;
                builder.append("ns", ns);
                return builder.obj();
            };

            auto bypassReplyStructBSON = [&]() {
                BSONObjBuilder builder;
                builder.append("field1", nsInfoStructBSON(ns1));
                builder.append("field2",
                               BSON_ARRAY(nsInfoStructBSON(ns2) << nsInfoStructBSON(ns3)));
                return builder.obj();
            };

            auto testDoc = bypassReplyStructBSON();
            auto testStruct = BypassReplyStruct::parse(ctxt, testDoc);

            auto serializationContextBypass = testStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextBypass._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextBypass._callerType,
                          SerializationContext::CallerType::Reply);

            auto nsInfoStruct = testStruct.getField1();
            auto serializationContextNsInfo = nsInfoStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextNsInfo._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextNsInfo._callerType,
                          SerializationContext::CallerType::Reply);

            auto nsInfoArray = testStruct.getField2();
            for (const auto& nsInfo : nsInfoArray) {
                auto serializationContextNsInfoArr = nsInfo.getSerializationContext();
                ASSERT_EQUALS(serializationContextNsInfoArr._source,
                              SerializationContext::Source::Command);
                ASSERT_EQUALS(serializationContextNsInfoArr._callerType,
                              SerializationContext::CallerType::Reply);
            }
        }
    }
}

TEST(IDLTypeCommand, TestCommandWithBypassAndNamespaceMember_EmptyConstruct) {
    for (bool multitenancySupport : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancySupport);
        for (bool featureFlag : {false, true}) {
            RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                       featureFlag);

            auto testStruct = CommandWithBypassAndNamespaceStruct();

            auto serializationContextCommand = testStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextCommand._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextCommand._callerType,
                          SerializationContext::CallerType::Request);

            auto bypassStruct = testStruct.getField1();
            auto serializationContextBypass = bypassStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextBypass._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextBypass._callerType,
                          SerializationContext::CallerType::Request);

            auto nsInfoStruct = bypassStruct.getField1();
            auto serializationContextNsInfo = nsInfoStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextNsInfo._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextNsInfo._callerType,
                          SerializationContext::CallerType::Request);

            // the vector container is empty, which means that the SerializationContext obj's will
            // reflect whatever flags are passed into the array's construction at runtime rather
            // than being passed in from the enclosing class
            auto nsInfoArray = bypassStruct.getField2();
            ASSERT_EQUALS(nsInfoArray.size(), 0);
        }
    }
}

TEST(IDLTypeCommand, TestStructWithBypassAndNamespaceMember_EmptyConstruct) {
    for (bool multitenancySupport : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancySupport);
        for (bool featureFlag : {false, true}) {
            RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                       featureFlag);

            auto testStruct = BypassStruct();

            auto serializationContextBypass = testStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextBypass._source,
                          SerializationContext::Source::Default);
            ASSERT_EQUALS(serializationContextBypass._callerType,
                          SerializationContext::CallerType::None);

            auto nsInfoStruct = testStruct.getField1();
            auto serializationContextNsInfo = nsInfoStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextNsInfo._source,
                          SerializationContext::Source::Default);
            ASSERT_EQUALS(serializationContextNsInfo._callerType,
                          SerializationContext::CallerType::None);

            // the vector container is empty, which means that the SerializationContext obj's will
            // reflect whatever flags are passed into the array's construction at runtime rather
            // than being passed in from the enclosing class
            auto nsInfoArray = testStruct.getField2();
            ASSERT_EQUALS(nsInfoArray.size(), 0);
        }
    }
}

TEST(IDLTypeCommand, TestStructWithBypassReplyAndNamespaceMember_EmptyConstruct) {
    for (bool multitenancySupport : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancySupport);
        for (bool featureFlag : {false, true}) {
            RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                       featureFlag);

            auto testStruct = BypassReplyStruct();

            auto serializationContextBypass = testStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextBypass._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextBypass._callerType,
                          SerializationContext::CallerType::Reply);

            auto nsInfoStruct = testStruct.getField1();
            auto serializationContextNsInfo = nsInfoStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextNsInfo._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextNsInfo._callerType,
                          SerializationContext::CallerType::Reply);

            // the vector container is empty, which means that the SerializationContext obj's will
            // reflect whatever flags are passed into the array's construction at runtime rather
            // than being passed in from the enclosing class
            auto nsInfoArray = testStruct.getField2();
            ASSERT_EQUALS(nsInfoArray.size(), 0);
        }
    }
}

TEST(IDLTypeCommand, TestCommandWithBypassAndNamespaceMember_ConstructWithArgsNoCtxt) {
    for (bool multitenancySupport : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancySupport);
        for (bool featureFlag : {false, true}) {
            RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                       featureFlag);
            boost::optional<TenantId> tenantId =
                multitenancySupport ? boost::make_optional(TenantId(OID::gen())) : boost::none;

            const char* ns1 = "db.coll1";

            NamespaceInfoStruct nsArg(
                NamespaceString::createNamespaceString_forTest(tenantId, ns1));
            std::vector<NamespaceInfoStruct> nsArrayArg = {nsArg};
            BypassStruct bypassArg(nsArg, nsArrayArg);

            // by passing structures in as args, we are overriding any SerializationContext state
            // because the structs being passed in are std::move'd into the enclosing object
            auto testStruct = CommandWithBypassAndNamespaceStruct(bypassArg);

            auto serializationContextCommand = testStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextCommand._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextCommand._callerType,
                          SerializationContext::CallerType::Request);

            // bypassArg was NOT passed in any SerializationContext flags so its flags are the
            // default
            auto bypassStruct = testStruct.getField1();
            auto serializationContextBypass = bypassStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextBypass._source,
                          SerializationContext::Source::Default);
            ASSERT_EQUALS(serializationContextBypass._callerType,
                          SerializationContext::CallerType::None);

            // nsArg was NOT passed in any SerializationContext flags so its flags are the default
            auto nsInfoStruct = bypassStruct.getField1();
            auto serializationContextNsInfo = nsInfoStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextNsInfo._source,
                          SerializationContext::Source::Default);
            ASSERT_EQUALS(serializationContextNsInfo._callerType,
                          SerializationContext::CallerType::None);

            auto nsInfoArray = bypassStruct.getField2();
            for (const auto& nsInfo : nsInfoArray) {
                auto serializationContextNsInfoArr = nsInfo.getSerializationContext();
                ASSERT_EQUALS(serializationContextNsInfoArr._source,
                              SerializationContext::Source::Default);
                ASSERT_EQUALS(serializationContextNsInfoArr._callerType,
                              SerializationContext::CallerType::None);
            }
        }
    }
}

TEST(IDLTypeCommand, TestCommandWithBypassAndNamespaceMember_ConstructWithArgs) {
    for (bool multitenancySupport : {false, true}) {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport",
                                                                    multitenancySupport);
        for (bool featureFlag : {false, true}) {
            RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                       featureFlag);
            boost::optional<TenantId> tenantId =
                multitenancySupport ? boost::make_optional(TenantId(OID::gen())) : boost::none;

            const char* ns1 = "db.coll1";

            NamespaceInfoStruct nsArg(NamespaceString::createNamespaceString_forTest(tenantId, ns1),
                                      SerializationContext::stateCommandRequest());
            std::vector<NamespaceInfoStruct> nsArrayArg = {nsArg};
            BypassStruct bypassArg(nsArg, nsArrayArg);

            auto testStruct = CommandWithBypassAndNamespaceStruct(bypassArg);

            auto serializationContextCommand = testStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextCommand._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextCommand._callerType,
                          SerializationContext::CallerType::Request);

            // bypassArg was NOT passed in any SerializationContext flags so its flags are the
            // default
            auto bypassStruct = testStruct.getField1();
            auto serializationContextBypass = bypassStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextBypass._source,
                          SerializationContext::Source::Default);
            ASSERT_EQUALS(serializationContextBypass._callerType,
                          SerializationContext::CallerType::None);

            // ...but we can still get the correct SerializationContext state if the state is
            // manually passed into nested structs
            auto nsInfoStruct = bypassStruct.getField1();
            auto serializationContextNsInfo = nsInfoStruct.getSerializationContext();
            ASSERT_EQUALS(serializationContextNsInfo._source,
                          SerializationContext::Source::Command);
            ASSERT_EQUALS(serializationContextNsInfo._callerType,
                          SerializationContext::CallerType::Request);

            auto nsInfoArray = bypassStruct.getField2();
            for (const auto& nsInfo : nsInfoArray) {
                auto serializationContextNsInfoArr = nsInfo.getSerializationContext();
                ASSERT_EQUALS(serializationContextNsInfoArr._source,
                              SerializationContext::Source::Command);
                ASSERT_EQUALS(serializationContextNsInfoArr._callerType,
                              SerializationContext::CallerType::Request);
            }
        }
    }
}

TEST(IDLTypeCommand, TestCommandParseExpectPrefix_MissingExpectPrefix) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    IDLParserContext ctxt("root");

    const char* ns1 = "db.coll1";
    const char* ns2 = "a.b";
    const char* ns3 = "c.d";
    auto nsInfoStructBSON = [&](const char* ns) {
        BSONObjBuilder builder;
        builder.append("ns", ns);
        return builder.obj();
    };
    auto bypassStructBSON = [&]() {
        BSONObjBuilder builder;
        builder.append("field1", nsInfoStructBSON(ns1));
        builder.append("field2", BSON_ARRAY(nsInfoStructBSON(ns2) << nsInfoStructBSON(ns3)));
        return builder.obj();
    };

    auto testDoc = BSONObjBuilder{}
                       .append("CommandWithBypassAndNamespaceMember", 1)
                       .append("field1", bypassStructBSON())
                       .append("$db", "admin")
                       .obj();

    OpMsgRequest request;
    const auto tenantId = TenantId(OID::gen());
    request = makeOMRWithTenant(testDoc, tenantId);

    auto testStruct = CommandWithBypassAndNamespaceStruct::parse(ctxt, request);

    auto serializationContextCommand = testStruct.getSerializationContext();
    ASSERT_EQUALS(serializationContextCommand._prefixState, SerializationContext::Prefix::Default);

    auto bypassStruct = testStruct.getField1();
    auto serializationContextBypass = bypassStruct.getSerializationContext();
    ASSERT_EQUALS(serializationContextBypass._prefixState, SerializationContext::Prefix::Default);

    auto nsInfoStruct = bypassStruct.getField1();
    auto serializationContextNsInfo = nsInfoStruct.getSerializationContext();
    ASSERT_EQUALS(serializationContextNsInfo._prefixState, SerializationContext::Prefix::Default);

    auto nsInfoArray = bypassStruct.getField2();
    for (const auto& nsInfo : nsInfoArray) {
        auto serializationContextNsInfoArr = nsInfo.getSerializationContext();
        ASSERT_EQUALS(serializationContextNsInfoArr._prefixState,
                      SerializationContext::Prefix::Default);
    }
}

TEST(IDLTypeCommand, TestCommandParseExpectPrefix) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    for (bool expectPrefix : {false, true}) {
        IDLParserContext ctxt("root");

        const char* ns1 = "db.coll1";
        const char* ns2 = "a.b";
        const char* ns3 = "c.d";
        auto nsInfoStructBSON = [&](const char* ns) {
            BSONObjBuilder builder;
            builder.append("ns", ns);
            return builder.obj();
        };
        auto bypassStructBSON = [&]() {
            BSONObjBuilder builder;
            builder.append("field1", nsInfoStructBSON(ns1));
            builder.append("field2", BSON_ARRAY(nsInfoStructBSON(ns2) << nsInfoStructBSON(ns3)));
            return builder.obj();
        };

        auto testDoc = BSONObjBuilder{}
                           .append("CommandWithBypassAndNamespaceMember", 1)
                           .append("field1", bypassStructBSON())
                           .append("expectPrefix", expectPrefix)
                           .append("$db", "admin")
                           .obj();

        OpMsgRequest request;
        const auto tenantId = TenantId(OID::gen());
        request = makeOMRWithTenant(testDoc, tenantId);

        auto testStruct = CommandWithBypassAndNamespaceStruct::parse(ctxt, request);

        auto serializationContextCommand = testStruct.getSerializationContext();
        ASSERT_EQUALS(serializationContextCommand._prefixState,
                      expectPrefix ? SerializationContext::Prefix::IncludePrefix
                                   : SerializationContext::Prefix::ExcludePrefix);

        auto bypassStruct = testStruct.getField1();
        auto serializationContextBypass = bypassStruct.getSerializationContext();
        ASSERT_EQUALS(serializationContextBypass._prefixState,
                      expectPrefix ? SerializationContext::Prefix::IncludePrefix
                                   : SerializationContext::Prefix::ExcludePrefix);

        auto nsInfoStruct = bypassStruct.getField1();
        auto serializationContextNsInfo = nsInfoStruct.getSerializationContext();
        ASSERT_EQUALS(serializationContextNsInfo._prefixState,
                      expectPrefix ? SerializationContext::Prefix::IncludePrefix
                                   : SerializationContext::Prefix::ExcludePrefix);

        auto nsInfoArray = bypassStruct.getField2();
        for (const auto& nsInfo : nsInfoArray) {
            auto serializationContextNsInfoArr = nsInfo.getSerializationContext();
            ASSERT_EQUALS(serializationContextNsInfoArr._prefixState,
                          expectPrefix ? SerializationContext::Prefix::IncludePrefix
                                       : SerializationContext::Prefix::ExcludePrefix);
        }
    }
}

TEST(IDLTypeCommand, TestCommandParseDuplicateExpectPrefix) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    for (bool featureFlag : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   featureFlag);
        IDLParserContext ctxt("root");

        const char* ns1 = "db.coll1";
        const char* ns2 = "a.b";
        const char* ns3 = "c.d";
        auto nsInfoStructBSON = [&](const char* ns) {
            BSONObjBuilder builder;
            builder.append("ns", ns);
            return builder.obj();
        };
        auto bypassStructBSON = [&]() {
            BSONObjBuilder builder;
            builder.append("field1", nsInfoStructBSON(ns1));
            builder.append("field2", BSON_ARRAY(nsInfoStructBSON(ns2) << nsInfoStructBSON(ns3)));
            return builder.obj();
        };

        auto testDoc = BSONObjBuilder{}
                           .append("CommandWithBypassAndNamespaceMember", 1)
                           .append("field1", bypassStructBSON())
                           .append("expectPrefix", true)
                           .append("expectPrefix", false)
                           .append("$db", "admin")
                           .obj();

        OpMsgRequest request;
        const auto tenantId = TenantId(OID::gen());
        request = makeOMRWithTenant(testDoc, tenantId);

        ASSERT_THROWS(CommandWithBypassAndNamespaceStruct::parse(ctxt, request),
                      AssertionException);
    }
}

void verifyContract(const AuthorizationContract& left, const AuthorizationContract& right) {
    ASSERT_TRUE(left.contains(right));
    ASSERT_TRUE(right.contains(left));
}

TEST(IDLAccessCheck, TestNone) {
    AuthorizationContract empty;

    verifyContract(empty, AccessCheckNone::kAuthorizationContract);
}

TEST(IDLAccessCheck, TestSimpleAccessCheck) {
    AuthorizationContract ac;
    ac.addAccessCheck(AccessCheckEnum::kIsAuthenticated);

    verifyContract(ac, AccessCheckSimpleAccessCheck::kAuthorizationContract);
}

TEST(IDLAccessCheck, TestSimplePrivilegeAccessCheck) {
    AuthorizationContract ac;
    ac.addPrivilege(Privilege(ResourcePattern::forClusterResource(), ActionType::addShard));
    ac.addPrivilege(Privilege(ResourcePattern::forClusterResource(), ActionType::serverStatus));

    verifyContract(ac, AccessCheckSimplePrivilege::kAuthorizationContract);
}

TEST(IDLAccessCheck, TestComplexAccessCheck) {
    AuthorizationContract ac;
    ac.addPrivilege(Privilege(ResourcePattern::forClusterResource(), ActionType::addShard));
    ac.addPrivilege(Privilege(ResourcePattern::forClusterResource(), ActionType::serverStatus));

    ac.addPrivilege(Privilege(ResourcePattern::forDatabaseName("test"), ActionType::trafficRecord));

    ac.addPrivilege(Privilege(ResourcePattern::forAnyResource(), ActionType::splitVector));

    ac.addAccessCheck(AccessCheckEnum::kIsAuthenticated);
    ac.addAccessCheck(AccessCheckEnum::kIsAuthorizedToParseNamespaceElement);

    verifyContract(ac, AccessCheckComplexPrivilege::kAuthorizationContract);
}

TEST(IDLFieldTests, TestOptionalBoolField) {
    IDLParserContext ctxt("root");

    {
        auto testDoc = BSON("optBoolField" << true);
        auto parsed = OptionalBool::parseFromBSON(testDoc.firstElement());
        ASSERT_TRUE(parsed.has_value());
        ASSERT_TRUE(parsed);
        BSONObjBuilder serialized;
        parsed.serializeToBSON("optBoolField", &serialized);
        ASSERT_BSONOBJ_EQ(serialized.obj(), testDoc);
    }

    {
        auto testDoc = BSON("optBoolField" << false);
        auto parsed = OptionalBool::parseFromBSON(testDoc.firstElement());
        ASSERT_TRUE(parsed.has_value());
        ASSERT_FALSE(parsed);
        BSONObjBuilder serialized;
        parsed.serializeToBSON("optBoolField", &serialized);
        ASSERT_BSONOBJ_EQ(serialized.obj(), testDoc);
    }

    {
        auto testDoc = BSONObj();
        auto parsed = OptionalBool::parseFromBSON(testDoc.firstElement());
        ASSERT_FALSE(parsed.has_value());
        ASSERT_FALSE(parsed);
        BSONObjBuilder serialized;
        parsed.serializeToBSON("", &serialized);
        ASSERT_BSONOBJ_EQ(serialized.obj(), testDoc);
    }

    {
        auto testDoc = BSON("optBoolField" << jstNULL);
        ASSERT_THROWS(OptionalBool::parseFromBSON(testDoc.firstElement()), AssertionException);
    }

    {
        auto testDoc = BSON("optBoolField" << BSONUndefined);
        ASSERT_THROWS(OptionalBool::parseFromBSON(testDoc.firstElement()), AssertionException);
    }

    {
        auto testDoc = BSON("optBoolField"
                            << "abc");
        ASSERT_THROWS(OptionalBool::parseFromBSON(testDoc.firstElement()), AssertionException);
    }
}

TEST(IDLFieldTests, TenantOverrideField) {
    const auto mkdoc = [](boost::optional<TenantId> tenantId) {
        BSONObjBuilder doc;
        doc.append("BasicIgnoredCommand", 1);
        doc.append("$db", "admin");
        if (tenantId) {
            tenantId->serializeToBSON("$tenant", &doc);
        }
        doc.append("field1", 42);
        doc.append("field2", "foo");
        return doc.obj();
    };

    // Test optionality of $tenant arg.
    {
        auto obj = BasicIgnoredCommand::parse(IDLParserContext{"nil"}, mkdoc(boost::none));
        auto tenant = obj.getDollarTenant();
        ASSERT(tenant == boost::none);
    }

    // Test passing an tenant id (acting on behalf of a specific tenant)
    {
        auto id = TenantId(OID::gen());
        auto obj = BasicIgnoredCommand::parse(IDLParserContext{"oid"}, mkdoc(id));
        auto tenant = obj.getDollarTenant();
        ASSERT(tenant == id);
    }
}

TEST(IDLFieldTests, TenantOverrideFieldWithInvalidValue) {
    const auto mkdoc = [](auto tenantId) {
        BSONObjBuilder doc;
        doc.append("BasicIgnoredCommand", 1);
        doc.append("$db", "admin");
        doc.append("$tenant", tenantId);
        doc.append("field1", 42);
        doc.append("field2", "foo");
        return doc.obj();
    };

    // Negative: Parse invalid types.
    {
        ASSERT_THROWS(BasicIgnoredCommand::parse(IDLParserContext{"int"}, mkdoc(123)), DBException);
        ASSERT_THROWS(BasicIgnoredCommand::parse(IDLParserContext{"float"}, mkdoc(3.14)),
                      DBException);
        ASSERT_THROWS(BasicIgnoredCommand::parse(IDLParserContext{"string"}, mkdoc("bar")),
                      DBException);
        ASSERT_THROWS(BasicIgnoredCommand::parse(IDLParserContext{"object"}, mkdoc(BSONObj())),
                      DBException);
    }
}

TEST(IDLOwnershipTests, ParseOwnAssumesOwnership) {
    IDLParserContext ctxt("root");
    One_plain_object idlStruct;
    {
        auto tmp = BSON("value" << BSON("x" << 42));
        idlStruct = One_plain_object::parseOwned(ctxt, std::move(tmp));
    }
    // Now that tmp is out of scope, if idlStruct didn't retain ownership, it would be accessing
    // free'd memory which should error on ASAN and debug builds.
    auto obj = idlStruct.getValue();
    ASSERT_BSONOBJ_EQ(obj, BSON("x" << 42));
}

TEST(IDLOwnershipTests, ParseSharingOwnershipTmpBSON) {
    IDLParserContext ctxt("root");
    One_plain_object idlStruct;
    {
        auto tmp = BSON("value" << BSON("x" << 42));
        idlStruct = One_plain_object::parseSharingOwnership(ctxt, tmp);
    }
    // Now that tmp is out of scope, if idlStruct didn't particpate in ownership, it would be
    // accessing free'd memory which should error on ASAN and debug builds.
    auto obj = idlStruct.getValue();
    ASSERT_BSONOBJ_EQ(obj, BSON("x" << 42));
}

TEST(IDLOwnershipTests, ParseSharingOwnershipTmpIDLStruct) {
    IDLParserContext ctxt("root");
    auto bson = BSON("value" << BSON("x" << 42));
    { auto idlStruct = One_plain_object::parseSharingOwnership(ctxt, bson); }
    // Now that idlStruct is out of scope, if bson didn't particpate in ownership, it would be
    // accessing free'd memory which should error on ASAN and debug builds.
    ASSERT_BSONOBJ_EQ(bson["value"].Obj(), BSON("x" << 42));
}
}  // namespace
}  // namespace mongo
