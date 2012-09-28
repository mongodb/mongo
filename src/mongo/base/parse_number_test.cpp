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
 */

#include "mongo/pch.h"

#include <limits>

#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/util/mongoutils/str.h"  // for str::stream()!
#include "mongo/unittest/unittest.h"

#define ASSERT_OK(EXPR) ASSERT_EQUALS(::mongo::Status::OK(), (EXPR))

#define ASSERT_PARSES(TYPE, INPUT_STRING, EXPECTED_VALUE) do {  \
        TYPE v;                                                 \
        ASSERT_OK(parseNumberFromString(INPUT_STRING, &v));     \
        ASSERT_EQUALS(static_cast<TYPE>(EXPECTED_VALUE), v);    \
    } while (false)

#define ASSERT_PARSES_WITH_BASE(TYPE, INPUT_STRING, BASE, EXPECTED_VALUE) do { \
        TYPE v;                                                         \
        ASSERT_OK(parseNumberFromStringWithBase(INPUT_STRING, BASE, &v)); \
        ASSERT_EQUALS(static_cast<TYPE>(EXPECTED_VALUE), v);            \
    } while (false)

namespace mongo {
namespace {

    // MakeUnique argument is to allow testing types that are aliases like int and int32_t
    template <typename _NumberType, int MakeUnique = 0>
    class CommonNumberParsingTests {
        TEMPLATE_SUITE_BOILERPLATE;

        typedef _NumberType NumberType;
        typedef std::numeric_limits<NumberType> Limits;

        TEMPLATE_SUITE_TEST(CommonNumberParsingTests, TestRejectingBadBases) {
            NumberType ignored;
            ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("0", -1, &ignored));
            ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("10", 1, &ignored));
            ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("-10", 37, &ignored));
            ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase(" ", -1, &ignored));
            ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("f", 37, &ignored));
            ASSERT_EQUALS(ErrorCodes::BadValue, parseNumberFromStringWithBase("^%", -1, &ignored));
        }

        TEMPLATE_SUITE_TEST(CommonNumberParsingTests, TestParsingNonNegatives) {
            ASSERT_PARSES(NumberType, "10", 10);
            ASSERT_PARSES(NumberType, "0", 0);
            ASSERT_PARSES(NumberType, "0xff", 0xff);
            ASSERT_PARSES(NumberType, "077", 077);
        }

        TEMPLATE_SUITE_TEST(CommonNumberParsingTests, TestParsingNegatives) {
            if (Limits::is_signed) {
                ASSERT_PARSES(NumberType, "-10", -10);
                ASSERT_PARSES(NumberType, "-0xff", -0xff);
                ASSERT_PARSES(NumberType, "-077", -077);
            }
            else {
                NumberType ignored;
                ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-10", &ignored));
                ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-0xff", &ignored));
                ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("-077", &ignored));
            }
        }

        TEMPLATE_SUITE_TEST(CommonNumberParsingTests, TestParsingGarbage) {
            NumberType ignored;
            ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("", &ignored));
            ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString(" ", &ignored));
            ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString(" 10", &ignored));
            ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromString("15b", &ignored));
        }

        TEMPLATE_SUITE_TEST(CommonNumberParsingTests, TestParsingWithExplicitBase) {
            NumberType ignored;
            ASSERT_PARSES_WITH_BASE(NumberType, "15b", 16, 0x15b);
            ASSERT_PARSES_WITH_BASE(NumberType, "77", 8, 077);
            ASSERT_PARSES_WITH_BASE(NumberType, "z", 36, 35);
            ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("1b", 10, &ignored));
            ASSERT_EQUALS(ErrorCodes::FailedToParse, parseNumberFromStringWithBase("80", 8, &ignored));
        }

        TEMPLATE_SUITE_TEST(CommonNumberParsingTests, TestParsingLimits) {
            using namespace mongoutils;
            NumberType ignored;
            ASSERT_PARSES(NumberType, (str::stream() << Limits::max()), Limits::max());
            ASSERT_PARSES(NumberType, (str::stream() << Limits::min()), Limits::min());
            ASSERT_EQUALS(ErrorCodes::FailedToParse,
                          parseNumberFromString((str::stream() << Limits::max() << '0'), &ignored));

            if (Limits::is_signed)
                ASSERT_EQUALS(
                        ErrorCodes::FailedToParse,
                        parseNumberFromString((str::stream() << Limits::min() << '0'), &ignored));
        }
    };

    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, short);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, int);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, long);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, long long);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, unsigned short);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, unsigned int);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, unsigned long);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, unsigned long long);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, int16_t, 1);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, int32_t, 1);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, int64_t, 1);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, uint16_t, 1);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, uint32_t, 1);
    TEMPLATE_SUITE_INSTANCE(CommonNumberParsingTests, uint64_t, 1);

}  // namespace
}  // namespace mongo
