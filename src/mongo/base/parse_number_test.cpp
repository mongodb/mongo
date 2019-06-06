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

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <typeinfo>
#include <vector>

#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/if_constexpr.h"
#include "mongo/util/str.h"  // for str::stream()!

#define ASSERT_PARSES_WITH_PARSER(type, input_string, parser, expected_value) \
    do {                                                                      \
        type v;                                                               \
        ASSERT_OK(parser(input_string, &v));                                  \
        ASSERT_EQ(static_cast<type>(expected_value), v);                      \
    } while (false)

#define ASSERT_PARSES(TYPE, INPUT_STRING, EXPECTED_VALUE) \
    ASSERT_PARSES_WITH_PARSER(TYPE, INPUT_STRING, NumberParser(), EXPECTED_VALUE)

#define ASSERT_PARSES_WITH_BASE(TYPE, INPUT_STRING, BASE, EXPECTED_VALUE) \
    ASSERT_PARSES_WITH_PARSER(TYPE, INPUT_STRING, NumberParser().base(BASE), EXPECTED_VALUE)

namespace mongo {
namespace {

template <typename... Ts>
struct TypeListTag {};
template <typename T>
using TypeTag = TypeListTag<T>;

template <typename F, typename... Ts>
void apply(F&& f, TypeListTag<Ts...>) {
    (f.run(TypeTag<Ts>{}), ...);
}


auto allTypes = TypeListTag<short,
                            int,
                            long,
                            long long,
                            unsigned short,
                            unsigned int,
                            unsigned long,
                            unsigned long long,
                            int16_t,
                            int32_t,
                            int64_t,
                            uint16_t,
                            uint32_t,
                            uint64_t>{};

#define PARSE_TEST(TEST_NAME)                 \
    struct PARSE_TEST_##TEST_NAME {           \
        template <typename NumberType>        \
        static void run();                    \
    };                                        \
    struct RUN_PARSE_TEST_##TEST_NAME {       \
        template <typename T>                 \
        void run(TypeTag<T>) const {          \
            PARSE_TEST_##TEST_NAME::run<T>(); \
        }                                     \
    } TEST_NAME;                              \
    template <typename NumberType>            \
    void PARSE_TEST_##TEST_NAME::run()

/*
 * The PARSE_TEST macro will generate boilerplate code to enable applying the same function to
 * multiple types.
 * NumberType is a template parameter representing a type the test is supposed to pass for.
 * After writing a PARSE_TEST, there is an object with the name passed in as the parameter. This
 * should be passed to the apply function along with a list of types to apply to the function.
 */

PARSE_TEST(TestParsingNegatives) {
    struct Spec {
        StringData spec;
        int expectedValue;
    };
    std::vector<Spec> specs = {{"-0", 0}, {"-10", -10}, {"-0xff", -0xff}};
    if (typeid(NumberType) != typeid(double)) {
        specs.push_back({"-077", -077});  // no octals for double
    }
    for (const auto& s : specs) {
        if (std::is_signed_v<NumberType>) {
            ASSERT_PARSES(NumberType, s.spec, s.expectedValue);
        } else {
            NumberType ignored;
            ASSERT_EQ(ErrorCodes::FailedToParse, NumberParser{}(s.spec, &ignored));
        }
    }
}

TEST(NumberParser, ParseNegatives) {
    apply(TestParsingNegatives, allTypes);
}

PARSE_TEST(TestRejectingBadBases) {
    struct Spec {
        int base;
        StringData spec;
    };
    std::vector<Spec> specs = {{-1, "0"}, {1, "10"}, {37, "-10"}, {-1, " "}, {37, "f"}, {-1, "^%"}};
    if (typeid(NumberType) == typeid(double)) {
        std::vector<Spec> doubleSpecs = {
            {8, "0"}, {10, "0"}, {16, "0"}, {36, "0"},
        };
        std::copy(doubleSpecs.begin(), doubleSpecs.end(), std::back_inserter(specs));
    }
    for (const auto& s : specs) {
        NumberType ignored;
        ASSERT_EQ(ErrorCodes::BadValue, NumberParser().base(s.base)(s.spec, &ignored));
    }
}

TEST(NumberParser, RejectBadBases) {
    apply(TestRejectingBadBases, allTypes);
}

PARSE_TEST(TestParsingNonNegatives) {
    struct {
        StringData spec;
        int expectedValue;
    } specs[] = {{"10", 10}, {"0", 0}, {"1", 1}, {"0xff", 0xff}, {"077", 077}};
    for (const auto[str, expected] : specs) {
        ASSERT_PARSES(NumberType, str, expected);
    }
}

