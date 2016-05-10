/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"  // for str::stream()!

#define ASSERT_PARSES(TYPE, INPUT_STRING, EXPECTED_VALUE)    \
    do {                                                     \
        TYPE v;                                              \
        ASSERT_OK(parseNumberFromString(INPUT_STRING, &v));  \
        ASSERT_EQUALS(static_cast<TYPE>(EXPECTED_VALUE), v); \
    } while (false)

#define ASSERT_PARSES_WITH_BASE(TYPE, INPUT_STRING, BASE, EXPECTED_VALUE) \
    do {                                                                  \
        TYPE v;                                                           \
        ASSERT_OK(parseNumberFromStringWithBase(INPUT_STRING, BASE, &v)); \
        ASSERT_EQUALS(static_cast<TYPE>(EXPECTED_VALUE), v);              \
    } while (false)

namespace mongo {
namespace {

template <typename _NumberType>
class CommonNumberParsingTests {
public:
    typedef _NumberType NumberType;
    typedef std::numeric_limits<NumberType> Limits;

    static void TestRejectingBadBases() {
        NumberType ignored;
        ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("0", -1, &ignored));
        ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("10", 1, &ignored));
        ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("-10", 37, &ignored));
        ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase(" ", -1, &ignored));
        ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("f", 37, &ignored));
        ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("^%", -1, &ignored));
    }

    static void TestParsingNonNegatives() {
        ASSERT_PARSES(NumberType, "10", 10);
        ASSERT_PARSES(NumberType, "0", 0);
        ASSERT_PARSES(NumberType, "1", 1);
        ASSERT_PARSES(NumberType, "0xff", 0xff);
        ASSERT_PARSES(NumberType, "077", 077);
    }

    static void TestParsingNegatives() {
        if (Limits::is_signed) {
            ASSERT_PARSES(NumberType, "-10", -10);
            ASSERT_PARSES(NumberType, "-0xff", -0xff);
            ASSERT_PARSES(NumberType, "-077", -077);
        } else {
            NumberType ignored;
            ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-10", &ignored));
            ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-0xff", &ignored));
            ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-077", &ignored));
        }
    }

    static void TestParsingGarbage() {
        NumberType ignored;
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString(" ", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString(" 10", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("15b", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("--10", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("+-10", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("++10", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("--10", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("0x+10", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("0x-10", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("0+10", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("0-10", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("1+10", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("1-10", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("48*3", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("0x", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("+", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("+0x", &ignored));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-0x", &ignored));
    }

    static void TestParsingWithExplicitBase() {
        NumberType x;
        ASSERT_PARSES_WITH_BASE(NumberType, "15b", 16, 0x15b);
        ASSERT_PARSES_WITH_BASE(NumberType, "77", 8, 077);
        ASSERT_PARSES_WITH_BASE(NumberType, "z", 36, 35);
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("1b", 10, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("80", 8, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("0X", 16, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("0x", 16, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("0x", 8, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("0X", 8, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("0x", 10, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("0X", 10, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("+0X", 16, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("+0x", 16, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("+0x", 8, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("+0X", 8, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("+0x", 10, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("+0X", 10, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("-0X", 16, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("-0x", 16, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("-0x", 8, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("-0X", 8, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("-0x", 10, &x));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("-0X", 10, &x));
    }

    static void TestParsingLimits() {
        using namespace mongoutils;
        NumberType ignored;
        ASSERT_PARSES(NumberType, std::string(str::stream() << Limits::max()), Limits::max());
        ASSERT_PARSES(NumberType, std::string(str::stream() << Limits::min()), Limits::min());
        ASSERT_EQUALS(
            ErrorCodes::FailedToParse,
            parseNumberFromString(std::string(str::stream() << Limits::max() << '0'), &ignored));

        if (Limits::is_signed)
            ASSERT_EQUALS(ErrorCodes::FailedToParse,
                          parseNumberFromString(std::string(str::stream() << Limits::min() << '0'),
                                                &ignored));
    }
};

#define GENERAL_NUMBER_TESTS(SHORT_NAME, TYPE)                    \
    class ParseNumberTests##SHORT_NAME : public unittest::Test {  \
    public:                                                       \
        typedef CommonNumberParsingTests<TYPE> TestFns;           \
    };                                                            \
    TEST_F(ParseNumberTests##SHORT_NAME, RejectBadBases) {        \
        TestFns::TestRejectingBadBases();                         \
    }                                                             \
    TEST_F(ParseNumberTests##SHORT_NAME, ParseNonNegatives) {     \
        TestFns::TestParsingNonNegatives();                       \
    }                                                             \
    TEST_F(ParseNumberTests##SHORT_NAME, ParseNegatives) {        \
        TestFns::TestParsingNegatives();                          \
    }                                                             \
    TEST_F(ParseNumberTests##SHORT_NAME, ParseGarbage) {          \
        TestFns::TestParsingGarbage();                            \
    }                                                             \
    TEST_F(ParseNumberTests##SHORT_NAME, ParseWithExplicitBase) { \
        TestFns::TestParsingWithExplicitBase();                   \
    }                                                             \
    TEST_F(ParseNumberTests##SHORT_NAME, TestParsingLimits) {     \
        TestFns::TestParsingLimits();                             \
    }

GENERAL_NUMBER_TESTS(Short, short)
GENERAL_NUMBER_TESTS(Int, int)
GENERAL_NUMBER_TESTS(Long, long)
GENERAL_NUMBER_TESTS(LongLong, long long)
GENERAL_NUMBER_TESTS(UnsignedShort, unsigned short)
GENERAL_NUMBER_TESTS(UnsignedInt, unsigned int)
GENERAL_NUMBER_TESTS(UnsignedLong, unsigned long)
GENERAL_NUMBER_TESTS(UnsignedLongLong, unsigned long long)
GENERAL_NUMBER_TESTS(Int16, int16_t);
GENERAL_NUMBER_TESTS(Int32, int32_t);
GENERAL_NUMBER_TESTS(Int64, int64_t);
GENERAL_NUMBER_TESTS(UInt16, uint16_t);
GENERAL_NUMBER_TESTS(UInt32, uint32_t);
GENERAL_NUMBER_TESTS(UInt64, uint64_t);

TEST(ParseNumber, NotNullTerminated) {
    ASSERT_PARSES(int, StringData("1234", 3), 123);
}

TEST(ParseNumber, Int8) {
    int8_t ignored;
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-129", &ignored));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-130", &ignored));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-900", &ignored));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("128", &ignored));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("130", &ignored));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("900", &ignored));

    for (int32_t i = -128; i <= 127; ++i)
        ASSERT_PARSES(int8_t, std::string(mongoutils::str::stream() << i), i);
}

TEST(ParseNumber, UInt8) {
    uint8_t ignored;
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-129", &ignored));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-130", &ignored));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-900", &ignored));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("+256", &ignored));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("+900", &ignored));

    for (uint32_t i = 0; i <= 255; ++i)
        ASSERT_PARSES(uint8_t, std::string(mongoutils::str::stream() << i), i);
}

TEST(Double, TestRejectingBadBases) {
    double ignored;

    // Only supported base for parseNumberFromStringWithBase<double> is 0.
    ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("0", -1, &ignored));
    ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("0", 1, &ignored));
    ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("0", 8, &ignored));
    ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("0", 10, &ignored));
    ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("0", 16, &ignored));
}

TEST(Double, TestParsingGarbage) {
    double d;
    CommonNumberParsingTests<double>::TestParsingGarbage();
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString<double>("1.0.1", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString<double>("1.0-1", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString<double>(" 1.0", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString<double>("1.0P4", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString<double>("1e6	", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString<double>("	1e6", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString<double>("1e6 ", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString<double>(" 1e6", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString<double>("0xabcab.defPa", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString<double>("1.0\0garbage"_sd, &d));
}

TEST(Double, TestParsingOverflow) {
    double d;
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("1e309", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-1e309", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("1e-400", &d));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-1e-400", &d));
}

TEST(Double, TestParsingNan) {
    double d = 0;
    ASSERT_OK(parseNumberFromString("NaN", &d));
    ASSERT_TRUE(std::isnan(d));
}

TEST(Double, TestParsingInfinity) {
    double d = 0;
    ASSERT_OK(parseNumberFromString("infinity", &d));
    ASSERT_TRUE(std::isinf(d));
    d = 0;
    ASSERT_OK(parseNumberFromString("-Infinity", &d));
    ASSERT_TRUE(std::isinf(d));
}

TEST(Double, TestParsingNormal) {
    ASSERT_PARSES(double, "10", 10);
    ASSERT_PARSES(double, "0", 0);
    ASSERT_PARSES(double, "1", 1);
    ASSERT_PARSES(double, "-10", -10);
    ASSERT_PARSES(double, "1e8", 1e8);
    ASSERT_PARSES(double, "1e-8", 1e-8);
    ASSERT_PARSES(double, "12e-8", 12e-8);
    ASSERT_PARSES(double, "-485.381e-8", -485.381e-8);

#if !(defined(_WIN32) || defined(__sun))
    // Parse hexadecimal representations of a double.  Hex literals not supported by MSVC, and
    // not parseable by the Windows SDK libc or the Solaris libc in the mode we build.
    // See SERVER-14131.

    ASSERT_PARSES(double, "0xff", 0xff);
    ASSERT_PARSES(double, "-0xff", -0xff);
    ASSERT_PARSES(double, "0xabcab.defdefP-10", 0xabcab.defdefP-10);
#endif
}

}  // namespace
}  // namespace mongo
