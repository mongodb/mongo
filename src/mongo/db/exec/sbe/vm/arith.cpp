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

#include "mongo/db/exec/sbe/vm/vm.h"

#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace sbe {
namespace vm {

using namespace value;

namespace {

static constexpr double kDoublePi = 3.141592653589793;
static constexpr double kDoublePiOver180 = kDoublePi / 180.0;
static constexpr double kDouble180OverPi = 180.0 / kDoublePi;

/**
 * The addition operation used by genericArithmeticOp.
 */
struct Addition {
    /**
     * Returns true if the operation failed (overflow).
     */
    template <typename T>
    static bool doOperation(const T& lhs, const T& rhs, T& result) {
        if constexpr (std::is_same_v<T, Decimal128>) {
            result = lhs.add(rhs);

            // We do not check overflows with Decimal128.
            return false;
        } else if constexpr (std::is_same_v<T, double>) {
            result = lhs + rhs;

            // We do not check overflows with double.
            return false;
        } else {
            return overflow::add(lhs, rhs, &result);
        }
    }
};

/**
 * The subtraction operation used by genericArithmeticOp.
 */
struct Subtraction {
    /**
     * Returns true if the operation failed (overflow).
     */
    template <typename T>
    static bool doOperation(const T& lhs, const T& rhs, T& result) {
        if constexpr (std::is_same_v<T, Decimal128>) {
            result = lhs.subtract(rhs);

            // We do not check overflows with Decimal128.
            return false;
        } else if constexpr (std::is_same_v<T, double>) {
            result = lhs - rhs;

            // We do not check overflows with double.
            return false;
        } else {
            return overflow::sub(lhs, rhs, &result);
        }
    }
};

/**
 * The multiplication operation used by genericArithmeticOp.
 */
struct Multiplication {
    /**
     * Returns true if the operation failed (overflow).
     */
    template <typename T>
    static bool doOperation(const T& lhs, const T& rhs, T& result) {
        if constexpr (std::is_same_v<T, Decimal128>) {
            result = lhs.multiply(rhs);

            // We do not check overflows with Decimal128.
            return false;
        } else if constexpr (std::is_same_v<T, double>) {
            result = lhs * rhs;

            // We do not check overflows with double.
            return false;
        } else {
            return overflow::mul(lhs, rhs, &result);
        }
    }
};

/**
 * This is a simple arithmetic operation templated by the Op parameter. It supports operations on
 * standard numeric types and also operations on the Date type.
 */
template <typename Op>
std::tuple<bool, value::TypeTags, value::Value> genericArithmeticOp(value::TypeTags lhsTag,
                                                                    value::Value lhsValue,
                                                                    value::TypeTags rhsTag,
                                                                    value::Value rhsValue) {
    if (value::isNumber(lhsTag) && value::isNumber(rhsTag)) {
        switch (getWidestNumericalType(lhsTag, rhsTag)) {
            case value::TypeTags::NumberInt32: {
                int32_t result;

                if (!Op::doOperation(numericCast<int32_t>(lhsTag, lhsValue),
                                     numericCast<int32_t>(rhsTag, rhsValue),
                                     result)) {
                    return {
                        false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(result)};
                }
                // The result does not fit into int32_t so fallthru to the wider type.
            }
            case value::TypeTags::NumberInt64: {
                int64_t result;
                if (!Op::doOperation(numericCast<int64_t>(lhsTag, lhsValue),
                                     numericCast<int64_t>(rhsTag, rhsValue),
                                     result)) {
                    return {
                        false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result)};
                }
                // The result does not fit into int64_t so fallthru to the wider type.
            }
            case value::TypeTags::NumberDecimal: {
                Decimal128 result;
                Op::doOperation(numericCast<Decimal128>(lhsTag, lhsValue),
                                numericCast<Decimal128>(rhsTag, rhsValue),
                                result);
                auto [tag, val] = value::makeCopyDecimal(result);
                return {true, tag, val};
            }
            case value::TypeTags::NumberDouble: {
                double result;
                Op::doOperation(numericCast<double>(lhsTag, lhsValue),
                                numericCast<double>(rhsTag, rhsValue),
                                result);
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
            }
            default:
                MONGO_UNREACHABLE;
        }
    } else if (lhsTag == TypeTags::Date || rhsTag == TypeTags::Date) {
        if (isNumber(lhsTag)) {
            int64_t result;
            if (!Op::doOperation(
                    numericCast<int64_t>(lhsTag, lhsValue), bitcastTo<int64_t>(rhsValue), result)) {
                return {false, value::TypeTags::Date, value::bitcastFrom<int64_t>(result)};
            }
        } else if (isNumber(rhsTag)) {
            int64_t result;
            if (!Op::doOperation(
                    bitcastTo<int64_t>(lhsValue), numericCast<int64_t>(rhsTag, rhsValue), result)) {
                return {false, value::TypeTags::Date, value::bitcastFrom<int64_t>(result)};
            }
        } else {
            int64_t result;
            if (!Op::doOperation(
                    bitcastTo<int64_t>(lhsValue), bitcastTo<int64_t>(lhsValue), result)) {
                return {false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(result)};
            }
        }
        // We got here if the Date operation overflowed.
        uasserted(ErrorCodes::Overflow, "date overflow");
    }