TEST(NumberParser, ParseNonNegatives) {
    apply(TestParsingNonNegatives, allTypes);
}

PARSE_TEST(TestParsingGarbage) {
    NumberType ignored;
    StringData garbage[] = {"",     " ",     " 10",   "15b",  "--10", "+-10", "++10",
                            "--10", "0x+10", "0x-10", "0+10", "0-10", "48*3", "0x",
                            "0X",   "+",     "-",     "+0x",  "+0X",  "-0X",  "-0x"};

    StringData decimalGarbage[] = {"1.0.1",
                                   "1.0-1",
                                   " 1.0",
                                   "1.0P4",
                                   "1e6	",
                                   "	1e6",
                                   "1e6 ",
                                   " 1e6",
                                   "0xabcab.defPa",
                                   "1.0\0garbage"_sd};
    for (const auto str : garbage) {
        ASSERT_EQ(ErrorCodes::FailedToParse, NumberParser{}(str, &ignored));
    }
    if (typeid(NumberType) == typeid(double)) {
        for (const auto str : decimalGarbage) {
            ASSERT_EQ(ErrorCodes::FailedToParse, NumberParser{}(str, &ignored));
        }
    }
}

TEST(NumberParser, ParseGarbage) {
    apply(TestParsingGarbage, allTypes);
}

PARSE_TEST(TestParsingWithExplicitBase) {
    struct {
        StringData spec;
        int base;
        NumberType val;
    } passes[] = {{"15b", 16, 0x15b},
                  {"77", 8, 077},
                  {"z", 36, 35},
                  {"09", 10, 9},
                  {"00000000000z0", 36, 36 * 35},
                  {"1011", 2, 0b1011},
                  {"11", 5, 6}};
    for (const auto& s : passes) {
        ASSERT_PARSES_WITH_BASE(NumberType, s.spec, s.base, s.val);
    }

    struct {
        StringData spec;
        int base;
    } fails[] = {{"1b", 10},  {"80", 8},   {"0X", 16},  {"0x", 16},  {"0X", 8},  {"0x", 8},
                 {"0X", 10},  {"0x", 10},  {"+0X", 16}, {"+0x", 16}, {"+0X", 8}, {"+0x", 8},
                 {"+0X", 10}, {"+0x", 10}, {"-0X", 16}, {"-0x", 16}, {"-0X", 8}, {"-0x", 8},
                 {"-0X", 10}, {"-0x", 10}, {"2", 2},    {"4", 3}};
    for (const auto& s : fails) {
        NumberType ignored;
        ASSERT_EQ(ErrorCodes::FailedToParse, NumberParser().base(s.base)(s.spec, &ignored));
    }
}

TEST(NumberParser, ParseWithExplicitBase) {
    apply(TestParsingWithExplicitBase, allTypes);
}

PARSE_TEST(TestParsingLimits) {
    using Limits = std::numeric_limits<NumberType>;
    NumberType ignored;
    ASSERT_PARSES(NumberType, std::string(str::stream() << Limits::max()), Limits::max());
    ASSERT_PARSES(NumberType, std::string(str::stream() << Limits::min()), Limits::min());
    ASSERT_EQUALS(ErrorCodes::Overflow,
                  NumberParser{}(std::string(str::stream() << Limits::max() << '0'), &ignored));

    if (std::is_signed_v<NumberType>) {
        // Max + 1
        ASSERT_EQUALS(ErrorCodes::Overflow,
                      NumberParser{}(std::to_string(uint64_t(Limits::max()) + 1), &ignored));

        // Min - 1 (equivalent to -(Max + 2))
        ASSERT_EQUALS(ErrorCodes::Overflow,
                      NumberParser{}("-" + std::to_string(uint64_t(Limits::max()) + 2), &ignored));

        ASSERT_EQUALS(ErrorCodes::Overflow,
                      NumberParser{}(std::string(str::stream() << Limits::min() << '0'), &ignored));
    }
}

TEST(NumberParser, ParseLimits) {
    apply(TestParsingLimits, allTypes);
}

