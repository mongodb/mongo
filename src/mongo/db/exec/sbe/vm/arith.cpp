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

#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace sbe {
namespace vm {

using namespace value;

namespace {
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
 * This is a simple arithemtic operation templated by the Op parameter. It support operations on
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
                    return {false, value::TypeTags::NumberInt32, value::bitcastFrom(result)};
                }
                // The result does not fit into int32_t so fallthru to the wider type.
            }
            case value::TypeTags::NumberInt64: {
                int64_t result;
                if (!Op::doOperation(numericCast<int64_t>(lhsTag, lhsValue),
                                     numericCast<int64_t>(rhsTag, rhsValue),
                                     result)) {
                    return {false, value::TypeTags::NumberInt64, value::bitcastFrom(result)};
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
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom(result)};
            }
            default:
                MONGO_UNREACHABLE;
        }
    } else if (lhsTag == TypeTags::Date || rhsTag == TypeTags::Date) {
        if (isNumber(lhsTag)) {
            int64_t result;
            if (!Op::doOperation(
                    numericCast<int64_t>(lhsTag, lhsValue), bitcastTo<int64_t>(rhsValue), result)) {
                return {false, value::TypeTags::Date, value::bitcastFrom(result)};
            }
        } else if (isNumber(rhsTag)) {
            int64_t result;
            if (!Op::doOperation(
                    bitcastTo<int64_t>(lhsValue), numericCast<int64_t>(rhsTag, rhsValue), result)) {
                return {false, value::TypeTags::Date, value::bitcastFrom(result)};
            }
        } else {
            int64_t result;
            if (!Op::doOperation(
                    bitcastTo<int64_t>(lhsValue), bitcastTo<int64_t>(lhsValue), result)) {
                return {false, value::TypeTags::NumberInt64, value::bitcastFrom(result)};
            }
        }
        // We got here if the Date operation overflowed.
        uasserted(ErrorCodes::Overflow, "date overflow");
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
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberInt64: {
                assertNonZero(numericCast<double>(rhsTag, rhsValue) != 0);
                auto result =
                    numericCast<double>(lhsTag, lhsValue) / numericCast<double>(rhsTag, rhsValue);
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberDouble: {
                assertNonZero(numericCast<double>(rhsTag, rhsValue) != 0);
                auto result =
                    numericCast<double>(lhsTag, lhsValue) / numericCast<double>(rhsTag, rhsValue);
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberDecimal: {
                assertNonZero(numericCast<Decimal128>(rhsTag, rhsValue).isZero());
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
                return {false, value::TypeTags::NumberInt32, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberInt64: {
                assertNonZero(numericCast<int64_t>(rhsTag, rhsValue) != 0);
                auto result =
                    numericCast<int64_t>(lhsTag, lhsValue) / numericCast<int64_t>(rhsTag, rhsValue);
                return {false, value::TypeTags::NumberInt64, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberDouble: {
                auto lhs = representAs<int64_t>(numericCast<double>(lhsTag, lhsValue));
                auto rhs = representAs<int64_t>(numericCast<double>(rhsTag, rhsValue));

                if (!lhs || !rhs) {
                    return {false, value::TypeTags::Nothing, 0};
                }
                assertNonZero(*rhs != 0);
                auto result = *lhs / *rhs;

                return {false, value::TypeTags::NumberInt64, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberDecimal: {
                auto lhs = representAs<int64_t>(numericCast<Decimal128>(lhsTag, lhsValue));
                auto rhs = representAs<int64_t>(numericCast<Decimal128>(rhsTag, rhsValue));

                if (!lhs || !rhs) {
                    return {false, value::TypeTags::Nothing, 0};
                }
                assertNonZero(*rhs != 0);
                auto result = *lhs / *rhs;

                return {false, value::TypeTags::NumberInt64, value::bitcastFrom(result)};
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
                return {false, value::TypeTags::NumberInt32, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberInt64: {
                assertNonZero(numericCast<int32_t>(rhsTag, rhsValue) != 0);
                auto result = overflow::safeMod(numericCast<int64_t>(lhsTag, lhsValue),
                                                numericCast<int64_t>(rhsTag, rhsValue));
                return {false, value::TypeTags::NumberInt64, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberDouble: {
                assertNonZero(numericCast<int32_t>(rhsTag, rhsValue) != 0);
                auto result = fmod(numericCast<double>(lhsTag, lhsValue),
                                   numericCast<double>(rhsTag, rhsValue));
                return {false, value::TypeTags::NumberDouble, value::bitcastFrom(result)};
            }
            case value::TypeTags::NumberDecimal: {
                assertNonZero(numericCast<Decimal128>(rhsTag, rhsValue).isZero());
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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericAbs(value::TypeTags operandTag,
                                                                     value::Value operandValue) {
    switch (operandTag) {
        case value::TypeTags::NumberInt32: {
            auto operand = value::bitcastTo<int32_t>(operandValue);
            if (operand == std::numeric_limits<int32_t>::min()) {
                return {false, value::TypeTags::NumberInt64, value::bitcastFrom(int64_t{operand})};
            }

            return {false,
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom(operand >= 0 ? operand : -operand)};
        }
        case value::TypeTags::NumberInt64: {
            auto operand = value::bitcastTo<int64_t>(operandValue);
            uassert(/* Intentionally duplicated */ 28680,
                    "can't take $abs of long long min",
                    operand != std::numeric_limits<int64_t>::min());
            return {false,
                    value::TypeTags::NumberInt64,
                    value::bitcastFrom(operand >= 0 ? operand : -operand)};
        }
        case value::TypeTags::NumberDouble: {
            auto operand = value::bitcastTo<double>(operandValue);
            return {false,
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom(operand >= 0 ? operand : -operand)};
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

std::tuple<bool, value::TypeTags, value::Value> ByteCode::genericNot(value::TypeTags tag,
                                                                     value::Value value) {
    if (tag == value::TypeTags::Boolean) {
        return {
            false, value::TypeTags::Boolean, value::bitcastFrom(!value::bitcastTo<bool>(value))};
    } else {
        return {false, value::TypeTags::Nothing, 0};
    }
}

std::pair<value::TypeTags, value::Value> ByteCode::genericCompareEq(value::TypeTags lhsTag,
                                                                    value::Value lhsValue,
                                                                    value::TypeTags rhsTag,
                                                                    value::Value rhsValue) {
    if ((value::isNumber(lhsTag) && value::isNumber(rhsTag)) ||
        (lhsTag == value::TypeTags::Date && rhsTag == value::TypeTags::Date) ||
        (lhsTag == value::TypeTags::Timestamp && rhsTag == value::TypeTags::Timestamp)) {
        return genericNumericCompare(lhsTag, lhsValue, rhsTag, rhsValue, std::equal_to<>{});
    } else if (value::isString(lhsTag) && value::isString(rhsTag)) {
        auto lhsStr = value::getStringView(lhsTag, lhsValue);
        auto rhsStr = value::getStringView(rhsTag, rhsValue);

        return {value::TypeTags::Boolean, lhsStr.compare(rhsStr) == 0};
    } else if (lhsTag == value::TypeTags::Boolean && rhsTag == value::TypeTags::Boolean) {
        return {value::TypeTags::Boolean, (lhsValue != 0) == (rhsValue != 0)};
    } else if (lhsTag == value::TypeTags::Null && rhsTag == value::TypeTags::Null) {
        // This is where Mongo differs from SQL.
        return {value::TypeTags::Boolean, true};
    } else if (lhsTag == value::TypeTags::ObjectId && rhsTag == value::TypeTags::ObjectId) {
        return {value::TypeTags::Boolean,
                (*value::getObjectIdView(lhsValue)) == (*value::getObjectIdView(rhsValue))};
    } else if ((value::isArray(lhsTag) && value::isArray(rhsTag)) ||
               (value::isObject(lhsTag) && value::isObject(rhsTag))) {
        auto [tag, val] = value::compareValue(lhsTag, lhsValue, rhsTag, rhsValue);
        if (tag == value::TypeTags::NumberInt32) {
            auto result = (value::bitcastTo<int32_t>(val) == 0);
            return {value::TypeTags::Boolean, value::bitcastFrom(result)};
        }
    }

    return {value::TypeTags::Nothing, 0};
}

std::pair<value::TypeTags, value::Value> ByteCode::genericCompareNeq(value::TypeTags lhsTag,
                                                                     value::Value lhsValue,
                                                                     value::TypeTags rhsTag,
                                                                     value::Value rhsValue) {
    auto [tag, val] = genericCompareEq(lhsTag, lhsValue, rhsTag, rhsValue);
    if (tag == value::TypeTags::Boolean) {
        return {tag, value::bitcastFrom(!value::bitcastTo<bool>(val))};
    } else {
        return {tag, val};
    }
}

std::pair<value::TypeTags, value::Value> ByteCode::compare3way(value::TypeTags lhsTag,
                                                               value::Value lhsValue,
                                                               value::TypeTags rhsTag,
                                                               value::Value rhsValue) {
    if (lhsTag == value::TypeTags::Nothing || rhsTag == value::TypeTags::Nothing) {
        return {value::TypeTags::Nothing, 0};
    }

    return value::compareValue(lhsTag, lhsValue, rhsTag, rhsValue);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
