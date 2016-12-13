/**
 *    Copyright (C) 2016 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/granularity_rounder.h"

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using boost::intrusive_ptr;
using std::string;
using std::vector;

namespace {
const double DELTA = 0.0001;

const vector<string> preferredNumberSeries{
    "R5", "R10", "R20", "R40", "R80", "1-2-5", "E6", "E12", "E24", "E48", "E96", "E192"};

/**
 * Tests that two values 'actual' and 'expected' are equal. If one of the values is a double, then
 * we check to see if the values are approximately equal with tolerance 'delta'.
 */
void testEquals(Value actual, Value expected, double delta = DELTA) {
    if (actual.getType() == BSONType::NumberDouble || actual.getType() == BSONType::NumberDouble) {
        ASSERT_APPROX_EQUAL(actual.coerceToDouble(), expected.coerceToDouble(), delta);
    } else {
        ASSERT_VALUE_EQ(actual, expected);
    }
}

const vector<double> getSeries(intrusive_ptr<GranularityRounder> rounder) {
    const auto* preferredNumbersRounder =
        dynamic_cast<GranularityRounderPreferredNumbers*>(rounder.get());
    ASSERT(preferredNumbersRounder != nullptr);

    return preferredNumbersRounder->getSeries();
}

/**
 * Gets the preferred number series from a GranularityRounder and converts all of the numbers to
 * Decimal128. This helps with testing that the GranularityRounders work with the Decimal128
 * datatype.
 */
const vector<Decimal128> getSeriesDecimal(intrusive_ptr<GranularityRounder> rounder) {
    const auto* preferredNumbersRounder =
        dynamic_cast<GranularityRounderPreferredNumbers*>(rounder.get());
    ASSERT(preferredNumbersRounder != nullptr);

    auto doubleSeries = preferredNumbersRounder->getSeries();
    vector<Decimal128> decimalSeries;

    for (auto&& doubleNumber : doubleSeries) {
        decimalSeries.push_back(Decimal128(doubleNumber));
    }

    return decimalSeries;
}

/**
 * Tests that a number in a preferred number series rounds up to the next number in the
 * series. For example, 1 is in the R5 series, so we test that it should round up to 1.6.
 */
void testRoundingUpInSeries(intrusive_ptr<GranularityRounder> rounder) {
    auto series = getSeries(rounder);
    double multiplier = 0.00001;
    for (int j = 0; j < 6; j++) {
        for (size_t i = 1; i < series.size(); i++) {
            // Make sure that each element in the series at position i - 1 rounds up to the number
            // at position i.
            Value input = Value(series[i - 1] * multiplier);
            Value roundedValue = rounder->roundUp(input);
            Value expectedValue = Value(series[i] * multiplier);
            try {
                testEquals(roundedValue, expectedValue);
            } catch (...) {
                FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                                   << " failed rounding up the value "
                                   << input.coerceToDouble()
                                   << " at multiplier level "
                                   << multiplier
                                   << ". Expected "
                                   << expectedValue.coerceToDouble()
                                   << ", but got "
                                   << roundedValue.coerceToDouble());
            }
        }
        multiplier *= 10.0;
    }
}

/**
 * Tests that a number in a preferred number series rounds up to the next number in the
 * series when the numbers are Decimal128 values.
 */
void testRoundingUpInSeriesDecimal(intrusive_ptr<GranularityRounder> rounder) {
    auto series = getSeriesDecimal(rounder);

    Decimal128 multiplier = Decimal128(0.00001);
    for (int j = 0; j < 6; j++) {
        for (size_t i = 1; i < series.size(); i++) {
            // Make sure that each element in the series at position i - 1 rounds up to the number
            // at position i.
            Value input = Value(series[i - 1].multiply(multiplier));
            Value expectedValue = Value(series[i].multiply(multiplier));
            Value roundedValue = rounder->roundUp(input);
            ASSERT_EQ(roundedValue.getType(), BSONType::NumberDecimal);

            try {
                testEquals(roundedValue, expectedValue);
            } catch (...) {
                FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                                   << " failed rounding up the value "
                                   << input.coerceToDecimal().toString()
                                   << " at multiplier level "
                                   << multiplier.toString()
                                   << ". Expected "
                                   << expectedValue.coerceToDecimal().toString()
                                   << ", but got "
                                   << roundedValue.coerceToDecimal().toString());
            }
        }
        multiplier = multiplier.multiply(Decimal128(10));
    }
}