    return {false, value::TypeTags::Nothing, 0};
}

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
std::tuple<bool, value::TypeTags, value::Value> genericTrigonometricFun(value::TypeTags argTag,
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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericAdd(value::TypeTags lhsTag,
                                                                     value::Value lhsValue,
                                                                     value::TypeTags rhsTag,
                                                                     value::Value rhsValue) {
    return genericArithmeticOp<Addition>(lhsTag, lhsValue, rhsTag, rhsValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericSub(value::TypeTags lhsTag,
                                                                     value::Value lhsValue,
                                                                     value::TypeTags rhsTag,
                                                                     value::Value rhsValue) {
    return genericArithmeticOp<Subtraction>(lhsTag, lhsValue, rhsTag, rhsValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericMul(value::TypeTags lhsTag,
                                                                     value::Value lhsValue,
                                                                     value::TypeTags rhsTag,
                                                                     value::Value rhsValue) {
    return genericArithmeticOp<Multiplication>(lhsTag, lhsValue, rhsTag, rhsValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericDiv(value::TypeTags lhsTag,
                                                                     value::Value lhsValue,
                                                                     value::TypeTags rhsTag,
                                                                     value::Value rhsValue) {
    auto assertNonZero = [](bool nonZero) { uassert(4848401, "can't $divide by zero", nonZero); };

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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericIDiv(value::TypeTags lhsTag,
                                                                      value::Value lhsValue,
                                                                      value::TypeTags rhsTag,
                                                                      value::Value rhsValue) {
    auto assertNonZero = [](bool nonZero) { uassert(4848402, "can't $divide by zero", nonZero); };

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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericMod(value::TypeTags lhsTag,
                                                                     value::Value lhsValue,
                                                                     value::TypeTags rhsTag,
                                                                     value::Value rhsValue) {
    auto assertNonZero = [](bool nonZero) { uassert(4848403, "can't $mod by zero", nonZero); };

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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericNumConvert(
    value::TypeTags lhsTag, value::Value lhsValue, value::TypeTags targetTag) {
    if (value::isNumber(lhsTag)) {
        switch (lhsTag) {
            case value::TypeTags::NumberInt32:
                return numericConvLossless<int32_t>(value::bitcastTo<int32_t>(lhsValue), targetTag);
            case value::TypeTags::NumberInt64:
                return numericConvLossless<int64_t>(value::bitcastTo<int64_t>(lhsValue), targetTag);
            case value::TypeTags::NumberDouble:
                return numericConvLossless<double>(value::bitcastTo<double>(lhsValue), targetTag);
            case value::TypeTags::NumberDecimal:
                return numericConvLossless<Decimal128>(value::bitcastTo<Decimal128>(lhsValue),
                                                       targetTag);
            default:
                MONGO_UNREACHABLE
        }
    }
    return {false, value::TypeTags::Nothing, 0};
}

static const double kDoubleLargestConsecutiveInteger =
    pow(std::numeric_limits<double>::radix, std::numeric_limits<double>::digits);

std::pair<value::TypeTags, value::Value> ByteCode::genericNumConvertToPreciseInt64(
    value::TypeTags lhsTag, value::Value lhsValue) {
    // If lhs is a double, we need to perform an extra check to ensure that lhs is within the range
    // where double can represent consecutive integers precisely. This check isn't necessary for
    // Decimal128, because Decimal128 can precisely represent every possible numeric value that can
    // fit in an int64_t.
    if (lhsTag == value::TypeTags::NumberDouble) {
        auto d = value::bitcastTo<double>(lhsValue);
        if (d >= kDoubleLargestConsecutiveInteger || d <= -kDoubleLargestConsecutiveInteger) {
            return {value::TypeTags::Nothing, 0};
        }
    }

    auto [owned, tag, val] = genericNumConvert(lhsTag, lhsValue, value::TypeTags::NumberInt64);
    invariant(!owned);
    return {tag, val};
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericAbs(value::TypeTags operandTag,
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
                    value::bitcastFrom<int32_t>(operand >= 0 ? operand : -operand)};
        }
        case value::TypeTags::NumberInt64: {
            auto operand = value::bitcastTo<int64_t>(operandValue);
            if (operand == std::numeric_limits<int64_t>::min()) {
                // Absolute value of the minimum int64_t value does not fit in any integer type.
                return {false, value::TypeTags::Nothing, 0};
            }
            return {false,
                    value::TypeTags::NumberInt64,
                    value::bitcastFrom<int64_t>(operand >= 0 ? operand : -operand)};
        }
        case value::TypeTags::NumberDouble: {
            auto operand = value::bitcastTo<double>(operandValue);
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(operand >= 0 ? operand : -operand)};
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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericCeil(value::TypeTags operandTag,
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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericFloor(value::TypeTags operandTag,
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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericTrunc(value::TypeTags operandTag,
                                                                       value::Value operandValue) {
    if (!isNumber(operandTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    switch (operandTag) {
        case value::TypeTags::NumberDouble: {
            auto truncatedValue = std::trunc(value::bitcastTo<double>(operandValue));
            return {
                false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(truncatedValue)};
        }
        case value::TypeTags::NumberDecimal: {
            auto value = value::bitcastTo<Decimal128>(operandValue);
            if (!value.isNaN() && value.isFinite()) {
                value = value.quantize(Decimal128::kNormalizedZero, Decimal128::kRoundTowardZero);
            }
            auto [resultTag, resultValue] = value::makeCopyDecimal(value);
            return {true, resultTag, resultValue};
        }
        case value::TypeTags::NumberInt32:
        case value::TypeTags::NumberInt64:
            // Trunc on integer values is the identity function.
            return {false, operandTag, operandValue};
        default:
            MONGO_UNREACHABLE;
    }
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericExp(value::TypeTags operandTag,
                                                                     value::Value operandValue) {
    switch (operandTag) {
        case value::TypeTags::NumberDouble: {
            auto result = exp(value::bitcastTo<double>(operandValue));
            return {false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(result)};
        }
        case value::TypeTags::NumberDecimal: {
            auto result = value::bitcastTo<Decimal128>(operandValue).exponential();
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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericLn(value::TypeTags operandTag,
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
            auto operandLn = operand.logarithm();

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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericLog10(value::TypeTags operandTag,
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
            auto operandLog10 = operand.logarithm(Decimal128(10));

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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericSqrt(value::TypeTags operandTag,
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
            auto [tag, value] = value::makeCopyDecimal(operand.squareRoot());
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

std::pair<value::TypeTags, value::Value> ByteCode::genericNot(value::TypeTags tag,
                                                              value::Value value) {
    if (tag == value::TypeTags::Boolean) {
        return {tag, value::bitcastFrom<bool>(!value::bitcastTo<bool>(value))};
    } else {
        return {value::TypeTags::Nothing, 0};
    }
}

std::pair<value::TypeTags, value::Value> ByteCode::compare3way(
    value::TypeTags lhsTag,
    value::Value lhsValue,
    value::TypeTags rhsTag,
    value::Value rhsValue,
    const StringData::ComparatorInterface* comparator) {
    if (lhsTag == value::TypeTags::Nothing || rhsTag == value::TypeTags::Nothing) {
        return {value::TypeTags::Nothing, 0};
    }

    return value::compareValue(lhsTag, lhsValue, rhsTag, rhsValue, comparator);
}

std::pair<value::TypeTags, value::Value> ByteCode::compare3way(value::TypeTags lhsTag,
                                                               value::Value lhsValue,
                                                               value::TypeTags rhsTag,
                                                               value::Value rhsValue,
                                                               value::TypeTags collTag,
                                                               value::Value collValue) {
    if (collTag != value::TypeTags::collator) {
        return {value::TypeTags::Nothing, 0};
    }

    auto comparator = static_cast<StringData::ComparatorInterface*>(getCollatorView(collValue));

    return value::compareValue(lhsTag, lhsValue, rhsTag, rhsValue, comparator);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericAcos(value::TypeTags argTag,
                                                                      value::Value argValue) {
    return genericTrigonometricFun<Acos>(argTag, argValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericAcosh(value::TypeTags argTag,
                                                                       value::Value argValue) {
    return genericTrigonometricFun<Acosh>(argTag, argValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericAsin(value::TypeTags argTag,
                                                                      value::Value argValue) {
    return genericTrigonometricFun<Asin>(argTag, argValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericAsinh(value::TypeTags argTag,
                                                                       value::Value argValue) {
    return genericTrigonometricFun<Asinh>(argTag, argValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericAtan(value::TypeTags argTag,
                                                                      value::Value argValue) {
    return genericTrigonometricFun<Atan>(argTag, argValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericAtanh(value::TypeTags argTag,
                                                                       value::Value argValue) {
    return genericTrigonometricFun<Atanh>(argTag, argValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericAtan2(value::TypeTags argTag1,
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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericCos(value::TypeTags argTag,
                                                                     value::Value argValue) {
    return genericTrigonometricFun<Cos>(argTag, argValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericCosh(value::TypeTags argTag,
                                                                      value::Value argValue) {
    return genericTrigonometricFun<Cosh>(argTag, argValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericDegreesToRadians(
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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericRadiansToDegrees(
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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericSin(value::TypeTags argTag,
                                                                     value::Value argValue) {
    return genericTrigonometricFun<Sin>(argTag, argValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericSinh(value::TypeTags argTag,
                                                                      value::Value argValue) {
    return genericTrigonometricFun<Sinh>(argTag, argValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericTan(value::TypeTags argTag,
                                                                     value::Value argValue) {
    return genericTrigonometricFun<Tan>(argTag, argValue);
}

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericTanh(value::TypeTags argTag,
                                                                      value::Value argValue) {
    return genericTrigonometricFun<Tanh>(argTag, argValue);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
