/**
 *    Copyright (C) 2016 MongoDB Inc.
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

namespace mongo {

using boost::intrusive_ptr;
using std::string;
using std::vector;

namespace {
// "Least rounded" Renard number series, taken from Wikipedia page on preferred
// numbers: https://en.wikipedia.org/wiki/Preferred_number#Renard_numbers
const vector<double> r5Series{10, 16, 25, 40, 63};

const vector<double> r10Series{100, 125, 160, 200, 250, 315, 400, 500, 630, 800};

const vector<double> r20Series{100, 112, 125, 140, 160, 180, 200, 224, 250, 280,
                               315, 355, 400, 450, 500, 560, 630, 710, 800, 900};

const vector<double> r40Series{100, 106, 112, 118, 125, 132, 140, 150, 160, 170, 180, 190, 200,
                               212, 224, 236, 250, 265, 280, 300, 315, 355, 375, 400, 425, 450,
                               475, 500, 530, 560, 600, 630, 670, 710, 750, 800, 850, 900, 950};

const vector<double> r80Series{103, 109, 115, 122, 128, 136, 145, 155, 165, 175, 185, 195, 206, 218,
                               230, 243, 258, 272, 290, 307, 325, 345, 365, 387, 412, 437, 462, 487,
                               515, 545, 575, 615, 650, 690, 730, 775, 825, 875, 925, 975};

// 1-2-5 series, taken from Wikipedia page on preferred numbers:
// https://en.wikipedia.org/wiki/Preferred_number#1-2-5_series
const vector<double> series125{10, 20, 50};

// E series, taken from Wikipedia page on preferred numbers:
// https://en.wikipedia.org/wiki/Preferred_number#E_series
const vector<double> e6Series{10, 15, 22, 33, 47, 68};

const vector<double> e12Series{10, 12, 15, 18, 22, 27, 33, 39, 47, 56, 68, 82};

const vector<double> e24Series{10, 11, 12, 13, 15, 16, 18, 20, 22, 24, 27, 30,
                               33, 36, 39, 43, 47, 51, 56, 62, 68, 75, 82, 91};

const vector<double> e48Series{100, 105, 110, 115, 121, 127, 133, 140, 147, 154, 162, 169,
                               178, 187, 196, 205, 215, 226, 237, 249, 261, 274, 287, 301,
                               316, 332, 348, 365, 383, 402, 422, 442, 464, 487, 511, 536,
                               562, 590, 619, 649, 681, 715, 750, 787, 825, 866, 909, 953};

const vector<double> e96Series{100, 102, 105, 107, 110, 113, 115, 118, 121, 124, 127, 130, 133, 137,
                               140, 143, 147, 150, 154, 158, 162, 165, 169, 174, 178, 182, 187, 191,
                               196, 200, 205, 210, 215, 221, 226, 232, 237, 243, 249, 255, 261, 267,
                               274, 280, 287, 294, 301, 309, 316, 324, 332, 340, 348, 357, 365, 374,
                               383, 392, 402, 412, 422, 432, 442, 453, 464, 475, 487, 499, 511, 523,
                               536, 549, 562, 576, 590, 604, 619, 634, 649, 665, 681, 698, 715, 732,
                               750, 768, 787, 806, 825, 845, 866, 887, 909, 931, 953, 976};

const vector<double> e192Series{
    100, 101, 102, 104, 105, 106, 107, 109, 110, 111, 113, 114, 115, 117, 118, 120, 121, 123,
    124, 126, 127, 129, 130, 132, 133, 135, 137, 138, 140, 142, 143, 145, 147, 149, 150, 152,
    154, 156, 158, 160, 162, 164, 165, 167, 169, 172, 174, 176, 178, 180, 182, 184, 187, 189,
    191, 193, 196, 198, 200, 203, 205, 208, 210, 213, 215, 218, 221, 223, 226, 229, 232, 234,
    237, 240, 243, 246, 249, 252, 255, 258, 261, 264, 267, 271, 274, 277, 280, 284, 287, 291,
    294, 298, 301, 305, 309, 312, 316, 320, 324, 328, 332, 336, 340, 344, 348, 352, 357, 361,
    365, 370, 374, 379, 383, 388, 392, 397, 402, 407, 412, 417, 422, 427, 432, 437, 442, 448,
    453, 459, 464, 470, 475, 481, 487, 493, 499, 505, 511, 517, 523, 530, 536, 542, 549, 556,
    562, 569, 576, 583, 590, 597, 604, 612, 619, 626, 634, 642, 649, 657, 665, 673, 681, 690,
    698, 706, 715, 723, 732, 741, 750, 759, 768, 777, 787, 796, 806, 816, 825, 835, 845, 856,
    866, 876, 887, 898, 909, 920, 931, 942, 953, 965, 976, 988};
}  //  namespace

// Register the GranularityRounders for the Renard number series.
REGISTER_GRANULARITY_ROUNDER(R5, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return GranularityRounderPreferredNumbers::create(expCtx, r5Series, "R5");
});
REGISTER_GRANULARITY_ROUNDER(R10, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return GranularityRounderPreferredNumbers::create(expCtx, r10Series, "R10");
});
REGISTER_GRANULARITY_ROUNDER(R20, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return GranularityRounderPreferredNumbers::create(expCtx, r20Series, "R20");
});
REGISTER_GRANULARITY_ROUNDER(R40, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return GranularityRounderPreferredNumbers::create(expCtx, r40Series, "R40");
});
REGISTER_GRANULARITY_ROUNDER(R80, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return GranularityRounderPreferredNumbers::create(expCtx, r80Series, "R80");
});

REGISTER_GRANULARITY_ROUNDER_GENERAL(
    "1-2-5", 1_2_5, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return GranularityRounderPreferredNumbers::create(expCtx, series125, "1-2-5");
    });

// Register the GranularityRounders for the E series.
REGISTER_GRANULARITY_ROUNDER(E6, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return GranularityRounderPreferredNumbers::create(expCtx, e6Series, "E6");
});
REGISTER_GRANULARITY_ROUNDER(E12, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return GranularityRounderPreferredNumbers::create(expCtx, e12Series, "E12");
});
REGISTER_GRANULARITY_ROUNDER(E24, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return GranularityRounderPreferredNumbers::create(expCtx, e24Series, "E24");
});
REGISTER_GRANULARITY_ROUNDER(E48, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return GranularityRounderPreferredNumbers::create(expCtx, e48Series, "E48");
});
REGISTER_GRANULARITY_ROUNDER(E96, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return GranularityRounderPreferredNumbers::create(expCtx, e96Series, "E96");
});
REGISTER_GRANULARITY_ROUNDER(E192, [](const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return GranularityRounderPreferredNumbers::create(expCtx, e192Series, "E192");
});

GranularityRounderPreferredNumbers::GranularityRounderPreferredNumbers(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, vector<double> baseSeries, string name)
    : GranularityRounder(expCtx), _baseSeries(baseSeries), _name(name) {
    invariant(_baseSeries.size() > 1);
    invariant(std::is_sorted(_baseSeries.begin(), _baseSeries.end()));
}

intrusive_ptr<GranularityRounder> GranularityRounderPreferredNumbers::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, vector<double> baseSeries, string name) {
    return new GranularityRounderPreferredNumbers(expCtx, baseSeries, name);
}

namespace {
void uassertNonNegativeNumber(Value value) {
    uassert(40262,
            str::stream() << "A granularity rounder can only round numeric values, but found type: "
                          << typeName(value.getType()),
            value.numeric());

    double number = value.coerceToDouble();
    uassert(40263, "A granularity rounder cannot round NaN", !std::isnan(number));
    uassert(40268, "A granularity rounder can only round non-negative numbers", number >= 0.0);
}
}  // namespace

Value GranularityRounderPreferredNumbers::roundUp(Value value) {
    uassertNonNegativeNumber(value);

    if (value.coerceToDouble() == 0.0) {
        return value;
    }

    if (value.getType() == BSONType::NumberDecimal) {
        Decimal128 number = value.getDecimal();
        Decimal128 multiplier = Decimal128(1);

        // '_baseSeries' contains doubles, so we create a vector that contains the Decimal128
        // versions of the numbers in '_baseSeries' to make it easier to compare values to 'number'.
        vector<Decimal128> decimalSeries;
        for (auto&& doubleNumber : _baseSeries) {
            decimalSeries.push_back(Decimal128(doubleNumber));
        }

        while (number.isGreaterEqual(decimalSeries.back().multiply(multiplier))) {
            multiplier = multiplier.multiply(Decimal128(10));
        }

        Decimal128 previousMin;
        while (number.isLess(decimalSeries.front().multiply(multiplier))) {
            previousMin = decimalSeries.front().multiply(multiplier);
            multiplier = multiplier.divide(Decimal128(10));
            if (number.isGreaterEqual(decimalSeries.back().multiply(multiplier))) {
                // The number was between the previous min and the current max, so it must round up
                // to the previous min. For example, rounding up 0.8 in the E6 series.
                return Value(previousMin);
            }
        }

        // After scaling up or down, 'number' should now fall into the range spanned by
        // decimalSeries[i] * multiplier for all i in decimalSeries.
        invariant(number.isGreaterEqual(decimalSeries.front().multiply(multiplier)) &&
                  number.isLess(decimalSeries.back().multiply(multiplier)));

        // Get an iterator pointing to the first element in '_baseSeries' that is greater
        // than'number'.
        auto iterator =
            std::upper_bound(decimalSeries.begin(),
                             decimalSeries.end(),
                             number,
                             [multiplier](Decimal128 roundingNumber, Decimal128 seriesNumber) {
                                 return roundingNumber.isLess(seriesNumber.multiply(multiplier));
                             });

        return Value((*iterator).multiply(multiplier));
    } else {
        double number = value.coerceToDouble();
        double multiplier = 1.0;

        while (number >= (_baseSeries.back() * multiplier)) {
            multiplier *= 10.0;
        }

        double previousMin;
        while (number < (_baseSeries.front() * multiplier)) {
            previousMin = _baseSeries.front() * multiplier;
            multiplier /= 10.0;
            if (number >= (_baseSeries.back() * multiplier)) {
                // The number was between the previous min and the current max, so it must round up
                // to the previous min. For example, rounding up 0.8 in the E6 series.
                return Value(previousMin);
            }
        }

        // After scaling up or down, 'number' should now fall into the range spanned by
        // _baseSeries[i] * multiplier for all i in _baseSeries.
        invariant(number >= (_baseSeries.front() * multiplier) &&
                  number < (_baseSeries.back() * multiplier));

        // Get an iterator pointing to the first element in '_baseSeries' that is greater
        // than'number'.
        auto iterator = std::upper_bound(_baseSeries.begin(),
                                         _baseSeries.end(),
                                         number,
                                         [multiplier](double roundingNumber, double seriesNumber) {
                                             return roundingNumber < (seriesNumber * multiplier);
                                         });
        return Value(*iterator * multiplier);
    }
}

Value GranularityRounderPreferredNumbers::roundDown(Value value) {
    uassertNonNegativeNumber(value);

    if (value.coerceToDouble() == 0.0) {
        return value;
    }

    if (value.getType() == BSONType::NumberDecimal) {
        Decimal128 number = value.getDecimal();
        Decimal128 multiplier = Decimal128(1);

        // '_baseSeries' contains doubles, so we create a vector that contains the Decimal128
        // versions of the numbers in '_baseSeries' to make it easier to compare values to 'number'.
        vector<Decimal128> decimalSeries;
        for (auto&& doubleNumber : _baseSeries) {
            decimalSeries.push_back(Decimal128(doubleNumber));
        }

        while (number.isLessEqual(decimalSeries.front().multiply(multiplier))) {
            multiplier = multiplier.divide(Decimal128(10));
        }

        Decimal128 previousMax;
        while (number.isGreater(decimalSeries.back().multiply(multiplier))) {
            previousMax = decimalSeries.back().multiply(multiplier);
            multiplier = multiplier.multiply(Decimal128(10));
            if (number.isLessEqual(decimalSeries.front().multiply(multiplier))) {
                // The number is less than or equal to the current min, so it must round down to the
                // previous max. For example, rounding down 0.8 in the E6 series.
                return Value(previousMax);
            }
        }

        // After scaling up or down, 'number' should now fall into the range spanned by
        // decimalSeries[i] * multiplier for all i in decimalSeries.
        invariant(number.isGreater(decimalSeries.front().multiply(multiplier)) &&
                  number.isLessEqual(decimalSeries.back().multiply(multiplier)));

        // Get an iterator pointing to the first element in '_baseSeries' that is greater than or
        // equal to 'number'.
        auto iterator =
            std::lower_bound(decimalSeries.begin(),
                             decimalSeries.end(),
                             number,
                             [multiplier](Decimal128 seriesNumber, Decimal128 roundingNumber) {
                                 return seriesNumber.multiply(multiplier).isLess(roundingNumber);
                             });

        // We need to move the iterator back by one so that we round down to a number that is
        // strictly less than the value we are rounding.
        return Value(Value((*(iterator - 1)).multiply(multiplier)));
    } else {
        double number = value.coerceToDouble();
        double multiplier = 1.0;

        while (number <= (_baseSeries.front() * multiplier)) {
            multiplier /= 10.0;
        }

        double previousMax;
        while (number > (_baseSeries.back() * multiplier)) {
            previousMax = _baseSeries.back() * multiplier;
            multiplier *= 10.0;
            if (number <= _baseSeries.front() * multiplier) {
                // The number is less than or equal to the current min, so it must round down to the
                // previous max. For example, rounding down 0.8 in the E6 series.
                return Value(previousMax);
            }
        }

        // After scaling up or down, 'number' should now fall into the range spanned by
        // _baseSeries[i] * multiplier for all i in _baseSeries.
        invariant(number > (_baseSeries.front() * multiplier) &&
                  number <= (_baseSeries.back() * multiplier));

        // Get an iterator pointing to the first element in '_baseSeries' that is greater than or
        // equal to 'number'.
        auto iterator = std::lower_bound(_baseSeries.begin(),
                                         _baseSeries.end(),
                                         number,
                                         [multiplier](double seriesNumber, double roundingNumber) {
                                             return (seriesNumber * multiplier) < roundingNumber;
                                         });

        // We need to move the iterator back by one so that we round down to a number that is
        // strictly less than the value we are rounding.
        return Value(Value(*(iterator - 1) * multiplier));
    }
}

string GranularityRounderPreferredNumbers::getName() {
    return _name;
}

const vector<double> GranularityRounderPreferredNumbers::getSeries() const {
    return _baseSeries;
}
}  //  namespace mongo