/**
 * Tests that a number in between two values in a preferred number series rounds up to the
 * appropriate value. For example, the values 1 and 1.6 are in the R5 series. We test that the value
 * 1.3 rounds up to 1.6.
 */
void testRoundingUpBetweenSeries(intrusive_ptr<GranularityRounder> rounder) {
    auto series = getSeries(rounder);
    double multiplier = 0.00001;
    for (int j = 0; j < 6; j++) {
        for (size_t i = 1; i < series.size(); i++) {
            double lower = series[i - 1] * multiplier;
            double upper = series[i] * multiplier;
            double middle = (lower + upper) / 2.0;

            // Make sure a number in between two numbers in the series rounds up correctly.
            Value roundedValue = rounder->roundUp(Value(middle));
            Value expectedValue = Value(upper);
            try {
                testEquals(roundedValue, expectedValue);
            } catch (...) {
                FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                                   << " failed rounding up the value "
                                   << middle
                                   << " at multiplier level "
                                   << multiplier
                                   << ". Expected "
                                   << expectedValue.coerceToDouble()
                                   << ", but got "
                                   << roundedValue.coerceToDouble());
            }
        }
        multiplier *= 10.0;
    }
}

/**
 * Tests that a number in between two values in a preferred number series rounds up to the
 * appropriate value when the values are Decimal128 values.
 */
void testRoundingUpBetweenSeriesDecimal(intrusive_ptr<GranularityRounder> rounder) {
    auto series = getSeriesDecimal(rounder);
    Decimal128 multiplier = Decimal128(0.00001);
    for (int j = 0; j < 6; j++) {
        for (size_t i = 1; i < series.size(); i++) {
            Decimal128 lower = series[i - 1].multiply(multiplier);
            Decimal128 upper = series[i].multiply(multiplier);
            Decimal128 middle = (lower.add(upper)).divide(Decimal128(2));

            // Make sure a number in between two numbers in the series rounds up correctly.
            Value expectedValue = Value(upper);
            Value roundedValue = rounder->roundUp(Value(middle));
            ASSERT_EQ(roundedValue.getType(), BSONType::NumberDecimal);

            try {
                testEquals(roundedValue, expectedValue);
            } catch (...) {
                FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                                   << " failed rounding up the value "
                                   << middle.toString()
                                   << " at multiplier level "
                                   << multiplier.toString()
                                   << ". Expected "
                                   << expectedValue.coerceToDecimal().toString()
                                   << ", but got "
                                   << roundedValue.coerceToDecimal().toString());
            }
        }
        multiplier = multiplier.multiply(Decimal128(10));
    }
}

/**
 * Tests that a number in a preferred number series rounds down to the previous number in the
 * series. For example, 1.6 is in the R5 series, so we test that it should round down to 1.
 */
void testRoundingDownInSeries(intrusive_ptr<GranularityRounder> rounder) {
    auto series = getSeries(rounder);
    double multiplier = 0.00001;
    for (int j = 0; j < 6; j++) {
        // Make sure that each element in the series at position i rounds down to the number at
        // position i - 1.
        for (size_t i = series.size() - 1; i > 0; i--) {
            Value input = Value(series[i] * multiplier);
            Value roundedValue = rounder->roundDown(input);
            Value expectedValue = Value(series[i - 1] * multiplier);
            try {
                testEquals(roundedValue, expectedValue);
            } catch (...) {
                FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                                   << " failed rounding down the value "
                                   << input.coerceToDouble()
                                   << " at multiplier level "
                                   << multiplier
                                   << ". Expected "
                                   << expectedValue.coerceToDouble()
                                   << ", but got "
                                   << roundedValue.coerceToDouble());
            }
        }
        multiplier *= 10.0;
    }
}

