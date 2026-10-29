// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/arith_common.h"

#include "mongo/base/error_codes.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/util/assert_util.h"

#include <cmath>
#include <cstdint>
#include <limits>


/**
These common operations - Addition, Subtraction and Multiplication - are used in both the VM and
constant folding in the optimizer. These methods are extensible for any computation with SBE values.
*/
namespace mongo::sbe::value {

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
value::TagValueMaybeOwned genericArithmeticOp(value::TypeTags lhsTag,
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
                    return value::TagValueMaybeOwned::numberInt32(result);
                }
                // The result does not fit into int32_t so fallthru to the wider type.
                [[fallthrough]];
            }
            case value::TypeTags::NumberInt64: {
                int64_t result;
                if (!Op::doOperation(numericCast<int64_t>(lhsTag, lhsValue),
                                     numericCast<int64_t>(rhsTag, rhsValue),
                                     result)) {
                    return value::TagValueMaybeOwned::numberInt64(result);
                }
                // The result does not fit into int64_t so fallthru to the wider type.
                [[fallthrough]];
            }
            case value::TypeTags::NumberDouble: {
                double result;
                Op::doOperation(numericCast<double>(lhsTag, lhsValue),
                                numericCast<double>(rhsTag, rhsValue),
                                result);
                return value::TagValueMaybeOwned::numberDouble(result);
            }
            case value::TypeTags::NumberDecimal: {
                Decimal128 result;
                Op::doOperation(numericCast<Decimal128>(lhsTag, lhsValue),
                                numericCast<Decimal128>(rhsTag, rhsValue),
                                result);
                auto [tag, val] = value::makeCopyDecimal(result);
                return {true, tag, val};
            }
            default:
                MONGO_UNREACHABLE_TASSERT(11122909);
        }
    } else if (lhsTag == TypeTags::Date || rhsTag == TypeTags::Date) {
        if (isNumber(lhsTag)) {
            int64_t result;
            switch (lhsTag) {
                case TypeTags::NumberDouble: {
                    using limits = std::numeric_limits<int64_t>;
                    double doubleLhs = numericCast<double>(lhsTag, lhsValue);
                    // The upper bound is exclusive because it rounds up when it is cast to a
                    // double.
                    if (doubleLhs >= static_cast<double>(limits::min()) &&
                        doubleLhs < static_cast<double>(limits::max()) &&
                        !Op::template doOperation<int64_t>(
                            llround(doubleLhs), bitcastTo<int64_t>(rhsValue), result)) {
                        return value::TagValueMaybeOwned::date(result);
                    }
                    break;
                }
                case TypeTags::NumberDecimal: {
                    using limits = std::numeric_limits<int64_t>;
                    auto decimalLhs = numericCast<Decimal128>(lhsTag, lhsValue);
                    if (decimalLhs.isGreaterEqual(Decimal128{limits::min()}) &&
                        decimalLhs.isLess(Decimal128{limits::max()}) &&
                        !Op::doOperation(
                            decimalLhs.toLong(), bitcastTo<int64_t>(rhsValue), result)) {
                        return value::TagValueMaybeOwned::date(result);
                    }
                    break;
                }
                default: {
                    if (!Op::doOperation(numericCast<int64_t>(lhsTag, lhsValue),
                                         bitcastTo<int64_t>(rhsValue),
                                         result)) {
                        return value::TagValueMaybeOwned::date(result);
                    }
                }
            }
        } else if (isNumber(rhsTag)) {
            int64_t result;
            switch (rhsTag) {
                case TypeTags::NumberDouble: {
                    using limits = std::numeric_limits<int64_t>;
                    double doubleRhs = numericCast<double>(rhsTag, rhsValue);
                    // The upper bound is exclusive because it rounds up when it is cast to a
                    // double.
                    if (doubleRhs >= static_cast<double>(limits::min()) &&
                        doubleRhs < static_cast<double>(limits::max()) &&
                        !Op::template doOperation<int64_t>(
                            bitcastTo<int64_t>(lhsValue), llround(doubleRhs), result)) {
                        return value::TagValueMaybeOwned::date(result);
                    }
                    break;
                }
                case TypeTags::NumberDecimal: {
                    auto decimalRhs = numericCast<Decimal128>(rhsTag, rhsValue);

                    std::uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;
                    std::int64_t longRhs = decimalRhs.toLong(&signalingFlags);
                    if (signalingFlags == Decimal128::SignalingFlag::kNoFlag &&
                        !Op::doOperation(bitcastTo<int64_t>(lhsValue), longRhs, result)) {
                        return value::TagValueMaybeOwned::date(result);
                    }
                    break;
                }
                default: {
                    if (!Op::doOperation(bitcastTo<int64_t>(lhsValue),
                                         numericCast<int64_t>(rhsTag, rhsValue),
                                         result)) {
                        return value::TagValueMaybeOwned::date(result);
                    }
                }
            }
        } else {
            int64_t result;
            if (!Op::doOperation(
                    bitcastTo<int64_t>(lhsValue), bitcastTo<int64_t>(rhsValue), result)) {
                return value::TagValueMaybeOwned::numberInt64(result);
            }
        }
        // We got here if the Date operation overflowed.
        uasserted(ErrorCodes::Overflow, "date overflow");
    }

    return value::TagValueMaybeOwned::nothing();
}

value::TagValueMaybeOwned genericAdd(value::TypeTags lhsTag,
                                     value::Value lhsValue,
                                     value::TypeTags rhsTag,
                                     value::Value rhsValue) {
    return genericArithmeticOp<Addition>(lhsTag, lhsValue, rhsTag, rhsValue);
}

value::TagValueMaybeOwned genericSub(value::TypeTags lhsTag,
                                     value::Value lhsValue,
                                     value::TypeTags rhsTag,
                                     value::Value rhsValue) {
    return genericArithmeticOp<Subtraction>(lhsTag, lhsValue, rhsTag, rhsValue);
}

value::TagValueMaybeOwned genericMul(value::TypeTags lhsTag,
                                     value::Value lhsValue,
                                     value::TypeTags rhsTag,
                                     value::Value rhsValue) {
    return genericArithmeticOp<Multiplication>(lhsTag, lhsValue, rhsTag, rhsValue);
}

value::TagValueMaybeOwned genericNumConvert(value::TypeTags lhsTag,
                                            value::Value lhsValue,
                                            value::TypeTags targetTag) {
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
                MONGO_UNREACHABLE_TASSERT(11122910);
        }
    }
    return value::TagValueMaybeOwned::nothing();
}

}  // namespace mongo::sbe::value
