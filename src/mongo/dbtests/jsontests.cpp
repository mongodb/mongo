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

/**
 * Tests for json.{h,cpp} code and BSONObj::jsonString()
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <fmt/format.h>
#include <fmt/printf.h>
#include <limits>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace {

using B = BSONObjBuilder;
using Arr = BSONArrayBuilder;

// Tests of the BSONObj::jsonString member function.
namespace JsonStringTests {

void checkJsonString(const BSONObj& bson, const std::string& json) {
    ASSERT_EQUALS(bson.jsonString(Strict), json);
}

void checkJsonStringEach(const std::vector<std::pair<BSONObj, std::string>>& pairs) {
    for (const auto& pair : pairs) {
        checkJsonString(pair.first, pair.second);
    }
}

TEST(JsonStringTest, BasicTest) {
    checkJsonStringEach({
        {B().obj(), "{}"},                                 // Empty
        {B().append("a", "b").obj(), R"({ "a" : "b" })"},  // SingleStringMember
        {B().append("a", "\" \\ / \b \f \n \r \t").obj(),
         R"({ "a" : "\" \\ / \b \f \n \r \t" })"},  // EscapedCharacters
        // per http://www.ietf.org/rfc/rfc4627.txt, control characters are
        // (U+0000 through U+001F).  U+007F is not mentioned as a control character.
        {B().append("a", "\x1 \x1f").obj(),
         R"({ "a" : "\u0001 \u001f" })"},                         // AdditionalControlCharacters
        {B().append("a", "\x80").obj(), "{ \"a\" : \"\x80\" }"},  // ExtendedAscii
        {B().append("\t", "b").obj(), R"({ "\t" : "b" })"},       // EscapeFieldName
        {B().append("a", 1).obj(), R"({ "a" : 1 })"},             // SingleIntMember
    });
}

TEST(JsonStringTest, NumbersTest) {
    const double qNaN = std::numeric_limits<double>::quiet_NaN();
    const double sNaN = std::numeric_limits<double>::signaling_NaN();
    // Note there is no NaN in the JSON RFC but what would be the alternative?
    ASSERT(str::contains(B().append("a", qNaN).obj().jsonString(Strict), "NaN"));
    ASSERT(str::contains(B().append("a", sNaN).obj().jsonString(Strict), "NaN"));

    checkJsonStringEach({
        {B().append("a", 1.5).obj(), R"({ "a" : 1.5 })"},              // SingleNumberMember
        {B().append("a", 123456789).obj(), R"({ "a" : 123456789 })"},  // NumberPrecision
        {B().append("a", -1).obj(), R"({ "a" : -1 })"},                // NegativeNumber
    });
}

TEST(JsonStringTest, NumberLongStrictZero) {
    BSONObjBuilder b;
    b.append("a", 0LL);
    ASSERT_EQUALS("{ \"a\" : { \"$numberLong\" : \"0\" } }", b.done().jsonString(Strict));
}

TEST(JsonStringTest, NumberLongStrict) {
    BSONObjBuilder b;
    b.append("a", 20000LL);
    ASSERT_EQUALS("{ \"a\" : { \"$numberLong\" : \"20000\" } }", b.done().jsonString(Strict));
}

// Test a NumberLong that is too big to fit into a 32 bit integer
TEST(JsonStringTest, NumberLongStrictLarge) {
    BSONObjBuilder b;
    b.append("a", 9223372036854775807LL);
    ASSERT_EQUALS("{ \"a\" : { \"$numberLong\" : \"9223372036854775807\" } }",
                  b.done().jsonString(Strict));
}

TEST(JsonStringTest, NumberLongStrictNegative) {
    BSONObjBuilder b;
    b.append("a", -20000LL);
    ASSERT_EQUALS("{ \"a\" : { \"$numberLong\" : \"-20000\" } }", b.done().jsonString(Strict));
}

TEST(JsonStringTest, NumberDecimal) {
    BSONObjBuilder b;
    b.append("a", mongo::Decimal128("123456789.12345"));
    ASSERT_EQUALS("{ \"a\" : NumberDecimal(\"123456789.12345\") }", b.done().jsonString(TenGen));
}

TEST(JsonStringTest, NumberDecimalStrict) {
    BSONObjBuilder b;
    b.append("a", mongo::Decimal128("123456789.12345"));
    ASSERT_EQUALS("{ \"a\" : { \"$numberDecimal\" : \"123456789.12345\" } }",
                  b.done().jsonString(Strict));
}

TEST(JsonStringTest, NumberDoubleNaN) {
    BSONObjBuilder b;
    b.append("a", std::numeric_limits<double>::quiet_NaN());
    ASSERT_EQUALS("{ \"a\" : NaN }", b.done().jsonString(Strict));
}

TEST(JsonStringTest, NumberDoubleInfinity) {
    BSONObjBuilder b;
    b.append("a", std::numeric_limits<double>::infinity());
    ASSERT_EQUALS("{ \"a\" : Infinity }", b.done().jsonString(Strict));
}

TEST(JsonStringTest, NumberDoubleNegativeInfinity) {
    BSONObjBuilder b;
    b.append("a", -std::numeric_limits<double>::infinity());
    ASSERT_EQUALS("{ \"a\" : -Infinity }", b.done().jsonString(Strict));
}

TEST(JsonStringTest, SingleBoolMember) {
    ASSERT_EQUALS(R"({ "a" : true })", B().appendBool("a", true).obj().jsonString(Strict));
    ASSERT_EQUALS(R"({ "a" : false })", B().appendBool("a", false).obj().jsonString(Strict));
}

TEST(JsonStringTest, SingleNullMember) {
    BSONObjBuilder b;
    b.appendNull("a");
    ASSERT_EQUALS("{ \"a\" : null }", b.done().jsonString(Strict));
}

TEST(JsonStringTest, SingleUndefinedMember) {
    BSONObjBuilder b;
    b.appendUndefined("a");
    ASSERT_EQUALS("{ \"a\" : { \"$undefined\" : true } }", b.done().jsonString(Strict));
    ASSERT_EQUALS("{ \"a\" : undefined }", b.done().jsonString(TenGen));
}

TEST(JsonStringTest, SingleObjectMember) {
    BSONObjBuilder b, c;
    b.append("a", c.done());
    ASSERT_EQUALS("{ \"a\" : {} }", b.done().jsonString(Strict));
}

TEST(JsonStringTest, TwoMembers) {
    BSONObjBuilder b;
    b.append("a", 1);
    b.append("b", 2);
    ASSERT_EQUALS("{ \"a\" : 1, \"b\" : 2 }", b.done().jsonString(Strict));
}

TEST(JsonStringTest, EmptyArray) {
    std::vector<int> arr;
    BSONObjBuilder b;
    b.append("a", arr);
    ASSERT_EQUALS("{ \"a\" : [] }", b.done().jsonString(Strict));
}

TEST(JsonStringTest, Array) {
    std::vector<int> arr;
    arr.push_back(1);
    arr.push_back(2);
    BSONObjBuilder b;
    b.append("a", arr);
    ASSERT_EQUALS("{ \"a\" : [ 1, 2 ] }", b.done().jsonString(Strict));
}

TEST(JsonStringTest, DBRef) {
    char OIDbytes[OID::kOIDSize];
    memset(&OIDbytes, 0xff, OID::kOIDSize);
    OID oid = OID::from(OIDbytes);
    BSONObjBuilder b;
    b.appendDBRef("a", "namespace", oid);
    BSONObj built = b.done();
    ASSERT_EQUALS(
        "{ \"a\" : { \"$ref\" : \"namespace\", \"$id\" : \"ffffffffffffffffffffffff\" } }",
        built.jsonString(Strict));
    ASSERT_EQUALS("{ \"a\" : Dbref( \"namespace\", \"ffffffffffffffffffffffff\" ) }",
                  built.jsonString(TenGen));
}

TEST(JsonStringTest, DBRefZero) {
    char OIDbytes[OID::kOIDSize];
    memset(&OIDbytes, 0, OID::kOIDSize);
    OID oid = OID::from(OIDbytes);
    BSONObjBuilder b;
    b.appendDBRef("a", "namespace", oid);
    ASSERT_EQUALS(
        "{ \"a\" : { \"$ref\" : \"namespace\", \"$id\" : \"000000000000000000000000\" } }",
        b.done().jsonString(Strict));
}

TEST(JsonStringTest, ObjectId) {
    char OIDbytes[OID::kOIDSize];
    memset(&OIDbytes, 0xff, OID::kOIDSize);
    OID oid = OID::from(OIDbytes);
    BSONObjBuilder b;
    b.appendOID("a", &oid);
    BSONObj built = b.done();
    ASSERT_EQUALS("{ \"a\" : { \"$oid\" : \"ffffffffffffffffffffffff\" } }",
                  built.jsonString(Strict));
    ASSERT_EQUALS("{ \"a\" : ObjectId( \"ffffffffffffffffffffffff\" ) }", built.jsonString(TenGen));
}

TEST(JsonStringTest, BinData) {
    char z[3];
    z[0] = 'a';
    z[1] = 'b';
    z[2] = 'c';
    BSONObjBuilder b;
    b.appendBinData("a", 3, BinDataGeneral, z);

    std::string o = b.done().jsonString(Strict);

    ASSERT_EQUALS("{ \"a\" : { \"$binary\" : \"YWJj\", \"$type\" : \"00\" } }", o);

    BSONObjBuilder c;
    c.appendBinData("a", 2, BinDataGeneral, z);
    ASSERT_EQUALS("{ \"a\" : { \"$binary\" : \"YWI=\", \"$type\" : \"00\" } }",
                  c.done().jsonString(Strict));

    BSONObjBuilder d;
    d.appendBinData("a", 1, BinDataGeneral, z);
    ASSERT_EQUALS("{ \"a\" : { \"$binary\" : \"YQ==\", \"$type\" : \"00\" } }",
                  d.done().jsonString(Strict));
}

TEST(JsonStringTest, Symbol) {
    BSONObjBuilder b;
    b.appendSymbol("a", "b");
    ASSERT_EQUALS("{ \"a\" : \"b\" }", b.done().jsonString(Strict));
}

#ifdef _WIN32
char tzEnvString[] = "TZ=EST+5EDT";
#else
char tzEnvString[] = "TZ=America/New_York";
#endif

class TimeZoneGuard {
public:
    TimeZoneGuard() {
        char* _oldTimezonePtr = getenv("TZ");
        _oldTimezone = std::string(_oldTimezonePtr ? _oldTimezonePtr : "");
        if (-1 == putenv(tzEnvString)) {
            FAIL(errnoWithDescription());
        }
        tzset();
    }
    ~TimeZoneGuard() {
        if (!_oldTimezone.empty()) {
#ifdef _WIN32
            errno_t ret = _putenv_s("TZ", _oldTimezone.c_str());
            if (0 != ret) {
                StringBuilder sb;
                sb << "Error setting TZ environment variable to:  " << _oldTimezone
                   << ".  Error code:  " << ret;
                FAIL(sb.str());
            }
#else
            if (-1 == setenv("TZ", _oldTimezone.c_str(), 1)) {
                FAIL(errnoWithDescription());
            }
#endif
        } else {
#ifdef _WIN32
            errno_t ret = _putenv_s("TZ", "");
            if (0 != ret) {
                StringBuilder sb;
                sb << "Error unsetting TZ environment variable.  Error code:  " << ret;
                FAIL(sb.str());
            }
#else
            if (-1 == unsetenv("TZ")) {
                FAIL(errnoWithDescription());
            }
#endif
        }
        tzset();
    }

private:
    std::string _oldTimezone;
};

TEST(JsonStringTest, Date) {
    TimeZoneGuard tzGuard;
    BSONObjBuilder b;
    b.appendDate("a", Date_t());
    BSONObj built = b.done();
    ASSERT_EQUALS("{ \"a\" : { \"$date\" : \"1969-12-31T19:00:00.000-0500\" } }",
                  built.jsonString(Strict));
    ASSERT_EQUALS("{ \"a\" : Date( 0 ) }", built.jsonString(TenGen));

    // Test dates above our maximum formattable date.  See SERVER-13760.
    BSONObjBuilder b2;
    b2.appendDate("a", Date_t::fromMillisSinceEpoch(32535262800000LL));
    BSONObj built2 = b2.done();
    ASSERT_EQUALS("{ \"a\" : { \"$date\" : { \"$numberLong\" : \"32535262800000\" } } }",
                  built2.jsonString(Strict));
}

TEST(JsonStringTest, DateNegative) {
    BSONObjBuilder b;
    b.appendDate("a", Date_t::fromMillisSinceEpoch(-1));
    BSONObj built = b.done();
    ASSERT_EQUALS("{ \"a\" : { \"$date\" : { \"$numberLong\" : \"-1\" } } }",
                  built.jsonString(Strict));
    ASSERT_EQUALS("{ \"a\" : Date( -1 ) }", built.jsonString(TenGen));
}

TEST(JsonStringTest, Regex) {
    BSONObj built = B().appendRegex("a", "abc", "i").obj();
    ASSERT_EQUALS(R"({ "a" : { "$regex" : "abc", "$options" : "i" } })", built.jsonString(Strict));
    ASSERT_EQUALS(R"({ "a" : /abc/i })", built.jsonString(TenGen));
}

TEST(JsonStringTest, RegexEscape) {
    BSONObjBuilder b;
    b.appendRegex("a", "/\"", "i");
    BSONObj built = b.done();
    ASSERT_EQUALS("{ \"a\" : { \"$regex\" : \"/\\\"\", \"$options\" : \"i\" } }",
                  built.jsonString(Strict));
    ASSERT_EQUALS("{ \"a\" : /\\/\\\"/i }", built.jsonString(TenGen));
}

TEST(JsonStringTest, RegexManyOptions) {
    BSONObjBuilder b;
    b.appendRegex("a", "z", "abcgimx");
    BSONObj built = b.done();
    ASSERT_EQUALS("{ \"a\" : { \"$regex\" : \"z\", \"$options\" : \"abcgimx\" } }",
                  built.jsonString(Strict));
    ASSERT_EQUALS("{ \"a\" : /z/gim }", built.jsonString(TenGen));
}

TEST(JsonStringTest, RegexValidOption) {
    BSONObj built = B().appendRegex("a", "sometext", "ms").obj();
    ASSERT_EQUALS(R"({ "a" : { "$regex" : "sometext", "$options" : "ms" } })",
                  built.jsonString(Strict));
    ASSERT_EQUALS(R"({ "a" : /sometext/ms })", built.jsonString(TenGen));
}

TEST(JsonStringTest, CodeTests) {
    BSONObjBuilder b;
    b.appendCode("x", "function(arg){ var string = \"\\n\"; return 1; }");
    BSONObj o = b.obj();
    ASSERT_EQUALS(
        "{ \"x\" : \"function(arg){ var string = \\\"\\\\n\\\"; "
        "return 1; }\" }",
        o.jsonString());
}

TEST(JsonStringTest, CodeWScopeTests) {
    BSONObjBuilder b;
    b.appendCodeWScope("x", "function(arg){ var string = \"\\n\"; return x; }", BSON("x" << 1));
    BSONObj o = b.obj();
    ASSERT_EQUALS(
        "{ \"x\" : "
        "{ \"$code\" : "
        "\"function(arg){ var string = \\\"\\\\n\\\"; return x; }\" , "
        "\"$scope\" : { \"x\" : 1 } } }",
        o.jsonString());
}

TEST(JsonStringTest, TimestampTests) {
    BSONObjBuilder b;
    b.append("x", Timestamp(4, 10));
    BSONObj o = b.obj();
    ASSERT_EQUALS("{ \"x\" : { \"$timestamp\" : { \"t\" : 4, \"i\" : 10 } } }",
                  o.jsonString(Strict));
    ASSERT_EQUALS("{ \"x\" : Timestamp( 4, 10 ) }", o.jsonString(TenGen));
}

TEST(JsonStringTest, NullString) {
    BSONObjBuilder b;
    b.append("x", "a\0b", 4);
    BSONObj o = b.obj();
    ASSERT_EQUALS("{ \"x\" : \"a\\u0000b\" }", o.jsonString());
}

TEST(JsonStringTest, AllTypesTest) {
    OID oid;
    oid.init();

    BSONObjBuilder b;
    b.appendMinKey("a");
    b.append("b", 5.5);
    b.append("c", "abc");
    b.append("e", BSON("x" << 1));
    b.append("f", BSON_ARRAY(1 << 2 << 3));
    b.appendBinData("g", sizeof(*this), bdtCustom, (const void*)this);
    b.appendUndefined("h");
    b.append("i", oid);
    b.appendBool("j", 1);
    b.appendDate("k", Date_t::fromMillisSinceEpoch(123));
    b.appendNull("l");
    b.appendRegex("m", "a");
    b.appendDBRef("n", "foo", oid);
    b.appendCode("o", "function(){}");
    b.appendSymbol("p", "foo");
    b.appendCodeWScope("q", "function(){}", BSON("x" << 1));
    b.append("r", (int)5);
    b.appendTimestamp("s", 123123123123123LL);
    b.append("t", 12321312312LL);
    b.append("u", "123456789.12345");
    b.appendMaxKey("v");

    BSONObj o = b.obj();
    o.jsonString();
}

}  // namespace JsonStringTests

namespace FromJsonTests {

void assertEquals(const std::string& json,
                  const BSONObj& expected,
                  const BSONObj& actual,
                  const char* msg) {
    const bool bad = expected.woCompare(actual);
    if (bad) {
        ::mongo::log() << "want:" << expected.jsonString() << " size: " << expected.objsize()
                       << std::endl;
        ::mongo::log() << "got :" << actual.jsonString() << " size: " << actual.objsize()
                       << std::endl;
        ::mongo::log() << expected.hexDump() << std::endl;
        ::mongo::log() << actual.hexDump() << std::endl;
        ::mongo::log() << msg << std::endl;
        ::mongo::log() << "orig json:" << json;
    }
    ASSERT(!bad);
}

void checkEquivalence(const std::string& json, const BSONObj& bson) {
    ASSERT(fromjson(json).valid(BSONVersion::kLatest));
    assertEquals(json, bson, fromjson(json), "mode: json-to-bson");
    assertEquals(json, bson, fromjson(tojson(bson)), "mode: <default>");
    assertEquals(json, bson, fromjson(tojson(bson, Strict)), "mode: strict");
    assertEquals(json, bson, fromjson(tojson(bson, TenGen)), "mode: tengen");
}

void checkRejection(const std::string& json) {
    ASSERT_THROWS(fromjson(json), AssertionException);
}

void checkEquivalenceEach(const std::vector<std::pair<std::string, BSONObj>>& sequence) {
    for (const auto& equiv : sequence) {
        checkEquivalence(equiv.first, equiv.second);
    }
}

void checkRejectionEach(const std::vector<std::string>& sequence) {
    for (const std::string& json : sequence) {
        checkRejection(json);
    }
}

TEST(FromJsonTest, Parsing) {
    checkEquivalenceEach({
        {"{}", B().obj()},
        {"{ }", B().obj()},
        {R"({ "a" : "b" })", B().append("a", "b").obj()},
        {R"({ "" : "" })", B().append("", "").obj()},
        {R"({ "$where" : 1 })", B().append("$where", 1).obj()},          // OkDollarFieldName
        {R"({ "a" : 1 })", B().append("a", 1).obj()},                    // SingleNumber
        {R"({ "a" : 0.7 })", B().append("a", 0.7).obj()},                // RealNumber
        {R"({ "a" : -4.4433e-2 })", B().append("a", -4.4433e-2).obj()},  // FancyNumber
        {R"({ "a" : 1, "b" : "foo" })", B().append("a", 1).append("b", "foo").obj()},  // 2Elem
        {R"({ "z" : { "a" : 1 } })", B().append("z", B().append("a", 1).obj()).obj()}  // Sub
    });
    checkRejectionEach({
        R"({ "$oid" : "b" })",
        R"({ "$ref" : "b" })",
        R"({ 0 : "b" })",
        R"({ test.test : "b" })",
        R"({ \"nc\0nc\" : \"b\" })",
        R"({ a : })",
        R"({ a : a })",
    });
}

TEST(FromJsonTest, DeeplyNestedObject) {
    std::string json = R"({"0":true})";
    BSONObj bson = B().append("0", true).obj();
    for (int depth = 35; depth-- > 0;) {
        json = fmt::sprintf(R"({"%d":%s})", depth, json);
        bson = B().append(std::to_string(depth), bson).obj();
    }
    checkEquivalence(json, bson);
}

TEST(FromJsonTest, ArrayTest) {
    checkEquivalenceEach({
        {R"({ "a" : [] })", B().append("a", std::vector<int>()).obj()},  // ArrayEmpty
        {"[]", BSONArray()},                                             // TopLevelArrayEmpty
        {R"([ 123, "abc" ])", Arr().append(123).append("abc").arr()},    // TopLevelArray
        {R"({ "a" : [ 1, 2, 3 ] })", B().append("a", std::vector<int>{1, 2, 3}).obj()},
    });
}

TEST(FromJsonTest, SpecialValuesTest) {
    checkEquivalenceEach({
        {R"({ "a" : true })", B().appendBool("a", true).obj()},
        {R"({ "a" : false })", B().appendBool("a", false).obj()},
        {R"({ "a" : null })", B().appendNull("a").obj()},
        {R"({ "a" : undefined })", B().appendUndefined("a").obj()},
        {R"({ "a" : { "$undefined" : true } })", B().appendUndefined("a").obj()},
    });
    checkRejectionEach({
        R"({ "a" : { "$undefined" : false } })",
    });
}

TEST(FromJsonTest, EscapedCharacters) {
    checkEquivalenceEach({
        {R"({ "a" : "\" \\ \/ \b \f \n \r \t \v" })",
         B().append("a", "\" \\ / \b \f \n \r \t \v").obj()},  // EscapedCharacters
        {R"({ "a" : "\% \{ \a \z \$ \# \' \ " })",
         B().append("a", "% { a z $ # '  ").obj()},               // NonEscapedCharacters
        {"{ \"a\" : \"\x7f\" }", B().append("a", "\x7f").obj()},  // AllowedControlCharacter
    });
    checkRejectionEach({
        "{ \"a\" : \"\x1f\" }",  // InvalidControlCharacter
    });
}

TEST(FromJsonTest, FieldNameTest) {
    checkEquivalenceEach({
        {R"({ b1 : "b" })", B().append("b1", "b").obj()},    // NumbersInFieldName
        {R"({ "\n" : "b" })", B().append("\n", "b").obj()},  // EscapeFieldName
    });
}

TEST(FromJsonTest, Utf8Test) {
    using namespace std::literals::string_literals;
    const std::string u = "\xea\x80\x80\xea\x80\x80"s;
    BSONObj built = B().append("a", u).obj();
    ASSERT_EQUALS(built.firstElement().valuestr(), u);

    checkEquivalenceEach({
        // EscapedUnicodeToUtf8
        {R"({ "a" : "\ua000\uA000" })", built},

        // Utf8AllOnes
        {R"({ "a" : "\u0001\u007f\u07ff\uffff" })",
         B().append("a", "\x01\x7f\xdf\xbf\xef\xbf\xbf").obj()},

        // Utf8FirstByteOnes
        {R"({ "a" : "\u0700\uff00" })", B().append("a", "\xdc\x80\xef\xbc\x80").obj()},

    });
    checkRejectionEach({
        R"({ "a" : "\u0ZZZ" })",  // Utf8Invalid
        R"({ "a" : "\u000" })",   // Utf8TooShort
    });
}

TEST(FromJsonTest, DBRefTest) {
    checkEquivalenceEach({
        // Constructor
        {R"({ "a" : Dbref( "ns", "000000000000000000000000" ) })",
         B().append("a", B().append("$ref", "ns").append("$id", "000000000000000000000000").obj())
             .obj()},
        // ConstructorCapitals
        {R"({ "a" : DBRef( "ns", "000000000000000000000000" ) })",
         B().append("a", B().append("$ref", "ns").append("$id", "000000000000000000000000").obj())
             .obj()},
        // ConstructorDbName
        {R"({ "a" : Dbref( "ns", "000000000000000000000000", "dbname" ) })",
         B().append("a",
                    B().append("$ref", "ns")
                        .append("$id", "000000000000000000000000")
                        .append("$db", "dbname")
                        .obj())
             .obj()},
        // ConstructorNumber
        {R"({ "a" : Dbref( "ns", 1 ) })",
         B().append("a", B().append("$ref", "ns").append("$id", 1).obj()).obj()},
        // ConstructorObject
        {R"({ "a" : Dbref( "ns", { "b" : true } ) })",
         B().append("a", B().append("$ref", "ns").append("$id", B().append("b", true).obj()).obj())
             .obj()},
        // NumberId
        {R"({ "a" : { "$ref" : "ns", "$id" : 1 } })",
         B().append("a", B().append("$ref", "ns").append("$id", 1).obj()).obj()},
        // ObjectAsId
        {R"({ "a" : { "$ref" : "ns", "$id" : { "b" : true } } })",
         B().append("a", B().append("$ref", "ns").append("$id", B().append("b", true).obj()).obj())
             .obj()},
        // StringId
        {R"({ "a" : { "$ref" : "ns", "$id" : "000000000000000000000000" } })",
         B().append("a", B().append("$ref", "ns").append("$id", "000000000000000000000000").obj())
             .obj()},
        // ObjectIDObject
        {R"({ "a" : { "$ref" : "ns", "$id" : { "$oid" : "000000000000000000000000" } } })",
         B().append("a", B().append("$ref", "ns").append("$id", OID()).obj()).obj()},
        // ObjectIDConstructor
        {R"({ "a" : { "$ref" : "ns", "$id" : ObjectId( "000000000000000000000000" ) } })",
         B().append("a", B().append("$ref", "ns").append("$id", OID()).obj()).obj()},
        // DbName
        {R"({ "a" : { "$ref" : "ns", "$id" : "000000000000000000000000", )"
         R"("$db" : "dbname" } }))",
         B().append("a",
                    B().append("$ref", "ns")
                        .append("$id", "000000000000000000000000")
                        .append("$db", "dbname")
                        .obj())
             .obj()},
    });
}

TEST(FromJsonTest, IdTest) {
    checkEquivalenceEach({
        {R"({ "_id" : "000000000000000000000000" })",
         B().append("_id", "000000000000000000000000").obj()},      // StringId
        {R"({ "_id" : { "$oid" : "000000000000000000000000" } })",  //
         B().appendOID("_id").obj()},                               // Oid
        {R"({ "_id" : ObjectId( "0f0f0f0f0f0f0f0f0f0f0f0f" ) })",   //
         B().append("_id", OID::from(std::string(OID::kOIDSize, '\x0f').data())).obj()},  // Oid2
    });
    checkRejectionEach({
        R"({ "_id" : { "$oid" : "0000000000000000000000000" } })",  // too long
        R"({ "_id" : ObjectId( "0f0f0f0f0f0f0f0f0f0f0f0f0" ) })",   //  "
        R"({ "_id" : { "$oid" : "00000000000000000000000" } })",    // too short
        R"({ "_id" : ObjectId( "0f0f0f0f0f0f0f0f0f0f0f0" ) })",     //  "
        R"({ "_id" : { "$oid" : "00000000000Z000000000000" } })",   // invalid char
        R"({ "_id" : ObjectId( "0f0f0f0f0f0fZf0f0f0f0f0f" ) })",    //  "
    });
}

TEST(FromJsonTest, BinDataTypes) {
    struct Spec {
        unsigned code;
        BinDataType bdt;
    };
    const Spec specs[] = {
        {0x00, BinDataGeneral},
        {0x01, Function},
        {0x02, ByteArrayDeprecated},
        {0x03, bdtUUID},
        {0x04, newUUID},
        {0x05, MD5Type},
        {0x06, Encrypt},
        {0x80, bdtCustom},
    };
    for (const auto& ts : specs) {
        checkEquivalence(
            fmt::sprintf(R"({ "a" : { "$binary" : "YWJj", "$type" : "%02x" } })", ts.code),
            BSONObjBuilder().appendBinData("a", 3, ts.bdt, "abc").obj());
    }
}

TEST(FromJsonTest, BinDataPadded) {
    checkEquivalenceEach({
        {R"({ "a" : { "$binary" : "YWI=", "$type" : "00" } })",  // padded
         B().appendBinData("a", 2, BinDataGeneral, "ab").obj()},
        {R"({ "a" : { "$binary" : "YQ==", "$type" : "00" } })",  // padded double
         B().appendBinData("a", 1, BinDataGeneral, "a").obj()},
    });
}

TEST(FromJsonTest, BinDataAllChars) {
    using namespace std::literals::string_literals;
    const std::string z =
        "\x00\x10\x83\x10\x51\x87\x20\x92"
        "\x8B\x30\xD3\x8F\x41\x14\x93\x51"
        "\x55\x97\x61\x96\x9B\x71\xD7\x9F"
        "\x82\x18\xA3\x92\x59\xA7\xA2\x9A"
        "\xAB\xB2\xDB\xAF\xC3\x1C\xB3\xD3"
        "\x5D\xB7\xE3\x9E\xBB\xF3\xDF\xBF"s;  // std::string literal for embedded '\0'
    const std::string json(
        R"({ "a" : {)"
        R"( "$binary" : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",)"
        R"( "$type" : "00" })"
        R"( })");
    checkEquivalence(json, B().appendBinData("a", z.size(), BinDataGeneral, z.data()).obj());
}

TEST(FromJsonTest, BinDataRejection) {
    // A few with bad length.
    checkRejectionEach({
        R"({ "a" : { "$binary" : "YQ=", "$type" : "00" } })",
        R"({ "a" : { "$binary" : "YQ", "$type" : "00" } })",
        R"({ "a" : { "$binary" : "YQX==", "$type" : "00" } })",
        R"({ "a" : { "$binary" : "YQX", "$type" : "00" } })",
        R"({ "a" : { "$binary" : "YQXZ=", "$type" : "00" } })",
        R"({ "a" : { "$binary" : "YQXZ==", "$type" : "00" } })",
    });

    checkRejectionEach({
        R"({ "a" : { "$binary" : "a...", "$type" : "00" } })",   // BadChars
        R"({ "a" : { "$binary" : "AAAA", "$type" : "0" } })",    // TypeTooShort
        R"({ "a" : { "$binary" : "AAAA", "$type" : "000" } })",  // TypeTooLong
        R"({ "a" : { "$binary" : "AAAA", "$type" : "ZZ" } })",   // TypeBadChars
        R"({ "a" : { "$binary" : "AAAA", "$type" : "" } })",     // EmptyType
        R"({ "a" : { "$binary" : "AAAA" } })",                   // NoType
        R"({ "a" : { "$binary" : "AAAA", "$type" : "100" } })",  // InvalidType
    });
}

TEST(FromJsonTest, Date) {
    const uint64_t u64Max = std::numeric_limits<uint64_t>::max();
    const BSONObj negDate = B().appendDate("a", Date_t::fromMillisSinceEpoch(-1)).obj();
    checkEquivalenceEach({
        // DateZero
        // DOCS-2539:  We cannot parse dates generated with a Unix timestamp of zero in local
        // time, since the body of the date may be before the Unix Epoch.  This causes parsing
        // to fail even if the offset would properly adjust the time.  For example,
        // "1969-12-31T19:00:00-05:00" actually represents the Unix timestamp of zero, but we
        // cannot parse it because the body of the date is before 1970.
        // {R"({ "a" : { "$date" : 0 } })", B().appendDate("a", Date_t()).obj()},

        // DateNonzero
        {R"({ "a" : { "$date" : 1000000000 } })",
         B().appendDate("a", Date_t::fromMillisSinceEpoch(1'000'000'000)).obj()},

        // DateStrictMaxUnsigned
        // Need to handle this because jsonString outputs the value of Date_t as unsigned.
        // See SERVER-8330 and SERVER-8573.
        {fmt::sprintf(R"({ "a" : { "$date" : %u } })", u64Max), negDate},

        {fmt::sprintf(R"({ "a" : Date( %u ) })", u64Max), negDate},  // DateMaxUnsigned
        {R"({ "a" : { "$date" : -1 } })", negDate},                  // DateStrictNegative
        {R"({ "a" : Date( -1 ) })", negDate},                        // DateNegative
    });
    checkRejectionEach({
        fmt::sprintf(R"({ "a" : { "$date" : %u1 } })", u64Max),  // DateStrictTooLong
        fmt::sprintf(R"({ "a" : Date( %u1 } ) })", u64Max),      // DateTooLong
        R"({ "a" : { "$date" : "100" } })",                      // DateIsString
        R"({ "a" : Date("a") })",                                // DateIsString1
        R"({ "a" : new Date("a") })",                            // DateIsString2
        R"({ "a" : { "$date" : 1.1 } })",                        // DateIsFloat
        R"({ "a" : Date(1.1) })",                                // DateIsFloat1
        R"({ "a" : new Date(1.1) })",                            // DateIsFloat2
        R"({ "a" : { "$date" : 10e3 } })",                       // DateIsExponent
        R"({ "a" : Date(10e3) })",                               // DateIsExponent1
        R"({ "a" : new Date(10e3) })",                           // DateIsExponent2
    });
}

TEST(FromJsonTest, NumberTest) {
    checkEquivalenceEach({
        {R"({ "a" : NumberLong( 20000 ) })", B().append("a", 20000LL).obj()},  // NumberLong
        {fmt::sprintf(R"({'a': NumberLong(%d) })", std::numeric_limits<long long>::min()),
         B().append("a", std::numeric_limits<long long>::min()).obj()},  // NumberLongMin

        {R"({ "a" : NumberInt( 20000 ) })", B().appendNumber("a", 20000).obj()},    // NumberInt
        {R"({ "a" : NumberLong( -20000 ) })", B().append("a", -20000LL).obj()},     // NumberLongNeg
        {R"({ "a" : NumberInt( -20000 ) })", B().appendNumber("a", -20000).obj()},  // NumberIntNeg
    });
    checkRejectionEach({
        R"({ "a" : NumberLong( 'sdf' ) })",
        R"({ "a" : NumberInt( 'sdf' ) })",
    });
}

TEST(FromJsonTest, JSTimestampTest) {
    checkEquivalenceEach({
        {R"({ "a" : Timestamp( 20, 5 ) })", B().append("a", Timestamp(20, 5)).obj()},
        {R"({ "a" : Timestamp( 0, 0 ) })", B().append("a", Timestamp()).obj()},
    });
    checkRejectionEach({
        R"({ "a" : Timestamp( 20 ) })",       // NoIncrement
        R"({ "a" : Timestamp() })",           // NoArgs
        R"({ "a" : Timestamp( 20.0, 1 ) })",  // FloatSeconds
        R"({ "a" : Timestamp( 20, 1.0 ) })",  // FloatIncrement
        R"({ "a" : Timestamp( -20, 5 ) })",   // NegativeSeconds
        R"({ "a" : Timestamp( 20, -5 ) })",   // NegativeIncrement
        R"({ "a" : Timestamp( q, 5 ) })",     // InvalidSeconds
    });
}

TEST(FromJsonTest, TimestampObjectTest) {
    checkEquivalenceEach({
        {R"({ "a" : { "$timestamp" : { "t" : 20 , "i" : 5 } } })",
         B().append("a", Timestamp(20, 5)).obj()},
        {R"({ "a" : { "$timestamp" : { "t" : 0, "i" : 0} } })", B().append("a", Timestamp()).obj()},
    });
    checkRejectionEach({
        // InvalidFieldName
        R"({ "a" : { "$timestamp" : { "time" : 20 , "increment" : 5 } } })",
        R"({ "a" : { "$timestamp" : { "t" : 20 } } })",             // NoIncrement
        R"({ "a" : { "$timestamp" : { "t" : -20 , "i" : 5 } } })",  // NegativeSeconds
        R"({ "a" : { "$timestamp" : { "t" : 20 , "i" : -5 } } })",  // NegativeIncrement
        R"({ "a" : { "$timestamp" : { "t" : q , "i" : 5 } } })",    // InvalidSeconds
        R"({ "a" : { "$timestamp" : { } } })",                      // NoArgs
        R"({ "a" : { "$timestamp" : { "t" : 1.0, "i" : 0} } })",    // FloatSeconds
        R"({ "a" : { "$timestamp" : { "t" : 20, "i" : 1.0} } })",   // FloatIncrement
    });
}

BSONObj re(const std::string& name, const std::string& re, const std::string& options) {
    BSONObjBuilder b;
    b.appendRegex(name, re, options);
    return b.obj();
}

TEST(FromJsonTest, Regex) {
    checkEquivalenceEach({
        {R"({ "a" : { "$regex" : "b", "$options" : "i" } })", re("a", "b", "i")},
        {R"({ "a" : { "$regex" : "b" } })", re("a", "b", "")},
        {R"({ "a" : { "$regex" : "\t", "$options" : "i" } })", re("a", "\t", "i")},
        {R"({ "a" : /"/ })", re("a", "\"", "")},
        {R"({ "a" : { $regex : "\"" }})", re("a", "\"", "")},
        {R"({ "a" : { "$regex" : "b", "$options" : "" } })", re("a", "b", "")},
        {R"({ "a" : { "$regex" : "b", "$options" : "ms" } })", re("a", "b", "ms")},
        {R"({ "a" : { "$regex" : "", "$options" : ""} })", re("a", "", "")},
        {R"({ "a" :  //  })", re("a", "", "")},
    });
    checkRejectionEach({
        R"({ "a" : { "$regex" : "b", "field" : "i" } })",
        R"({ "a" : { "$regex" : "b", "$options" : "1" } })",
        R"({ "a" : /b/c })",
        R"({ "a" : /b/ic })",
        R"({ "a" : { "$regex" : "b", "$options" : "a" } })",
        R"({ "a" : /b/a })",
        R"({ "a" : { "$regex" : // } })",
        R"({ "a" : { "$regex" : "test", "$options" : "ii" } })",
    });
}

TEST(FromJsonTest, Malformed) {
    checkRejectionEach({
        R"({)",
        R"(})",
        R"({test})",
        R"({test)",
        R"({ test : 1)",
        R"({ test : 1 , })",
        R"({ test : 1 , tst})",
        R"({ a : [])",
        R"({ a : { test : 1 })",
        R"({ a : [ { test : 1]})",
        R"({ a : [ { test : 1], b : 2})",
        R"({ a : "test"string })",
        R"({ a : test"string" })",
        R"({ a"bad" : "teststring" })",
        R"({ "a"test : "teststring" })",
        R"({ "atest : "teststring" })",
        R"({ atest" : "teststring" })",
        R"({ atest" : 1 })",
        R"({ atest : "teststring })",
        R"({ atest : teststring" })",
    });
}

TEST(FromJsonTest, UnquotedFieldName) {
    checkEquivalenceEach({
        {"{ a_b : 1 }", BSONObjBuilder().append("a_b", 1).obj()},    //
        {"{ $a_b : 1 }", BSONObjBuilder().append("$a_b", 1).obj()},  //
    });
    checkRejectionEach({
        "{ 123 : 1 }",
        "{ -123 : 1 }",
        "{ .123 : 1 }",
        "{ -.123 : 1 }",
        "{ -1.23 : 1 }",
        "{ 1e23 : 1 }",
        "{ -1e23 : 1 }",
        "{ -1e-23 : 1 }",
        "{ -hello : 1 }",
        "{ il.legal : 1 }",
        "{ 10gen : 1 }",
        "{ _123. : 1 }",
        "{ he-llo : 1 }",
        "{ bad\nchar : 1 }",
        "{ thiswill\fail : 1 }",
        "{ failu\re : 1 }",
        "{ t\test : 1 }",
        "{ \break: 1 }",
        "{ \xdc\x80\xef\xbc\x80 : 1 }",  // "\u0700\uff00"
        "{ bl\\u3333p: 1 }",
        "{ bl-33p: 1 }",
    });
}

TEST(FromJsonTest, QuoteTest) {
    checkEquivalenceEach({
        {R"({ 'ab\'c"' : 'bb\b \'"' })", B().append("ab'c\"", "bb\b '\"").obj()},
        {R"({ '"' : "test" })", B().append(R"(")", "test").obj()},
        {R"({ "'" : "test" })", B().append(R"(')", "test").obj()},
        {R"({ '"' : "test" })", B().append(R"(")", "test").obj()},
        {R"({ '"\'"' : "test" })", B().append(R"("'")", "test").obj()},
        {R"({ "'\"'" : "test" })", B().append(R"('"')", "test").obj()},
        {R"({ "test" : "'" })", B().append("test", R"(')").obj()},
        {R"({ "test" : '"' })", B().append("test", R"(")").obj()},
    });
}

TEST(FromJsonTest, ObjectIdTest) {
    OID id;
    id.init("deadbeeff00ddeadbeeff00d");
    checkEquivalenceEach({
        // ObjectId
        {R"({ "_id": ObjectId( "deadbeeff00ddeadbeeff00d" ) })", B().append("_id", id).obj()},
        // ObjectId2
        {R"({ "foo": ObjectId( "deadbeeff00ddeadbeeff00d" ) })", B().append("foo", id).obj()},
    });
}

TEST(FromJsonTest, NumericTypes) {
    long long kMaxS64 = 0x7fff'ffff'ffff'ffff;
    struct Val {
        int i;
        long l;
        double d;
    };
    const Val vals[] = {
        {123, kMaxS64, 3.14}, {-123, -kMaxS64, -3.14},
    };
    for (const Val& val : vals) {
        const BSONObj obj =
            B().append("int", val.i).append("long", val.l).append("double", val.d).obj();
        const std::string altReps[] = {
            fmt::sprintf(R"({ "int": %d, "long": %d, "double": %.2f })", val.i, val.l, val.d),
            fmt::sprintf(R"({ 'int': NumberInt(%d), 'long': NumberLong(%d), 'double': %.2f })",
                         val.i,
                         val.l,
                         val.d),
        };
        for (const auto& json : altReps) {
            checkEquivalence(json, obj);
            BSONObj o = fromjson(json);
            ASSERT(o["int"].type() == NumberInt);
            ASSERT(o["long"].type() == NumberLong);
            ASSERT(o["double"].type() == NumberDouble);
            ASSERT(o["long"].numberLong() == val.l);
        }
    }

    checkEquivalenceEach({
        // NumericIntMin
        {fmt::sprintf("{'a': %d }", std::numeric_limits<int>::min()),
         B().appendNumber("a", std::numeric_limits<int>::min()).obj()},
        // NumericLongMin
        {fmt::sprintf("{'a': %d }", std::numeric_limits<long long>::min()),
         B().append("a", std::numeric_limits<long long>::min()).obj()},
        // NumericLimits
        {fmt::sprintf("{'': [%d,%d,%d,%d] }",
                      std::numeric_limits<long long>::max(),
                      std::numeric_limits<long long>::min(),
                      std::numeric_limits<int>::max(),
                      std::numeric_limits<int>::min()),
         B().append("",
                    Arr()
                        .append(std::numeric_limits<long long>::max())
                        .append(std::numeric_limits<long long>::min())
                        .append(std::numeric_limits<int>::max())
                        .append(std::numeric_limits<int>::min())
                        .arr())
             .obj()},
    });

    // Overflows double by giving it an exponent that is too large
    checkRejectionEach({
        fmt::sprintf("{ test : %g%s}", std::numeric_limits<double>::max(), "1111111111"),
        fmt::sprintf("{ test : %g%s}", std::numeric_limits<double>::min(), "11111111111"),
    });
}

TEST(FromJsonTest, EmbeddedDates) {
    const long long kMin = 1257829200000;
    const long long kMax = 1257829200100;
    auto makeDate = [](long long ms) { return Date_t::fromMillisSinceEpoch(ms); };
    const BSONObj bson =
        B().append("time.valid",
                   B().appendDate("$gt", makeDate(kMin)).appendDate("$lt", makeDate(kMax)).obj())
            .obj();
    const std::string formats[] = {
        R"({ "time.valid" : { $gt : { "$date" :  %d } , $lt : { "$date" : %d } } })",
        R"({ "time.valid" : { $gt : { "$date" :  %d } , $lt : { "$date" : %d } } })",
        R"({ "time.valid" : { $gt : Date(%d) , $lt : Date( %d ) } })",
    };
    for (const auto& format : formats) {
        const std::string json = fmt::sprintf(format, kMin, kMax);
        BSONObj o = fromjson(json);
        ASSERT_EQUALS(3, (o["time.valid"].type()));
        BSONObj e = o["time.valid"].embeddedObjectUserCheck();
        ASSERT_EQUALS(9, e["$gt"].type());
        ASSERT_EQUALS(9, e["$lt"].type());
        checkEquivalence(json, bson);
    }
}

TEST(FromJsonTest, StringContainingNull) {
    checkEquivalence(R"({ "x" : "a\u0000b" })", B().append("x", "a\0b", 4).obj());
    checkRejection(R"({ x\u0000y : "a" })");  // NullFieldUnquoted
}

TEST(FromJsonTest, MinMaxKey) {
    checkEquivalenceEach({
        {R"({ "a" : { "$minKey" : 1 } })", B().appendMinKey("a").obj()},  // MinKey
        {R"({ "a" : { "$maxKey" : 1 } })", B().appendMaxKey("a").obj()},  // MaxKey
    });
    checkRejectionEach({
        R"({ "$minKey" : 1 })",  // MinKeyAlone
        R"({ "$maxKey" : 1 })",  // MaxKeyAlone
    });
}

}  // namespace FromJsonTests
}  // namespace