/**
 * Tests that a number in a preferred number series rounds down to the previous number in the
 * series when the values are Decimal128 values.
 */
void testRoundingDownInSeriesDecimal(intrusive_ptr<GranularityRounder> rounder) {
    auto series = getSeriesDecimal(rounder);
    Decimal128 multiplier = Decimal128(0.00001);
    for (int j = 0; j < 6; j++) {
        // Make sure that each element in the series at position i rounds down to the number at
        // position i - 1.
        for (size_t i = series.size() - 1; i > 0; i--) {
            Value input = Value(series[i].multiply(multiplier));
            Value expectedValue = Value(series[i - 1].multiply(multiplier));
            Value roundedValue = rounder->roundDown(input);
            ASSERT_EQ(roundedValue.getType(), BSONType::NumberDecimal);

            try {
                testEquals(roundedValue, expectedValue);
            } catch (...) {
                FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                                   << " failed rounding down the value "
                                   << input.coerceToDecimal().toString()
                                   << " at multiplier level "
                                   << multiplier.toString()
                                   << ". Expected "
                                   << expectedValue.coerceToDecimal().toString()
                                   << ", but got "
                                   << roundedValue.coerceToDecimal().toString());
            }
        }
        multiplier = multiplier.multiply(Decimal128(10));
    }
}

/**
 * Tests that a number in between two values in a preferred number series rounds down to the
 * appropriate value. For example, the values 1 and 1.6 are in the R5 series. We test that the value
 * 1.3 rounds down to 1.
 */
void testRoundingDownBetweenSeries(intrusive_ptr<GranularityRounder> rounder) {
    auto series = getSeries(rounder);
    double multiplier = 0.00001;
    for (int j = 0; j < 6; j++) {
        for (size_t i = 1; i < series.size(); i++) {
            double lower = series[i - 1] * multiplier;
            double upper = series[i] * multiplier;
            double middle = (lower + upper) / 2.0;

            // Make sure a number in between two numbers in the series rounds down correctly.
            Value roundedValue = rounder->roundDown(Value(middle));
            Value expectedValue = Value(lower);
            try {
                testEquals(roundedValue, expectedValue);
            } catch (...) {
                FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                                   << " failed rounding down the value "
                                   << middle
                                   << " at multiplier level "
                                   << multiplier
                                   << ". Expected "
                                   << expectedValue.coerceToDouble()
                                   << ", but got "
                                   << roundedValue.coerceToDouble());
            }
        }
        multiplier *= 10.0;
    }
}

/**
 * Tests that a number in between two values in a preferred number series rounds down to the
 * appropriate value when the values are Decimal128 values.
 */
void testRoundingDownBetweenSeriesDecimal(intrusive_ptr<GranularityRounder> rounder) {
    auto series = getSeriesDecimal(rounder);
    Decimal128 multiplier = Decimal128(0.00001);
    for (int j = 0; j < 6; j++) {
        for (size_t i = 1; i < series.size(); i++) {
            Decimal128 lower = series[i - 1].multiply(multiplier);
            Decimal128 upper = series[i].multiply(multiplier);
            Decimal128 middle = (lower.add(upper)).divide(Decimal128(2));

            // Make sure a number in between two numbers in the series rounds down correctly.
            Value expectedValue = Value(lower);
            Value roundedValue = rounder->roundDown(Value(middle));
            ASSERT_EQ(roundedValue.getType(), BSONType::NumberDecimal);

            try {
                testEquals(roundedValue, expectedValue);
            } catch (...) {
                FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                                   << " failed rounding down the value "
                                   << middle.toString()
                                   << " at multiplier level "
                                   << multiplier.toString()
                                   << ". Expected "
                                   << expectedValue.coerceToDecimal().toString()
                                   << ", but got "
                                   << roundedValue.coerceToDecimal().toString());
            }
        }
        multiplier = multiplier.multiply(Decimal128(10));
    }
}

