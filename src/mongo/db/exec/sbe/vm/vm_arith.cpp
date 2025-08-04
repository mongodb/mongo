/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/str.h"
#include "mongo/util/summation.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/numeric/conversion/converter_policies.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
namespace vm {

using namespace value;

namespace {

/**
 * Helper for ExpressionPow to determine wither base^exp can be represented in a 64 bit int.
 *
 *'base' and 'exp' are both integers. Assumes 'exp' is in the range [0, 63].
 */
bool representableAsLong(long long base, long long exp) {
    invariant(exp <= 63);
    invariant(exp >= 0);
    struct MinMax {
        long long min;
        long long max;
    };

    // Array indices correspond to exponents 0 through 63. The values in each index are the min
    // and max bases, respectively, that can be raised to that exponent without overflowing a
    // 64-bit int. For max bases, this was computed by solving for b in
    // b = (2^63-1)^(1/exp) for exp = [0, 63] and truncating b. To calculate min bases, for even
    // exps the equation  used was b = (2^63-1)^(1/exp), and for odd exps the equation used was
    // b = (-2^63)^(1/exp). Since the magnitude of long min is greater than long max, the
    // magnitude of some of the min bases raised to odd exps is greater than the corresponding
    // max bases raised to the same exponents.

    static const MinMax kBaseLimits[] = {
        {std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max()},  // 0
        {std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max()},
        {-3037000499LL, 3037000499LL},
        {-2097152, 2097151},
        {-55108, 55108},
        {-6208, 6208},
        {-1448, 1448},
        {-512, 511},
        {-234, 234},
        {-128, 127},
        {-78, 78},  // 10
        {-52, 52},
        {-38, 38},
        {-28, 28},
        {-22, 22},
        {-18, 18},
        {-15, 15},
        {-13, 13},
        {-11, 11},
        {-9, 9},
        {-8, 8},  // 20
        {-8, 7},
        {-7, 7},
        {-6, 6},
        {-6, 6},
        {-5, 5},
        {-5, 5},
        {-5, 5},
        {-4, 4},
        {-4, 4},
        {-4, 4},  // 30
        {-4, 4},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-3, 3},
        {-2, 2},  // 40
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},  // 50
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},
        {-2, 2},  // 60
        {-2, 2},
        {-2, 2},
        {-2, 1}};

    return base >= kBaseLimits[exp].min && base <= kBaseLimits[exp].max;
};

static constexpr double kDoublePi = 3.141592653589793;
static constexpr double kDoublePiOver180 = kDoublePi / 180.0;
static constexpr double kDouble180OverPi = 180.0 / kDoublePi;


// Structures defining trigonometric functions computation.
struct Acos {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.acos();
        } else {
            result = std::acos(arg);
        }
    }
};

struct Acosh {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.acosh();
        } else {
            result = std::acosh(arg);
        }
    }
};

struct Asin {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.asin();
        } else {
            result = std::asin(arg);
        }
    }
};

struct Asinh {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.asinh();
        } else {
            result = std::asinh(arg);
        }
    }
};

struct Atan {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.atan();
        } else {
            result = std::atan(arg);
        }
    }
};

struct Atanh {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.atanh();
        } else {
            result = std::atanh(arg);
        }
    }
};

struct Cos {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.cos();
        } else {
            result = std::cos(arg);
        }
    }
};

struct Cosh {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.cosh();
        } else {
            result = std::cosh(arg);
        }
    }
};

struct Sin {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.sin();
        } else {
            result = std::sin(arg);
        }
    }
};

struct Sinh {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.sinh();
        } else {
            result = std::sinh(arg);
        }
    }
};

struct Tan {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.tan();
        } else {
            result = std::tan(arg);
        }
    }
};

struct Tanh {
    template <typename ArgT, typename ResT>
    static void computeFunction(const ArgT& arg, ResT& result) {
        if constexpr (std::is_same_v<ArgT, Decimal128>) {
            result = arg.tanh();
        } else {
            result = std::tanh(arg);
        }
    }
};

/**
 * Template for generic trigonometric function. The type in the template is a structure defining the
 * computation of the respective trigonometric function.
 */