PARSE_TEST(TestSkipLeadingWhitespace) {
    StringData whitespaces[] = {" ", "", "\t  \t", "\r\n\n\t", "\f\v "};
    struct {
        StringData spec;
        bool is_negative;
    } specs[] = {{"10", false},
                 {"0", false},
                 {"1", false},
                 {"0xff", false},
                 {"077", false},
                 {"-10", true},
                 {"-0", true},
                 {"-1", true},
                 {"-0xff", true},
                 {"-077", true}};
    NumberParser defaultParser;
    NumberParser skipWs = NumberParser().skipWhitespace();
    for (const auto[numStr, is_negative] : specs) {
        NumberType expected;

        bool shouldParse = !is_negative || (is_negative && std::is_signed_v<NumberType>);
        Status parsed = defaultParser(numStr, &expected);

        if (shouldParse) {
            ASSERT_OK(parsed);
        } else {
            ASSERT_EQ(ErrorCodes::FailedToParse, parsed);
        }

        for (StringData ws : whitespaces) {
            std::string withWhitespace = ws.toString() + numStr;
            if (shouldParse) {
                ASSERT_PARSES_WITH_PARSER(NumberType, withWhitespace, skipWs, expected);
            } else {
                NumberType actual;
                ASSERT_EQ(ErrorCodes::FailedToParse, skipWs(withWhitespace, &actual));
            }
        }
    }
}

TEST(NumberParser, TestSkipLeadingWhitespace) {
    apply(TestSkipLeadingWhitespace, allTypes);
}

PARSE_TEST(TestEndOfNum) {
    struct {
        StringData spec;
        bool is_negative;
    } specs[] = {{"10", false},
                 {"0", false},
                 {"1", false},
                 {"0xff", false},
                 {"077", false},
                 {"-10", true},
                 {"-0", true},
                 {"-1", true},
                 {"-0xff", true},
                 {"-077", true}};
    StringData suffixes[] = {
        " ",
        "\r\t",
        "@!()",
        "  #$",
        "Hello World",
        "g",  // since the largest inferred base is 16, next non-number character will be g
        ""};
    NumberParser defaultParser;
    for (const auto[numStr, is_negative] : specs) {
        NumberType expected;
        bool shouldParse = !is_negative || (is_negative && std::is_signed_v<NumberType>);
        Status parsed = defaultParser(numStr, &expected);
        if (shouldParse) {
            ASSERT_OK(parsed);
        } else {
            ASSERT_EQ(ErrorCodes::FailedToParse, parsed);
        }
        for (StringData& suffix : suffixes) {
            std::string spec = numStr.toString() + suffix;
            char* numEnd = nullptr;
            NumberType actual;
            parsed = NumberParser().allowTrailingText()(spec, &actual, &numEnd);
            if (shouldParse) {
                ASSERT_OK(parsed);
                ASSERT_EQ(actual, expected);
                StringData remaining_str{numEnd, suffix.size()};
                ASSERT_TRUE(remaining_str == suffix);
            } else {
                ASSERT_EQ(ErrorCodes::FailedToParse, parsed);
                ASSERT_TRUE(numEnd == spec.c_str());
            }
        }
    }
}

TEST(NumberParser, TestEndOfNum) {
    apply(TestEndOfNum, allTypes);
}

PARSE_TEST(TestNotNullTerminated) {
    StringData noNull{"1234", 3};
    NumberParser parsers[] = {NumberParser(),
                              NumberParser().skipWhitespace(),
                              NumberParser().base(10),
                              NumberParser().allowTrailingText()};
    for (auto& parser : parsers) {
        ASSERT_PARSES_WITH_PARSER(NumberType, noNull, parser, 123);
    }
}

TEST(NumberParser, TestNotNullTerminated) {
    apply(TestNotNullTerminated, allTypes);
}

PARSE_TEST(TestSkipLeadingWsAndEndptr) {
    struct {
        StringData spec;
        bool is_negative;
    } specs[] = {{"10", false},
                 {"0", false},
                 {"1", false},
                 {"0xff", false},
                 {"077", false},
                 {"-10", true},
                 {"-0", true},
                 {"-1", true},
                 {"-0xff", true},
                 {"-077", true}};
    StringData whitespaces[] = {" ", "", "\t  \t", "\r\n\n\t", "\f\v "};
    NumberParser defaultParser;
    for (const auto[numStr, is_negative] : specs) {
        NumberType expected;
        bool shouldParse = !is_negative || (is_negative && std::is_signed_v<NumberType>);
        Status parsed = defaultParser(numStr, &expected);
        if (shouldParse) {
            ASSERT_OK(parsed);
        } else {
            ASSERT_EQ(ErrorCodes::FailedToParse, parsed);
        }
        for (StringData& prefix : whitespaces) {
            std::string spec = prefix.toString() + numStr;
            char* numEnd = nullptr;
            NumberType actual;
            parsed = NumberParser().skipWhitespace()(spec, &actual, &numEnd);
            if (shouldParse) {
                ASSERT_OK(parsed);
                ASSERT_EQ(actual, expected);
                ASSERT_TRUE(numEnd == (spec.c_str() + spec.size()));
            } else {
                ASSERT_EQ(ErrorCodes::FailedToParse, parsed);
                ASSERT_TRUE(StringData(numEnd, spec.size()) == spec.c_str());
            }
        }
    }
}