/**
 * Internally we represent preferred number series with a finite list of numbers. If we are rounding
 * a value outside the range of this list, we scale the values in the list by a power of 10. We
 * keep scaling until the number we are rounding falls into the range spanned by the preferred
 * numbers list. This method tests that the last value in the preferred numbers list rounds up to
 * the first value in the preferred numbers list multiplied by 10. This method also tests that the
 * first value in the preferred numbers list rounds down to the last value in the preferred numbers
 * list divided by 10.
 */
void testSeriesWrappingAround(intrusive_ptr<GranularityRounder> rounder) {
    auto series = getSeries(rounder);
    double multiplier = 0.00001;
    for (int j = 0; j < 6; j++) {
        Value input = Value(series.back() * multiplier);
        Value roundedValue = rounder->roundUp(input);
        Value expectedValue = Value(series.front() * multiplier * 10.0);
        try {
            testEquals(roundedValue, expectedValue);
        } catch (...) {
            FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                               << " failed rounding up the value "
                               << input.coerceToDouble()
                               << " at multiplier level "
                               << multiplier
                               << ". Expected "
                               << expectedValue.coerceToDouble()
                               << ", but got "
                               << roundedValue.coerceToDouble());
        }

        input = Value(series.front() * multiplier);
        roundedValue = rounder->roundDown(input);
        expectedValue = Value(series.back() * multiplier / 10.0);
        try {
            testEquals(roundedValue, expectedValue);
        } catch (...) {
            FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                               << " failed rounding down the value "
                               << input.coerceToDouble()
                               << " at multiplier level "
                               << multiplier
                               << ". Expected "
                               << expectedValue.coerceToDouble()
                               << ", but got "
                               << roundedValue.coerceToDouble());
        }
        multiplier *= 10.0;
    }
}

/**
 * Tests that rounding wraps around when we are rounding Decimal128 values.
 */
void testSeriesWrappingAroundDecimal(intrusive_ptr<GranularityRounder> rounder) {
    auto series = getSeriesDecimal(rounder);
    Decimal128 multiplier = Decimal128(0.00001);
    for (int j = 0; j < 6; j++) {
        Value input = Value(series.back().multiply(multiplier));
        Value expectedValue = Value(series.front().multiply(multiplier).multiply(Decimal128(10)));
        Value roundedValue = rounder->roundUp(input);
        ASSERT_EQ(roundedValue.getType(), BSONType::NumberDecimal);

        try {
            testEquals(roundedValue, expectedValue);
        } catch (...) {
            FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                               << " failed rounding up the value "
                               << input.coerceToDecimal().toString()
                               << " at multiplier level "
                               << multiplier.toString()
                               << ". Expected "
                               << expectedValue.coerceToDecimal().toString()
                               << ", but got "
                               << roundedValue.coerceToDecimal().toString());
        }

        input = Value(series.front().multiply(multiplier));
        expectedValue = Value(series.back().multiply(multiplier).divide(Decimal128(10)));
        roundedValue = rounder->roundDown(input);
        ASSERT_EQ(roundedValue.getType(), BSONType::NumberDecimal);

        try {
            testEquals(roundedValue, expectedValue);
        } catch (...) {
            FAIL(str::stream() << "The GranularityRounder for " << rounder->getName()
                               << " failed rounding down the value "
                               << input.coerceToDecimal().toString()
                               << " at multiplier level "
                               << multiplier.toString()
                               << ". Expected "
                               << expectedValue.coerceToDecimal().toString()
                               << ", but got "
                               << roundedValue.coerceToDecimal().toString());
        }
        multiplier.multiply(Decimal128(10));
    }
}

TEST(GranularityRounderPreferredNumbersTest, ShouldRoundUpNumberInSeriesToNextNumberInSeries) {
    for (auto&& series : preferredNumberSeries) {
        auto rounder =
            GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), series);

        testRoundingUpInSeries(rounder);
        testRoundingUpInSeriesDecimal(rounder);
    }
}

TEST(GranularityRounderPreferredNumbersTest,
     ShouldRoundDownNumberInSeriesToPreviousNumberInSeries) {
    for (auto&& series : preferredNumberSeries) {
        auto rounder =
            GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), series);

        testRoundingDownInSeries(rounder);
        testRoundingDownInSeriesDecimal(rounder);
    }
}