template <typename TrigFunction>
FastTuple<bool, value::TypeTags, value::Value> genericTrigonometricFun(value::TypeTags argTag,
                                                                       value::Value argValue) {
    if (value::isNumber(argTag)) {
        switch (argTag) {
            case value::TypeTags::NumberInt32: {
                double result;
                TrigFunction::computeFunction(numericCast<int32_t>(argTag, argValue), result);
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberInt64: {
                double result;
                TrigFunction::computeFunction(numericCast<int64_t>(argTag, argValue), result);
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberDouble: {
                double result;
                TrigFunction::computeFunction(numericCast<double>(argTag, argValue), result);
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberDecimal: {
                Decimal128 result;
                TrigFunction::computeFunction(numericCast<Decimal128>(argTag, argValue), result);
                auto [resTag, resValue] = value::makeCopyDecimal(result);
                return {true, resTag, resValue};
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
    return {false, value::TypeTags::Nothing, 0};
}
}  // namespace


namespace {
void setNonDecimalTotal(TypeTags nonDecimalTotalTag,
                        const DoubleDoubleSummation& nonDecimalTotal,
                        Array* arr) {
    auto [sum, addend] = nonDecimalTotal.getDoubleDouble();
    arr->setAt(AggSumValueElems::kNonDecimalTotalTag,
               nonDecimalTotalTag,
               value::bitcastFrom<int32_t>(0.0));
    arr->setAt(AggSumValueElems::kNonDecimalTotalSum,
               TypeTags::NumberDouble,
               value::bitcastFrom<double>(sum));
    arr->setAt(AggSumValueElems::kNonDecimalTotalAddend,
               TypeTags::NumberDouble,
               value::bitcastFrom<double>(addend));
}

void setDecimalTotal(TypeTags nonDecimalTotalTag,
                     const DoubleDoubleSummation& nonDecimalTotal,
                     const Decimal128& decimalTotal,
                     Array* arr) {
    setNonDecimalTotal(nonDecimalTotalTag, nonDecimalTotal, arr);
    // We don't need to use 'ValueGuard' for decimal because we've already allocated enough storage
    // and Array::push_back() is guaranteed to not throw.
    auto [tag, val] = makeCopyDecimal(decimalTotal);
    if (arr->size() < AggSumValueElems::kMaxSizeOfArray) {
        arr->push_back(tag, val);
    } else {
        arr->setAt(AggSumValueElems::kDecimalTotal, tag, val);
    }
}

void addNonDecimal(TypeTags tag, Value val, DoubleDoubleSummation& nonDecimalTotal) {
    switch (tag) {
        case TypeTags::NumberInt64:
            nonDecimalTotal.addLong(value::bitcastTo<int64_t>(val));
            break;
        case TypeTags::NumberInt32:
            nonDecimalTotal.addInt(value::bitcastTo<int32_t>(val));
            break;
        case TypeTags::NumberDouble:
            nonDecimalTotal.addDouble(value::bitcastTo<double>(val));
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(5755316);
    }
}

void setStdDevArray(value::Value count, value::Value mean, value::Value m2, Array* arr) {
    arr->setAt(AggStdDevValueElems::kCount, value::TypeTags::NumberInt64, count);
    arr->setAt(AggStdDevValueElems::kRunningMean, value::TypeTags::NumberDouble, mean);
    arr->setAt(AggStdDevValueElems::kRunningM2, value::TypeTags::NumberDouble, m2);
}
}  // namespace

void ByteCode::aggDoubleDoubleSumImpl(value::Array* accumulator,
                                      value::TypeTags rhsTag,
                                      value::Value rhsValue) {
    if (!isNumber(rhsTag)) {
        return;
    }

    tassert(5755310,
            str::stream() << "The result slot must have at least "
                          << AggSumValueElems::kMaxSizeOfArray - 1
                          << " elements but got: " << accumulator->size(),
            accumulator->size() >= AggSumValueElems::kMaxSizeOfArray - 1);

    // Only uses tag information from the kNonDecimalTotalTag element.
    auto [nonDecimalTotalTag, _] = accumulator->getAt(AggSumValueElems::kNonDecimalTotalTag);
    tassert(5755311,
            "The nonDecimalTag can't be NumberDecimal",
            nonDecimalTotalTag != TypeTags::NumberDecimal);
    // Only uses values from the kNonDecimalTotalSum/kNonDecimalTotalAddend elements.
    auto [sumTag, sum] = accumulator->getAt(AggSumValueElems::kNonDecimalTotalSum);
    auto [addendTag, addend] = accumulator->getAt(AggSumValueElems::kNonDecimalTotalAddend);
    tassert(5755312,
            "The sum and addend must be NumberDouble",
            sumTag == addendTag && sumTag == TypeTags::NumberDouble);

    // We're guaranteed to always have a valid nonDecimalTotal value.
    auto nonDecimalTotal = DoubleDoubleSummation::create(value::bitcastTo<double>(sum),
                                                         value::bitcastTo<double>(addend));

    if (auto nElems = accumulator->size(); nElems < AggSumValueElems::kMaxSizeOfArray) {
        // We haven't seen any decimal value so far.
        if (auto totalTag = getWidestNumericalType(nonDecimalTotalTag, rhsTag);
            totalTag == TypeTags::NumberDecimal) {
            // Hit the first decimal. Start storing sum of decimal values into the 'kDecimalTotal'
            // element and sum of non-decimal values into 'kNonDecimalXXX' elements.
            tassert(
                5755313, "The arg value must be NumberDecimal", rhsTag == TypeTags::NumberDecimal);

            setDecimalTotal(nonDecimalTotalTag,
                            nonDecimalTotal,
                            value::bitcastTo<Decimal128>(rhsValue),
                            accumulator);
        } else {
            addNonDecimal(rhsTag, rhsValue, nonDecimalTotal);
            setNonDecimalTotal(totalTag, nonDecimalTotal, accumulator);
        }
    } else {
        // We've already seen a decimal value so are using both the 'kDecimalTotal' element and the
        // 'kNonDecimalXXX' elements in the accumulator.
        tassert(5755314,
                str::stream() << "The result slot must have at most "
                              << AggSumValueElems::kMaxSizeOfArray
                              << " elements but got: " << accumulator->size(),
                nElems == AggSumValueElems::kMaxSizeOfArray);
        auto [decimalTotalTag, decimalTotalVal] =
            accumulator->getAt(AggSumValueElems::kDecimalTotal);
        tassert(5755315,
                "The decimalTotal must be NumberDecimal",
                decimalTotalTag == TypeTags::NumberDecimal);

        auto decimalTotal = value::bitcastTo<Decimal128>(decimalTotalVal);
        if (rhsTag == TypeTags::NumberDecimal) {
            decimalTotal = decimalTotal.add(value::bitcastTo<Decimal128>(rhsValue));
        } else {
            nonDecimalTotalTag = getWidestNumericalType(nonDecimalTotalTag, rhsTag);
            addNonDecimal(rhsTag, rhsValue, nonDecimalTotal);
        }

        setDecimalTotal(nonDecimalTotalTag, nonDecimalTotal, decimalTotal, accumulator);
    }
}  // ByteCode::aggDoubleDoubleSumImpl

void ByteCode::aggMergeDoubleDoubleSumsImpl(value::Array* accumulator,
                                            value::TypeTags rhsTag,
                                            value::Value rhsValue) {
    auto [accumWidestType, _1] = accumulator->getAt(AggSumValueElems::kNonDecimalTotalTag);

    tassert(7039532, "value must be of type 'Array'", rhsTag == value::TypeTags::Array);
    value::Array* nextDoubleDoubleArr = value::getArrayView(rhsValue);

    tassert(7039533,
            "array does not have enough elements",
            nextDoubleDoubleArr->size() >= AggSumValueElems::kMaxSizeOfArray - 1);

    // First aggregate the non-decimal sum, then the non-decimal addend. Both should be doubles.
    auto [sumTag, sum] = nextDoubleDoubleArr->getAt(AggSumValueElems::kNonDecimalTotalSum);
    tassert(7039534, "expected 'NumberDouble'", sumTag == value::TypeTags::NumberDouble);
    aggDoubleDoubleSumImpl(accumulator, sumTag, sum);

    auto [addendTag, addend] = nextDoubleDoubleArr->getAt(AggSumValueElems::kNonDecimalTotalAddend);
    tassert(7039535, "expected 'NumberDouble'", addendTag == value::TypeTags::NumberDouble);
    // There is a special case when the 'sum' is infinite and the 'addend' is NaN. This DoubleDouble
    // value represents infinity, not NaN. Therefore, we avoid incorporating the NaN 'addend' value
    // into the sum.
    if (std::isfinite(value::bitcastTo<double>(sum)) ||
        !std::isnan(value::bitcastTo<double>(addend))) {
        aggDoubleDoubleSumImpl(accumulator, addendTag, addend);
    }

    // Determine the widest non-decimal type that we've seen so far, and set the accumulator state
    // accordingly. We do this after computing the sums, since 'aggDoubleDoubleSumImpl()' will
    // set the widest type to 'NumberDouble' when we call it above.
    auto [newValWidestType, _2] = nextDoubleDoubleArr->getAt(AggSumValueElems::kNonDecimalTotalTag);
    tassert(
        7039536, "unexpected 'NumberDecimal'", newValWidestType != value::TypeTags::NumberDecimal);
    tassert(
        7039537, "unexpected 'NumberDecimal'", accumWidestType != value::TypeTags::NumberDecimal);
    auto widestType = getWidestNumericalType(newValWidestType, accumWidestType);
    accumulator->setAt(
        AggSumValueElems::kNonDecimalTotalTag, widestType, value::bitcastFrom<int32_t>(0));

    // If there's a decimal128 sum as part of the incoming DoubleDouble sum, incorporate it into the
    // accumulator.
    if (nextDoubleDoubleArr->size() == AggSumValueElems::kMaxSizeOfArray) {
        auto [decimalTotalTag, decimalTotalVal] =
            nextDoubleDoubleArr->getAt(AggSumValueElems::kDecimalTotal);
        tassert(7039538,
                "The decimalTotal must be 'NumberDecimal'",
                decimalTotalTag == TypeTags::NumberDecimal);
        aggDoubleDoubleSumImpl(accumulator, decimalTotalTag, decimalTotalVal);
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::aggDoubleDoubleSumFinalizeImpl(
    value::Array* arr) {
    tassert(5755321,
            str::stream() << "The result slot must have at least "
                          << AggSumValueElems::kMaxSizeOfArray - 1
                          << " elements but got: " << arr->size(),
            arr->size() >= AggSumValueElems::kMaxSizeOfArray - 1);

    auto nonDecimalTotalTag = arr->getAt(AggSumValueElems::kNonDecimalTotalTag).first;
    tassert(5755322,
            "The nonDecimalTag can't be NumberDecimal",
            nonDecimalTotalTag != value::TypeTags::NumberDecimal);
    auto [sumTag, sum] = arr->getAt(AggSumValueElems::kNonDecimalTotalSum);
    auto [addendTag, addend] = arr->getAt(AggSumValueElems::kNonDecimalTotalAddend);
    tassert(5755323,
            "The sum and addend must be NumberDouble",
            sumTag == addendTag && sumTag == value::TypeTags::NumberDouble);

    // We're guaranteed to always have a valid nonDecimalTotal value.
    auto nonDecimalTotal = DoubleDoubleSummation::create(value::bitcastTo<double>(sum),
                                                         value::bitcastTo<double>(addend));

    if (auto nElems = arr->size(); nElems < AggSumValueElems::kMaxSizeOfArray) {
        // We've not seen any decimal value.
        switch (nonDecimalTotalTag) {
            case value::TypeTags::NumberInt32:
            case value::TypeTags::NumberInt64:
                if (nonDecimalTotal.fitsLong()) {
                    auto longVal = nonDecimalTotal.getLong();
                    if (int intVal = longVal;
                        nonDecimalTotalTag == value::TypeTags::NumberInt32 && intVal == longVal) {
                        return {true,
                                value::TypeTags::NumberInt32,
                                value::bitcastFrom<int32_t>(intVal)};
                    } else {
                        return {true,
                                value::TypeTags::NumberInt64,
                                value::bitcastFrom<int64_t>(longVal)};
                    }
                }

                // Sum doesn't fit a NumberLong, so return a NumberDouble instead.
                [[fallthrough]];
            case value::TypeTags::NumberDouble:
                return {true,
                        value::TypeTags::NumberDouble,
                        value::bitcastFrom<double>(nonDecimalTotal.getDouble())};
            default:
                MONGO_UNREACHABLE_TASSERT(5755324);
        }
    } else {
        // We've seen a decimal value.
        tassert(5755325,
                str::stream() << "The result slot must have at most "
                              << AggSumValueElems::kMaxSizeOfArray
                              << " elements but got: " << arr->size(),
                nElems == AggSumValueElems::kMaxSizeOfArray);
        auto [decimalTotalTag, decimalTotalVal] = arr->getAt(AggSumValueElems::kDecimalTotal);
        tassert(5755326,
                "The decimalTotal must be NumberDecimal",
                decimalTotalTag == value::TypeTags::NumberDecimal);

        auto decimalTotal = value::bitcastTo<Decimal128>(decimalTotalVal);
        auto [tag, val] = value::makeCopyDecimal(decimalTotal.add(nonDecimalTotal.getDecimal()));
        return {true, tag, val};
    }
}


void ByteCode::aggStdDevImpl(value::Array* arr, value::TypeTags rhsTag, value::Value rhsValue) {
    if (!isNumber(rhsTag)) {
        return;
    }

    auto [countTag, countVal] = arr->getAt(AggStdDevValueElems::kCount);
    tassert(5755201, "The count must be of type NumberInt64", countTag == TypeTags::NumberInt64);

    auto [meanTag, meanVal] = arr->getAt(AggStdDevValueElems::kRunningMean);
    auto [m2Tag, m2Val] = arr->getAt(AggStdDevValueElems::kRunningM2);
    tassert(5755202,
            "The mean and m2 must be of type Double",
            m2Tag == meanTag && meanTag == TypeTags::NumberDouble);

    double inputDouble = 0.0;
    // Within our query execution engine, $stdDevPop and $stdDevSamp do not maintain the precision
    // of decimal types and converts all values to double. We do this here by converting
    // NumberDecimal to Decimal128 and then extract a double value from it.
    if (rhsTag == value::TypeTags::NumberDecimal) {
        auto decimal = value::bitcastTo<Decimal128>(rhsValue);
        inputDouble = decimal.toDouble();
    } else {
        inputDouble = numericCast<double>(rhsTag, rhsValue);
    }
    auto curVal = value::bitcastFrom<double>(inputDouble);

    auto count = value::bitcastTo<int64_t>(countVal);
    tassert(5755211,
            "The total number of elements must be less than INT64_MAX",
            ++count < std::numeric_limits<int64_t>::max());
    auto newCountVal = value::bitcastFrom<int64_t>(count);

    auto [deltaOwned, deltaTag, deltaVal] =
        genericSub(value::TypeTags::NumberDouble, curVal, value::TypeTags::NumberDouble, meanVal);
    auto [deltaDivCountOwned, deltaDivCountTag, deltaDivCountVal] =
        genericDiv(deltaTag, deltaVal, value::TypeTags::NumberInt64, newCountVal);
    auto [newMeanOwned, newMeanTag, newMeanVal] =
        genericAdd(meanTag, meanVal, deltaDivCountTag, deltaDivCountVal);
    auto [newDeltaOwned, newDeltaTag, newDeltaVal] =
        genericSub(value::TypeTags::NumberDouble, curVal, newMeanTag, newMeanVal);
    auto [deltaMultNewDeltaOwned, deltaMultNewDeltaTag, deltaMultNewDeltaVal] =
        genericMul(deltaTag, deltaVal, newDeltaTag, newDeltaVal);
    auto [newM2Owned, newM2Tag, newM2Val] =
        genericAdd(m2Tag, m2Val, deltaMultNewDeltaTag, deltaMultNewDeltaVal);

    return setStdDevArray(newCountVal, newMeanVal, newM2Val, arr);
}

void ByteCode::aggMergeStdDevsImpl(value::Array* accumulator,
                                   value::TypeTags rhsTag,
                                   value::Value rhsValue) {
    tassert(7039542, "expected value of type 'Array'", rhsTag == value::TypeTags::Array);
    auto nextArr = value::getArrayView(rhsValue);

    tassert(7039543,
            "expected array to have exactly 3 elements",
            accumulator->size() == AggStdDevValueElems::kSizeOfArray);
    tassert(7039544,
            "expected array to have exactly 3 elements",
            nextArr->size() == AggStdDevValueElems::kSizeOfArray);

    auto [newCountTag, newCountVal] = nextArr->getAt(AggStdDevValueElems::kCount);
    tassert(7039545, "expected 64-bit int", newCountTag == value::TypeTags::NumberInt64);
    int64_t newCount = value::bitcastTo<int64_t>(newCountVal);

    // If the incoming partial aggregate has a count of zero, then it represents the partial
    // standard deviation of no data points. This means that it can be safely ignored, and we return
    // the accumulator as is.
    if (newCount == 0) {
        return;
    }

    auto [oldCountTag, oldCountVal] = accumulator->getAt(AggStdDevValueElems::kCount);
    tassert(7039546, "expected 64-bit int", oldCountTag == value::TypeTags::NumberInt64);
    int64_t oldCount = value::bitcastTo<int64_t>(oldCountVal);

    auto [oldMeanTag, oldMeanVal] = accumulator->getAt(AggStdDevValueElems::kRunningMean);
    tassert(7039547, "expected double", oldMeanTag == value::TypeTags::NumberDouble);
    double oldMean = value::bitcastTo<double>(oldMeanVal);

    auto [newMeanTag, newMeanVal] = nextArr->getAt(AggStdDevValueElems::kRunningMean);
    tassert(7039548, "expected double", newMeanTag == value::TypeTags::NumberDouble);
    double newMean = value::bitcastTo<double>(newMeanVal);

    auto [oldM2Tag, oldM2Val] = accumulator->getAt(AggStdDevValueElems::kRunningM2);
    tassert(7039531, "expected double", oldM2Tag == value::TypeTags::NumberDouble);
    double oldM2 = value::bitcastTo<double>(oldM2Val);

    auto [newM2Tag, newM2Val] = nextArr->getAt(AggStdDevValueElems::kRunningM2);
    tassert(7039541, "expected double", newM2Tag == value::TypeTags::NumberDouble);
    double newM2 = value::bitcastTo<double>(newM2Val);

    const double delta = newMean - oldMean;
    // We've already handled the case where 'newCount' is zero above. This means that 'totalCount'
    // must be positive, and prevents us from ever dividing by zero in the subsequent calculation.
    int64_t totalCount = oldCount + newCount;
    // If oldCount is zero, we should avoid needless calcuations, because they may damage floating
    // point precision.
    if (delta != 0 && oldCount != 0) {
        newMean = ((oldCount * oldMean) + (newCount * newMean)) / totalCount;
        newM2 += delta * delta *
            (static_cast<double>(oldCount) * static_cast<double>(newCount) / totalCount);
    }
    newM2 += oldM2;

    setStdDevArray(value::bitcastFrom<int64_t>(totalCount),
                   value::bitcastFrom<double>(newMean),
                   value::bitcastFrom<double>(newM2),
                   accumulator);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::aggStdDevFinalizeImpl(
    value::Value fieldValue, bool isSamp) {
    auto arr = value::getArrayView(fieldValue);

    auto [countTag, countVal] = arr->getAt(AggStdDevValueElems::kCount);
    tassert(5755207, "The count must be a NumberInt64", countTag == value::TypeTags::NumberInt64);

    auto count = value::bitcastTo<int64_t>(countVal);

    if (count == 0) {
        return {true, value::TypeTags::Null, 0};
    }

    if (isSamp && count == 1) {
        return {true, value::TypeTags::Null, 0};
    }

    auto [m2Tag, m2] = arr->getAt(AggStdDevValueElems::kRunningM2);
    tassert(5755208,
            "The m2 value must be of type NumberDouble",
            m2Tag == value::TypeTags::NumberDouble);
    auto m2Double = value::bitcastTo<double>(m2);
    auto variance = isSamp ? (m2Double / (count - 1)) : (m2Double / count);
    auto stdDev = sqrt(variance);

    return {true, value::TypeTags::NumberDouble, value::bitcastFrom<double>(stdDev)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericDiv(value::TypeTags lhsTag,
                                                                    value::Value lhsValue,
                                                                    value::TypeTags rhsTag,
                                                                    value::Value rhsValue) {
    auto assertNonZero = [](bool nonZero) {
        uassert(4848401, "can't $divide by zero", nonZero);
    };

    if (value::isNumber(lhsTag) && value::isNumber(rhsTag)) {
        switch (getWidestNumericalType(lhsTag, rhsTag)) {
            case value::TypeTags::NumberInt32: {
                assertNonZero(numericCast<double>(rhsTag, rhsValue) != 0);
                auto result =
                    numericCast<double>(lhsTag, lhsValue) / numericCast<double>(rhsTag, rhsValue);
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberInt64: {
                assertNonZero(numericCast<double>(rhsTag, rhsValue) != 0);
                auto result =
                    numericCast<double>(lhsTag, lhsValue) / numericCast<double>(rhsTag, rhsValue);
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberDouble: {
                assertNonZero(numericCast<double>(rhsTag, rhsValue) != 0);
                auto result =
                    numericCast<double>(lhsTag, lhsValue) / numericCast<double>(rhsTag, rhsValue);
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberDecimal: {
                assertNonZero(!numericCast<Decimal128>(rhsTag, rhsValue).isZero());
                auto result = numericCast<Decimal128>(lhsTag, lhsValue)
                                  .divide(numericCast<Decimal128>(rhsTag, rhsValue));
                auto [tag, val] = value::makeCopyDecimal(result);
                return {true, tag, val};
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericIDiv(value::TypeTags lhsTag,
                                                                     value::Value lhsValue,
                                                                     value::TypeTags rhsTag,
                                                                     value::Value rhsValue) {
    auto assertNonZero = [](bool nonZero) {
        uassert(4848402, "can't $divide by zero", nonZero);
    };

    if (value::isNumber(lhsTag) && value::isNumber(rhsTag)) {
        switch (getWidestNumericalType(lhsTag, rhsTag)) {
            case value::TypeTags::NumberInt32: {
                assertNonZero(numericCast<int32_t>(rhsTag, rhsValue) != 0);
                auto result =
                    numericCast<int32_t>(lhsTag, lhsValue) / numericCast<int32_t>(rhsTag, rhsValue);
                return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(result)};
            }
            case value::TypeTags::NumberInt64: {
                assertNonZero(numericCast<int64_t>(rhsTag, rhsValue) != 0);
                auto result =
                    numericCast<int64_t>(lhsTag, lhsValue) / numericCast<int64_t>(rhsTag, rhsValue);
                return {false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result)};
            }
            case value::TypeTags::NumberDouble: {
                auto lhs = representAs<int64_t>(numericCast<double>(lhsTag, lhsValue));
                auto rhs = representAs<int64_t>(numericCast<double>(rhsTag, rhsValue));

                if (!lhs || !rhs) {
                    return {false, value::TypeTags::Nothing, 0};
                }
                assertNonZero(*rhs != 0);
                auto result = *lhs / *rhs;

                return {false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result)};
            }
            case value::TypeTags::NumberDecimal: {
                auto lhs = representAs<int64_t>(numericCast<Decimal128>(lhsTag, lhsValue));
                auto rhs = representAs<int64_t>(numericCast<Decimal128>(rhsTag, rhsValue));

                if (!lhs || !rhs) {
                    return {false, value::TypeTags::Nothing, 0};
                }
                assertNonZero(*rhs != 0);
                auto result = *lhs / *rhs;

                return {false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result)};
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericMod(value::TypeTags lhsTag,
                                                                    value::Value lhsValue,
                                                                    value::TypeTags rhsTag,
                                                                    value::Value rhsValue) {
    auto assertNonZero = [](bool nonZero) {
        uassert(4848403, "can't $mod by zero", nonZero);
    };

    if (value::isNumber(lhsTag) && value::isNumber(rhsTag)) {
        switch (getWidestNumericalType(lhsTag, rhsTag)) {
            case value::TypeTags::NumberInt32: {
                assertNonZero(numericCast<int32_t>(rhsTag, rhsValue) != 0);
                auto result = overflow::safeMod(numericCast<int32_t>(lhsTag, lhsValue),
                                                numericCast<int32_t>(rhsTag, rhsValue));
                return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(result)};
            }
            case value::TypeTags::NumberInt64: {
                assertNonZero(numericCast<int64_t>(rhsTag, rhsValue) != 0);
                auto result = overflow::safeMod(numericCast<int64_t>(lhsTag, lhsValue),
                                                numericCast<int64_t>(rhsTag, rhsValue));
                return {false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result)};
            }
            case value::TypeTags::NumberDouble: {
                assertNonZero(numericCast<double>(rhsTag, rhsValue) != 0);
                auto result = fmod(numericCast<double>(lhsTag, lhsValue),
                                   numericCast<double>(rhsTag, rhsValue));
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberDecimal: {
                assertNonZero(!numericCast<Decimal128>(rhsTag, rhsValue).isZero());
                auto result = numericCast<Decimal128>(lhsTag, lhsValue)
                                  .modulo(numericCast<Decimal128>(rhsTag, rhsValue));
                auto [tag, val] = value::makeCopyDecimal(result);
                return {true, tag, val};
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericAbs(value::TypeTags operandTag,
                                                                    value::Value operandValue) {
    switch (operandTag) {
        case value::TypeTags::NumberInt32: {
            auto operand = value::bitcastTo<int32_t>(operandValue);
            if (operand == std::numeric_limits<int32_t>::min()) {
                return {false,
                        value::TypeTags::NumberInt64,
                        value::bitcastFrom<int64_t>(-int64_t{operand})};
            }

            return {false,
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(std::abs(operand))};
        }
        case value::TypeTags::NumberInt64: {
            auto operand = value::bitcastTo<int64_t>(operandValue);
            if (operand == std::numeric_limits<int64_t>::min()) {
                // Absolute value of the minimum int64_t value does not fit in any integer type.
                return {false, value::TypeTags::Nothing, 0};
            }
            return {false,
                    value::TypeTags::NumberInt64,
                    value::bitcastFrom<int64_t>(std::abs(operand))};
        }
        case value::TypeTags::NumberDouble: {
            auto operand = value::bitcastTo<double>(operandValue);
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(std::abs(operand))};
        }
        case value::TypeTags::NumberDecimal: {
            auto operand = value::bitcastTo<Decimal128>(operandValue);
            auto [tag, value] = value::makeCopyDecimal(operand.toAbs());
            return {true, tag, value};
        }
        default:
            return {false, value::TypeTags::Nothing, 0};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericCeil(value::TypeTags operandTag,
                                                                     value::Value operandValue) {
    if (isNumber(operandTag)) {
        switch (operandTag) {
            case value::TypeTags::NumberDouble: {
                auto result = std::ceil(value::bitcastTo<double>(operandValue));
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberDecimal: {
                auto result =
                    value::bitcastTo<Decimal128>(operandValue)
                        .quantize(Decimal128::kNormalizedZero, Decimal128::kRoundTowardPositive);
                auto [tag, value] = value::makeCopyDecimal(result);
                return {true, tag, value};
            }
            case value::TypeTags::NumberInt32:
            case value::TypeTags::NumberInt64:
                // Ceil on integer values is the identity function.
                return {false, operandTag, operandValue};
            default:
                MONGO_UNREACHABLE;
        }
    }

    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericFloor(value::TypeTags operandTag,
                                                                      value::Value operandValue) {
    if (isNumber(operandTag)) {
        switch (operandTag) {
            case value::TypeTags::NumberDouble: {
                auto result = std::floor(value::bitcastTo<double>(operandValue));
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberDecimal: {
                auto result =
                    value::bitcastTo<Decimal128>(operandValue)
                        .quantize(Decimal128::kNormalizedZero, Decimal128::kRoundTowardNegative);
                auto [tag, value] = value::makeCopyDecimal(result);
                return {true, tag, value};
            }
            case value::TypeTags::NumberInt32:
            case value::TypeTags::NumberInt64:
                // Floor on integer values is the identity function.
                return {false, operandTag, operandValue};
            default:
                MONGO_UNREACHABLE;
        }
    }

    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericExp(value::TypeTags operandTag,
                                                                    value::Value operandValue) {
    switch (operandTag) {
        case value::TypeTags::NumberDouble: {
            auto result = exp(value::bitcastTo<double>(operandValue));
            return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
        }
        case value::TypeTags::NumberDecimal: {
            auto result = value::bitcastTo<Decimal128>(operandValue).exp();
            auto [tag, value] = value::makeCopyDecimal(result);
            return {true, tag, value};
        }
        case value::TypeTags::NumberInt32:
        case value::TypeTags::NumberInt64: {
            auto operand = (operandTag == value::TypeTags::NumberInt32)
                ? static_cast<double>(value::bitcastTo<int32_t>(operandValue))
                : static_cast<double>(value::bitcastTo<int64_t>(operandValue));
            return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(exp(operand))};
        }
        default:
            return {false, value::TypeTags::Nothing, 0};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericLn(value::TypeTags operandTag,
                                                                   value::Value operandValue) {
    switch (operandTag) {
        case value::TypeTags::NumberDouble: {
            auto operand = value::bitcastTo<double>(operandValue);
            if (operand <= 0 && !std::isnan(operand)) {
                // Logarithms are only defined on the domain of positive numbers and NaN. NaN is a
                // legal input to ln(), returning NaN.
                return {false, value::TypeTags::Nothing, 0};
            }
            // Note: NaN is a legal input to log(), returning NaN.
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(std::log(operand))};
        }
        case value::TypeTags::NumberDecimal: {
            auto operand = value::bitcastTo<Decimal128>(operandValue);
            if (!operand.isGreater(Decimal128::kNormalizedZero) && !operand.isNaN()) {
                return {false, value::TypeTags::Nothing, 0};
            }
            auto operandLn = operand.log();

            auto [tag, value] = value::makeCopyDecimal(operandLn);
            return {true, tag, value};
        }
        case value::TypeTags::NumberInt32:
        case value::TypeTags::NumberInt64: {
            auto operand = (operandTag == value::TypeTags::NumberInt32)
                ? static_cast<double>(value::bitcastTo<int32_t>(operandValue))
                : static_cast<double>(value::bitcastTo<int64_t>(operandValue));
            if (operand <= 0 && !std::isnan(operand)) {
                return {false, value::TypeTags::Nothing, 0};
            }
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(std::log(operand))};
        }
        default:
            return {false, value::TypeTags::Nothing, 0};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericLog10(value::TypeTags operandTag,
                                                                      value::Value operandValue) {
    switch (operandTag) {
        case value::TypeTags::NumberDouble: {
            auto operand = value::bitcastTo<double>(operandValue);
            if (operand <= 0 && !std::isnan(operand)) {
                // Logarithms are only defined on the domain of positive numbers and NaN. NaN is a
                // legal input to log10(), returning NaN.
                return {false, value::TypeTags::Nothing, 0};
            }
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(std::log10(operand))};
        }
        case value::TypeTags::NumberDecimal: {
            auto operand = value::bitcastTo<Decimal128>(operandValue);
            if (!operand.isGreater(Decimal128::kNormalizedZero) && !operand.isNaN()) {
                return {false, value::TypeTags::Nothing, 0};
            }
            auto operandLog10 = operand.log10();

            auto [tag, value] = value::makeCopyDecimal(operandLog10);
            return {true, tag, value};
        }
        case value::TypeTags::NumberInt32:
        case value::TypeTags::NumberInt64: {
            auto operand = (operandTag == value::TypeTags::NumberInt32)
                ? static_cast<double>(value::bitcastTo<int32_t>(operandValue))
                : static_cast<double>(value::bitcastTo<int64_t>(operandValue));
            if (operand <= 0 && !std::isnan(operand)) {
                return {false, value::TypeTags::Nothing, 0};
            }
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(std::log10(operand))};
        }
        default:
            return {false, value::TypeTags::Nothing, 0};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericSqrt(value::TypeTags operandTag,
                                                                     value::Value operandValue) {
    switch (operandTag) {
        case value::TypeTags::NumberDouble: {
            auto operand = value::bitcastTo<double>(operandValue);
            if (operand < 0 && !std::isnan(operand)) {
                // Sqrt is only defined in the domain of non-negative numbers and NaN. NaN is a
                // legal input to sqrt(), returning NaN.
                return {false, value::TypeTags::Nothing, 0};
            }
            // Note: NaN is a legal input to sqrt(), returning NaN.
            return {
                false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(sqrt(operand))};
        }
        case value::TypeTags::NumberDecimal: {
            auto operand = value::bitcastTo<Decimal128>(operandValue);
            if (operand.isLess(Decimal128::kNormalizedZero) && !operand.isNaN()) {
                return {false, value::TypeTags::Nothing, 0};
            }
            auto [tag, value] = value::makeCopyDecimal(operand.sqrt());
            return {true, tag, value};
        }
        case value::TypeTags::NumberInt32:
        case value::TypeTags::NumberInt64: {
            auto operand = (operandTag == value::TypeTags::NumberInt32)
                ? static_cast<double>(value::bitcastTo<int32_t>(operandValue))
                : static_cast<double>(value::bitcastTo<int64_t>(operandValue));
            if (operand < 0 && !std::isnan(operand)) {
                return {false, value::TypeTags::Nothing, 0};
            }
            return {
                false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(sqrt(operand))};
        }
        default:
            return {false, value::TypeTags::Nothing, 0};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericPow(value::TypeTags baseTag,
                                                                    value::Value baseValue,
                                                                    value::TypeTags exponentTag,
                                                                    value::Value exponentValue) {

    // pow supports only numeric values
    if (!value::isNumber(baseTag) || !value::isNumber(exponentTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    if (baseTag == value::TypeTags::NumberDecimal ||
        exponentTag == value::TypeTags::NumberDecimal) {
        auto baseDecimal = numericCast<Decimal128>(baseTag, baseValue);
        auto exponenetDecimal = numericCast<Decimal128>(exponentTag, exponentValue);
        if (baseDecimal == Decimal128("0") && exponenetDecimal < Decimal128("0")) {
            return {false, value::TypeTags::Nothing, 0};
        }
        auto result = baseDecimal.power(exponenetDecimal);
        auto [resTag, resValue] = value::makeCopyDecimal(result);
        return {true, resTag, resValue};
    }

    // If either argument is a double, return a double.
    if (baseTag == value::TypeTags::NumberDouble || exponentTag == value::TypeTags::NumberDouble) {
        auto baseDouble = numericCast<double>(baseTag, baseValue);
        auto exponentDouble = numericCast<double>(exponentTag, exponentValue);
        if (baseDouble == 0 && exponentDouble < 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        return {false,
                value::TypeTags::NumberDouble,
                value::bitcastFrom<double>(std::pow(baseDouble, exponentDouble))};
    }

    auto baseLong = value::bitcastTo<int64_t>(baseValue);
    auto exponentLong = value::bitcastTo<int64_t>(exponentValue);
    if (baseLong == 0 && exponentLong < 0) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // If both values are int and the res fits in int then return int, otherwise return long
    const auto formatResult = [baseTag, exponentTag](int64_t longRes) {
        FastTuple<bool, value::TypeTags, value::Value> res = {
            false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(longRes)};

        if (baseTag == value::TypeTags::NumberInt32 &&
            exponentTag == value::TypeTags::NumberInt32) {

            int32_t intRes = static_cast<int32_t>(longRes);
            if (intRes == longRes) {
                // should be an int since all arguments were int and it fits
                res = {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(intRes)};
            }
        }

        return res;
    };

    // Avoid doing repeated multiplication or using std::pow if the base is -1, 0, or 1.
    if (baseLong == 0) {
        if (exponentLong == 0) {
            // 0^0 = 1.
            return formatResult(1);
        } else if (exponentLong > 0) {
            // 0^x where x > 0 is 0.
            return formatResult(0);
        }

        // We should have checked earlier that 0 to a negative power is banned.
        MONGO_UNREACHABLE;
    } else if (baseLong == 1) {
        // 1^x = 1.
        return formatResult(1);
    } else if (baseLong == -1) {
        // ... -1^-4 = -1^-2 = -1^0 = -1^2 = -1^4 = -1^6 ... = 1
        // ... -1^-3 = -1^-1 = -1^1 = -1^3 = -1^5 = -1^7 ... = -1
        return formatResult((exponentLong % 2 == 0) ? 1 : -1);
    } else if (exponentLong > 63 || exponentLong < 0) {
        // If the base is not 0, 1, or -1 and the exponent is too large, or negative,
        // the result cannot be represented as a long.
        return {false,
                value::TypeTags::NumberDouble,
                value::bitcastFrom<double>(std::pow(baseLong, exponentLong))};
    }

    // It's still possible that the result cannot be represented as a long. If that's the case,
    // return a double.
    if (!representableAsLong(baseLong, exponentLong)) {
        return {false,
                value::TypeTags::NumberDouble,
                value::bitcastFrom<double>(std::pow(baseLong, exponentLong))};
    }


    // Use repeated multiplication, since pow() casts args to doubles which could result in
    // loss of precision if arguments are very large.
    const auto computeWithRepeatedMultiplication = [](int64_t base, int64_t exp) {
        int64_t result = 1;

        while (exp > 1) {
            if (exp % 2 == 1) {
                result *= base;
                exp--;
            }
            // 'exp' is now guaranteed to be even.
            base *= base;
            exp /= 2;
        }

        if (exp) {
            invariant(exp == 1);
            result *= base;
        }

        return result;
    };

    return formatResult(computeWithRepeatedMultiplication(baseLong, exponentLong));
}

std::pair<value::TypeTags, value::Value> ByteCode::genericNot(value::TypeTags tag,
                                                              value::Value value) {
    if (tag == value::TypeTags::Boolean) {
        return {tag, value::bitcastFrom<bool>(!value::bitcastTo<bool>(value))};
    } else {
        return {value::TypeTags::Nothing, 0};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericAcos(value::TypeTags argTag,
                                                                     value::Value argValue) {
    return genericTrigonometricFun<Acos>(argTag, argValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericAcosh(value::TypeTags argTag,
                                                                      value::Value argValue) {
    return genericTrigonometricFun<Acosh>(argTag, argValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericAsin(value::TypeTags argTag,
                                                                     value::Value argValue) {
    return genericTrigonometricFun<Asin>(argTag, argValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericAsinh(value::TypeTags argTag,
                                                                      value::Value argValue) {
    return genericTrigonometricFun<Asinh>(argTag, argValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericAtan(value::TypeTags argTag,
                                                                     value::Value argValue) {
    return genericTrigonometricFun<Atan>(argTag, argValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericAtanh(value::TypeTags argTag,
                                                                      value::Value argValue) {
    return genericTrigonometricFun<Atanh>(argTag, argValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericAtan2(value::TypeTags argTag1,
                                                                      value::Value argValue1,
                                                                      value::TypeTags argTag2,
                                                                      value::Value argValue2) {
    if (value::isNumber(argTag1) && value::isNumber(argTag2)) {
        switch (getWidestNumericalType(argTag1, argTag2)) {
            case value::TypeTags::NumberInt32:
            case value::TypeTags::NumberInt64:
            case value::TypeTags::NumberDouble: {
                auto result = std::atan2(numericCast<double>(argTag1, argValue1),
                                         numericCast<double>(argTag2, argValue2));
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberDecimal: {
                auto result = numericCast<Decimal128>(argTag1, argValue1)
                                  .atan2(numericCast<Decimal128>(argTag2, argValue2));
                auto [resTag, resValue] = value::makeCopyDecimal(result);
                return {true, resTag, resValue};
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericCos(value::TypeTags argTag,
                                                                    value::Value argValue) {
    return genericTrigonometricFun<Cos>(argTag, argValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericCosh(value::TypeTags argTag,
                                                                     value::Value argValue) {
    return genericTrigonometricFun<Cosh>(argTag, argValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericDegreesToRadians(
    value::TypeTags argTag, value::Value argValue) {
    if (value::isNumber(argTag)) {
        switch (argTag) {
            case value::TypeTags::NumberInt32:
            case value::TypeTags::NumberInt64:
            case value::TypeTags::NumberDouble: {
                auto result = numericCast<double>(argTag, argValue) * kDoublePiOver180;
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberDecimal: {
                auto result =
                    numericCast<Decimal128>(argTag, argValue).multiply(Decimal128::kPiOver180);
                auto [resTag, resValue] = value::makeCopyDecimal(result);
                return {true, resTag, resValue};
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericRadiansToDegrees(
    value::TypeTags argTag, value::Value argValue) {
    if (value::isNumber(argTag)) {
        switch (argTag) {
            case value::TypeTags::NumberInt32:
            case value::TypeTags::NumberInt64:
            case value::TypeTags::NumberDouble: {
                auto result = numericCast<double>(argTag, argValue) * kDouble180OverPi;
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            case value::TypeTags::NumberDecimal: {
                auto result =
                    numericCast<Decimal128>(argTag, argValue).multiply(Decimal128::k180OverPi);
                auto [resTag, resValue] = value::makeCopyDecimal(result);
                return {true, resTag, resValue};
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericSin(value::TypeTags argTag,
                                                                    value::Value argValue) {
    return genericTrigonometricFun<Sin>(argTag, argValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericSinh(value::TypeTags argTag,
                                                                     value::Value argValue) {
    return genericTrigonometricFun<Sinh>(argTag, argValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericTan(value::TypeTags argTag,
                                                                    value::Value argValue) {
    return genericTrigonometricFun<Tan>(argTag, argValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericTanh(value::TypeTags argTag,
                                                                     value::Value argValue) {
    return genericTrigonometricFun<Tanh>(argTag, argValue);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