TEST(NumberParser, TestSkipLeadingWsAndEndptr) {
    apply(TestSkipLeadingWsAndEndptr, allTypes);
}

TEST(ParseNumber, NotNullTerminated) {
    ASSERT_PARSES(int, StringData("1234", 3), 123);
}

TEST(ParseNumber, Int8) {
    int8_t ignored;
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("-129", &ignored));
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("-130", &ignored));
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("-900", &ignored));
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("128", &ignored));
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("130", &ignored));
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("900", &ignored));

    for (int32_t i = -128; i <= 127; ++i)
        ASSERT_PARSES(int8_t, std::string(str::stream() << i), i);
}

TEST(ParseNumber, UInt8) {
    uint8_t ignored;
    ASSERT_EQUALS(ErrorCodes::FailedToParse, NumberParser{}("-129", &ignored));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, NumberParser{}("-130", &ignored));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, NumberParser{}("-900", &ignored));
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("+256", &ignored));
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("+900", &ignored));

    for (uint32_t i = 0; i <= 255; ++i)
        ASSERT_PARSES(uint8_t, std::string(str::stream() << i), i);
}

TEST(ParseNumber, TestParsingOverflow) {
    uint64_t u64;
    // These both have one too many hex digits and will overflow the multiply. The second overflows
    // such that the truncated result is still greater than either input and can catch overly
    // simplistic overflow checks.
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser().base(16)("0xfffffffffffffffff", &u64));
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser().base(16)("0x7ffffffffffffffff", &u64));

    // 2**64 exactly. This will overflow the add.
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser().base(10)("18446744073709551616", &u64));

    uint32_t u32;
    // Too large when down-converting.
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser().base(16)("0xfffffffff", &u32));

    int32_t i32;
    // Too large when down-converting.
    ASSERT_EQUALS(ErrorCodes::Overflow,
                  NumberParser{}(std::to_string(std::numeric_limits<uint32_t>::max()), &i32));
}

PARSE_TEST(DoubleNormalParse) {
    ASSERT_PARSES(NumberType, "10", 10);
    ASSERT_PARSES(NumberType, "0", 0);
    ASSERT_PARSES(NumberType, "1", 1);
    ASSERT_PARSES(NumberType, "-10", -10);
    ASSERT_PARSES(NumberType, "1e8", 1e8);
    ASSERT_PARSES(NumberType, "1e-8", 1e-8);
    ASSERT_PARSES(NumberType, "12e-8", 12e-8);
    ASSERT_PARSES(NumberType, "-485.381e-8", -485.381e-8);

#if !(defined(_WIN32) || defined(__sun))
    // Parse hexadecimal representations of a double.  Hex literals not supported by MSVC, and
    // not parseable by the Windows SDK libc or the Solaris libc in the mode we build.
    // See SERVER-14131.

    ASSERT_PARSES(NumberType, "0xff", 255);
    ASSERT_PARSES(NumberType, "-0xff", -255);
    ASSERT_PARSES(NumberType, "0xabcab.defdefP-10", 687.16784283419838);
#endif
}

TEST(NumberParser, TestDoubleNormalParse) {
    apply(DoubleNormalParse, TypeListTag<double>{});
}

TEST(Double, TestParsingOverflow) {
    double d;
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("1e309", &d));
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("-1e309", &d));
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("1e-400", &d));
    ASSERT_EQUALS(ErrorCodes::Overflow, NumberParser{}("-1e-400", &d));
}

TEST(Double, TestParsingNan) {
    double d = 0;
    ASSERT_OK(NumberParser{}("NaN", &d));
    ASSERT_OK(NumberParser{}("nan", &d));
    ASSERT_TRUE(std::isnan(d));
}

TEST(Double, TestParsingNegativeZero) {
    double d = 0;
    ASSERT_OK(NumberParser{}("-0.0", &d));
    ASSERT_EQ(d, -0.0);
    ASSERT_TRUE(std::signbit(d));
}

TEST(Double, TestParsingInfinity) {
    double d = 0;
    ASSERT_OK(NumberParser{}("infinity", &d));
    ASSERT_TRUE(std::isinf(d));
    d = 0;
    ASSERT_OK(NumberParser{}("-Infinity", &d));
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

    ASSERT_PARSES(double, "0xff", 255);
    ASSERT_PARSES(double, "-0xff", -255);
    ASSERT_PARSES(double, "0xabcab.defdefP-10", 687.16784283419838);
#endif
}

}  // namespace
}  // namespace mongo