TEST(GranularityRounderPreferredNumbersTest, ShouldRoundUpValueInBetweenSeriesNumbers) {
    for (auto&& series : preferredNumberSeries) {
        auto rounder =
            GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), series);

        testRoundingUpBetweenSeries(rounder);
        testRoundingUpBetweenSeriesDecimal(rounder);
    }
}

TEST(GranularityRounderPreferredNumbersTest, ShouldRoundDownValueInBetweenSeriesNumbers) {
    for (auto&& series : preferredNumberSeries) {
        auto rounder =
            GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), series);

        testRoundingDownBetweenSeries(rounder);
        testRoundingDownBetweenSeriesDecimal(rounder);
    }
}

TEST(GranularityRounderPreferredNumbersTest, SeriesShouldWrapAroundWhenRounding) {
    for (auto&& series : preferredNumberSeries) {
        auto rounder =
            GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), series);

        testSeriesWrappingAround(rounder);
        testSeriesWrappingAroundDecimal(rounder);
    }
}

TEST(GranularityRounderPreferredNumbersTest, ShouldRoundZeroToZero) {
    for (auto&& series : preferredNumberSeries) {
        auto rounder =
            GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), series);

        // Make sure that each GranularityRounder rounds zero to zero.
        testEquals(rounder->roundUp(Value(0)), Value(0));
        testEquals(rounder->roundDown(Value(0)), Value(0));

        testEquals(rounder->roundUp(Value(Decimal128(0))), Value(Decimal128(0)));
        testEquals(rounder->roundDown(Value(Decimal128(0))), Value(Decimal128(0)));
    }
}

TEST(GranularityRounderPreferredNumbersTest, ShouldFailOnRoundingNonNumericValues) {
    for (auto&& series : preferredNumberSeries) {
        auto rounder =
            GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), series);

        // Make sure that each GranularityRounder fails when rounding a non-numeric value.
        Value stringValue = Value("test");
        ASSERT_THROWS_CODE(rounder->roundUp(stringValue), UserException, 40262);
        ASSERT_THROWS_CODE(rounder->roundDown(stringValue), UserException, 40262);
    }
}

TEST(GranularityRounderPreferredNumbersTest, ShouldFailOnRoundingNaN) {
    for (auto&& series : preferredNumberSeries) {
        auto rounder =
            GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), series);

        // Make sure that each GranularityRounder fails when rounding NaN.
        Value nan = Value(std::nan("NaN"));
        ASSERT_THROWS_CODE(rounder->roundUp(nan), UserException, 40263);
        ASSERT_THROWS_CODE(rounder->roundDown(nan), UserException, 40263);

        Value positiveNan = Value(Decimal128::kPositiveNaN);
        Value negativeNan = Value(Decimal128::kNegativeNaN);
        ASSERT_THROWS_CODE(rounder->roundUp(positiveNan), UserException, 40263);
        ASSERT_THROWS_CODE(rounder->roundDown(positiveNan), UserException, 40263);
        ASSERT_THROWS_CODE(rounder->roundUp(negativeNan), UserException, 40263);
        ASSERT_THROWS_CODE(rounder->roundDown(negativeNan), UserException, 40263);
    }
}

TEST(GranularityRounderPreferredNumbersTest, ShouldFailOnRoundingNegativeNumber) {
    for (auto&& series : preferredNumberSeries) {
        auto rounder =
            GranularityRounder::getGranularityRounder(new ExpressionContextForTest(), series);

        // Make sure that each GranularityRounder fails when rounding a negative number.
        Value negativeNumber = Value(-1);
        ASSERT_THROWS_CODE(rounder->roundUp(negativeNumber), UserException, 40268);
        ASSERT_THROWS_CODE(rounder->roundDown(negativeNumber), UserException, 40268);

        negativeNumber = Value(Decimal128(-1));
        ASSERT_THROWS_CODE(rounder->roundUp(negativeNumber), UserException, 40268);
        ASSERT_THROWS_CODE(rounder->roundDown(negativeNumber), UserException, 40268);
    }
}
}  // namespace
}  // namespace mongo
