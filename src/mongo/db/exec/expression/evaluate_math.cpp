/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/convert_utils.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/query/random_utils.h"
#include "mongo/util/str_escape.h"
#include "mongo/util/text.h"

#include <fmt/compile.h>
#include <fmt/format.h>

namespace mongo {

namespace exec::expression {

namespace {

/**
 * We'll try to return the narrowest possible result value while avoiding overflow or implicit use
 * of decimal types. To do that, compute separate sums for long, double and decimal values, and
 * track the current widest type. The long sum will be converted to double when the first double
 * value is seen or when long arithmetic would overflow.
 */
class AddState {
public:
    /**
     * Update the internal state with another operand. It is up to the caller to validate that the
     * operand is of a proper type.
     */
    void operator+=(const Value& operand) {
        auto oldWidestType = widestType;
        // Dates are represented by the long number of milliseconds since the unix epoch, so we can
        // treat them as regular numeric values for the purposes of addition after making sure that
        // only one date is present in the operand list.
        Value valToAdd;
        if (operand.getType() == BSONType::date) {
            uassert(16612, "only one date allowed in an $add expression", !isDate);
            Value oldValue = getValue();
            longTotal = 0;
            addToDateValue(oldValue);
            isDate = true;
            valToAdd = Value(operand.getDate().toMillisSinceEpoch());
        } else {
            widestType = Value::getWidestNumeric(widestType, operand.getType());
            valToAdd = operand;
        }

        if (isDate) {
            addToDateValue(valToAdd);
            return;
        }

        // If this operation widens the return type, perform any necessary type conversions.
        if (oldWidestType != widestType) {
            switch (widestType) {
                case BSONType::numberLong:
                    // Int -> Long is handled by the same sum.
                    break;
                case BSONType::numberDouble:
                    // Int/Long -> Double converts the existing longTotal to a doubleTotal.
                    doubleTotal = longTotal;
                    break;
                case BSONType::numberDecimal:
                    // Convert the right total to NumberDecimal by looking at the old widest type.
                    switch (oldWidestType) {
                        case BSONType::numberInt:
                        case BSONType::numberLong:
                            decimalTotal = Decimal128(longTotal);
                            break;
                        case BSONType::numberDouble:
                            decimalTotal = Decimal128(doubleTotal);
                            break;
                        default:
                            MONGO_UNREACHABLE;
                    }
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
        }

        // Perform the add operation.
        switch (widestType) {
            case BSONType::numberInt:
            case BSONType::numberLong:
                // If the long long arithmetic overflows, promote the result to a NumberDouble and
                // start incrementing the doubleTotal.
                long long newLongTotal;
                if (overflow::add(longTotal, valToAdd.coerceToLong(), &newLongTotal)) {
                    widestType = BSONType::numberDouble;
                    doubleTotal = longTotal + valToAdd.coerceToDouble();
                } else {
                    longTotal = newLongTotal;
                }
                break;
            case BSONType::numberDouble:
                doubleTotal += valToAdd.coerceToDouble();
                break;
            case BSONType::numberDecimal:
                decimalTotal = decimalTotal.add(valToAdd.coerceToDecimal());
                break;
            default:
                uasserted(ErrorCodes::TypeMismatch,
                          str::stream() << "$add only supports numeric or date types, not "
                                        << typeName(valToAdd.getType()));
        }
    }

    Value getValue() const {
        // If one of the operands was a date, then return long value as Date.
        if (isDate) {
            return Value(Date_t::fromMillisSinceEpoch(longTotal));
        } else {
            switch (widestType) {
                case BSONType::numberInt:
                    return Value::createIntOrLong(longTotal);
                case BSONType::numberLong:
                    return Value(longTotal);
                case BSONType::numberDouble:
                    return Value(doubleTotal);
                case BSONType::numberDecimal:
                    return Value(decimalTotal);
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }

private:
    // Convert 'valToAdd' into the data type used for dates (long long) and add it to 'longTotal'.
    void addToDateValue(const Value& valToAdd) {
        switch (valToAdd.getType()) {
            case BSONType::numberInt:
            case BSONType::numberLong:
                if (overflow::add(longTotal, valToAdd.coerceToLong(), &longTotal)) {
                    uasserted(ErrorCodes::Overflow, "date overflow");
                }
                break;
            case BSONType::numberDouble: {
                using limits = std::numeric_limits<long long>;
                double doubleToAdd = valToAdd.coerceToDouble();
                uassert(ErrorCodes::Overflow,
                        "date overflow",
                        // The upper bound is exclusive because it rounds up when it is cast to
                        // a double.
                        doubleToAdd >= static_cast<double>(limits::min()) &&
                            doubleToAdd < static_cast<double>(limits::max()));

                if (overflow::add(longTotal, llround(doubleToAdd), &longTotal)) {
                    uasserted(ErrorCodes::Overflow, "date overflow");
                }
                break;
            }
            case BSONType::numberDecimal: {
                Decimal128 decimalToAdd = valToAdd.coerceToDecimal();

                std::uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;
                std::int64_t longToAdd = decimalToAdd.toLong(&signalingFlags);
                if (signalingFlags != Decimal128::SignalingFlag::kNoFlag ||
                    overflow::add(longTotal, longToAdd, &longTotal)) {
                    uasserted(ErrorCodes::Overflow, "date overflow");
                }
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }

    long long longTotal = 0;
    double doubleTotal = 0;
    Decimal128 decimalTotal;
    BSONType widestType = BSONType::numberInt;
    bool isDate = false;
};

Status checkAddOperandType(const Value& val) {
    if (!val.numeric() && val.getType() != BSONType::date) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "$add only supports numeric or date types, not "
                                    << typeName(val.getType()));
    }

    return Status::OK();
}

}  // namespace

StatusWith<Value> evaluateAdd(Value lhs, Value rhs) {
    if (lhs.nullish()) {
        return Value(BSONNULL);
    }
    if (Status s = checkAddOperandType(lhs); !s.isOK()) {
        return s;
    }
    if (rhs.nullish()) {
        return Value(BSONNULL);
    }
    if (Status s = checkAddOperandType(rhs); !s.isOK()) {
        return s;
    }

    AddState state;
    state += lhs;
    state += rhs;
    return state.getValue();
}

Value evaluate(const ExpressionAdd& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    AddState state;
    for (auto&& child : children) {
        Value val = child->evaluate(root, variables);
        if (val.nullish()) {
            return Value(BSONNULL);
        }
        uassertStatusOK(checkAddOperandType(val));
        state += val;
    }
    return state.getValue();
}

StatusWith<Value> evaluateDivide(Value lhs, Value rhs) {
    if (lhs.numeric() && rhs.numeric()) {
        // If, and only if, either side is decimal, return decimal.
        if (lhs.getType() == BSONType::numberDecimal || rhs.getType() == BSONType::numberDecimal) {
            Decimal128 numer = lhs.coerceToDecimal();
            Decimal128 denom = rhs.coerceToDecimal();
            if (denom.isZero()) {
                return Status(ErrorCodes::BadValue, "can't $divide by zero");
            }
            return Value(numer.divide(denom));
        }

        double numer = lhs.coerceToDouble();
        double denom = rhs.coerceToDouble();
        if (denom == 0.0) {
            return Status(ErrorCodes::BadValue, "can't $divide by zero");
        }

        return Value(numer / denom);
    } else if (lhs.nullish() || rhs.nullish()) {
        return Value(BSONNULL);
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream()
                          << "$divide only supports numeric types, not " << typeName(lhs.getType())
                          << " and " << typeName(rhs.getType()));
    }
}

StatusWith<Value> evaluateMod(Value lhs, Value rhs) {
    BSONType leftType = lhs.getType();
    BSONType rightType = rhs.getType();

    if (lhs.numeric() && rhs.numeric()) {

        // If either side is decimal, perform the operation in decimal.
        if (leftType == BSONType::numberDecimal || rightType == BSONType::numberDecimal) {
            Decimal128 left = lhs.coerceToDecimal();
            Decimal128 right = rhs.coerceToDecimal();
            if (right.isZero()) {
                return Status(ErrorCodes::Error(5733415), str::stream() << "can't $mod by zero");
            }

            return Value(left.modulo(right));
        }

        // ensure we aren't modding by 0
        double right = rhs.coerceToDouble();
        if (right == 0) {
            return Status(ErrorCodes::Error(16610), str::stream() << "can't $mod by zero");
        }

        if (leftType == BSONType::numberDouble || rightType == BSONType::numberDouble) {
            double left = lhs.coerceToDouble();
            return Value(fmod(left, right));
        }

        if (leftType == BSONType::numberLong || rightType == BSONType::numberLong) {
            // if either is long, return long
            long long left = lhs.coerceToLong();
            long long rightLong = rhs.coerceToLong();
            return Value(overflow::safeMod(left, rightLong));
        }

        // lastly they must both be ints, return int
        int left = lhs.coerceToInt();
        int rightInt = rhs.coerceToInt();
        return Value(overflow::safeMod(left, rightInt));
    } else if (lhs.nullish() || rhs.nullish()) {
        return Value(BSONNULL);
    } else {
        return Status(ErrorCodes::Error(16611),
                      str::stream()
                          << "$mod only supports numeric types, not " << typeName(lhs.getType())
                          << " and " << typeName(rhs.getType()));
    }
}

namespace {
class MultiplyState {
    /**
     * We'll try to return the narrowest possible result value.  To do that without creating
     * intermediate Values, do the arithmetic for double and integral types in parallel, tracking
     * the current narrowest type.
     */
    double doubleProduct = 1;
    long long longProduct = 1;
    Decimal128 decimalProduct;  // This will be initialized on encountering the first decimal.
    BSONType productType = BSONType::numberInt;

public:
    void operator*=(const Value& val) {
        tassert(5423304, "MultiplyState::operator*= only supports numbers", val.numeric());

        BSONType oldProductType = productType;
        productType = Value::getWidestNumeric(productType, val.getType());
        if (productType == BSONType::numberDecimal) {
            // On finding the first decimal, convert the partial product to decimal.
            if (oldProductType != BSONType::numberDecimal) {
                decimalProduct = oldProductType == BSONType::numberDouble
                    ? Decimal128(doubleProduct, Decimal128::kRoundTo15Digits)
                    : Decimal128(static_cast<int64_t>(longProduct));
            }
            decimalProduct = decimalProduct.multiply(val.coerceToDecimal());
        } else {
            doubleProduct *= val.coerceToDouble();

            if (productType != BSONType::numberDouble) {
                // If `productType` is not a double, it must be one of the integer types, so we
                // attempt to update `longProduct`.
                if (!std::isfinite(val.coerceToDouble()) ||
                    overflow::mul(longProduct, val.coerceToLong(), &longProduct)) {
                    // The multiplier is either Infinity or NaN, or the `longProduct` would
                    // have overflowed, so we're abandoning it.
                    productType = BSONType::numberDouble;
                }
            }
        }
    }

    Value getValue() const {
        if (productType == BSONType::numberDouble) {
            return Value(doubleProduct);
        } else if (productType == BSONType::numberLong) {
            return Value(longProduct);
        } else if (productType == BSONType::numberInt) {
            return Value::createIntOrLong(longProduct);
        } else if (productType == BSONType::numberDecimal) {
            return Value(decimalProduct);
        } else {
            massert(16418, "$multiply resulted in a non-numeric type", false);
        }
    }
};

Status checkMultiplyNumeric(const Value& val) {
    if (!val.numeric()) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "$multiply only supports numeric types, not "
                                    << typeName(val.getType()));
    }
    return Status::OK();
}
}  // namespace

StatusWith<Value> evaluateMultiply(Value lhs, Value rhs) {
    // evaluate() checks arguments left-to-right, short circuiting on the first null or non-number.
    // Imitate that behavior here.
    if (lhs.nullish()) {
        return Value(BSONNULL);
    }
    if (Status s = checkMultiplyNumeric(lhs); !s.isOK()) {
        return s;
    }
    if (rhs.nullish()) {
        return Value(BSONNULL);
    }
    if (Status s = checkMultiplyNumeric(rhs); !s.isOK()) {
        return s;
    }

    MultiplyState state;
    state *= lhs;
    state *= rhs;
    return state.getValue();
}

Value evaluate(const ExpressionMultiply& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    MultiplyState state;
    for (auto&& child : children) {
        Value val = child->evaluate(root, variables);
        if (val.nullish()) {
            return Value(BSONNULL);
        }
        uassertStatusOK(checkMultiplyNumeric(val));
        state *= val;
    }
    return state.getValue();
}

Value evaluate(const ExpressionLog& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value argVal = children[0]->evaluate(root, variables);
    Value baseVal = children[1]->evaluate(root, variables);
    if (argVal.nullish() || baseVal.nullish()) {
        return Value(BSONNULL);
    }

    uassert(28756,
            str::stream() << "$log's argument must be numeric, not " << typeName(argVal.getType()),
            argVal.numeric());
    uassert(28757,
            str::stream() << "$log's base must be numeric, not " << typeName(baseVal.getType()),
            baseVal.numeric());

    if (argVal.getType() == BSONType::numberDecimal ||
        baseVal.getType() == BSONType::numberDecimal) {
        Decimal128 argDecimal = argVal.coerceToDecimal();
        Decimal128 baseDecimal = baseVal.coerceToDecimal();

        if (argDecimal.isGreater(Decimal128::kNormalizedZero) &&
            baseDecimal.isNotEqual(Decimal128(1)) &&
            baseDecimal.isGreater(Decimal128::kNormalizedZero)) {
            // Change of logarithm base: log_B(A) == log(A) / log(B)
            return Value(argDecimal.log().divide(baseDecimal.log()));
        }
        // Fall through for error cases.
    }

    double argDouble = argVal.coerceToDouble();
    double baseDouble = baseVal.coerceToDouble();
    uassert(28758,
            str::stream() << "$log's argument must be a positive number, but is " << argDouble,
            argDouble > 0 || std::isnan(argDouble));
    uassert(28759,
            str::stream() << "$log's base must be a positive number not equal to 1, but is "
                          << baseDouble,
            (baseDouble > 0 && baseDouble != 1) || std::isnan(baseDouble));
    return Value(std::log(argDouble) / std::log(baseDouble));
}

Value evaluate(const ExpressionRandom& expr, const Document& root, Variables* variables) {
    static constexpr double kMinValue = 0.0;
    static constexpr double kMaxValue = 1.0;

    return Value(kMinValue +
                 (kMaxValue - kMinValue) * random_utils::getRNG().nextCanonicalDouble());
}

Value evaluate(const ExpressionRange& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value startVal(children[0]->evaluate(root, variables));
    Value endVal(children[1]->evaluate(root, variables));

    uassert(34443,
            str::stream() << "$range requires a numeric starting value, found value of type: "
                          << typeName(startVal.getType()),
            startVal.numeric());
    uassert(34444,
            str::stream() << "$range requires a starting value that can be represented as a 32-bit "
                             "integer, found value: "
                          << startVal.toString(),
            startVal.integral());
    uassert(34445,
            str::stream() << "$range requires a numeric ending value, found value of type: "
                          << typeName(endVal.getType()),
            endVal.numeric());
    uassert(34446,
            str::stream() << "$range requires an ending value that can be represented as a 32-bit "
                             "integer, found value: "
                          << endVal.toString(),
            endVal.integral());

    // Cast to broader type 'int64_t' to prevent overflow during loop.
    int64_t current = startVal.coerceToInt();
    int64_t end = endVal.coerceToInt();

    int64_t step = 1;
    if (children.size() == 3) {
        // A step was specified by the user.
        Value stepVal(children[2]->evaluate(root, variables));

        uassert(34447,
                str::stream() << "$range requires a numeric step value, found value of type:"
                              << typeName(stepVal.getType()),
                stepVal.numeric());
        uassert(34448,
                str::stream() << "$range requires a step value that can be represented as a 32-bit "
                                 "integer, found value: "
                              << stepVal.toString(),
                stepVal.integral());
        step = stepVal.coerceToInt();

        uassert(34449, "$range requires a non-zero step value", step != 0);
    }

    // Calculate how much memory is needed to generate the array and avoid going over the memLimit.
    auto steps = (end - current) / step;
    // If steps not positive then no amount of steps can get you from start to end. For example
    // with start=5, end=7, step=-1 steps would be negative and in this case we would return an
    // empty array.
    auto length = steps >= 0 ? 1 + steps : 0;
    int64_t memNeeded = sizeof(std::vector<Value>) + length * startVal.getApproximateSize();
    auto memLimit = internalQueryMaxRangeBytes.load();
    uassert(ErrorCodes::ExceededMemoryLimit,
            str::stream() << "$range would use too much memory (" << memNeeded << " bytes) "
                          << "and cannot spill to disk. Memory limit: " << memLimit << " bytes",
            memNeeded < memLimit);

    std::vector<Value> output;

    while ((step > 0 ? current < end : current > end)) {
        output.emplace_back(static_cast<int>(current));
        current += step;
    }

    return Value(std::move(output));
}

StatusWith<Value> evaluateSubtract(Value lhs, Value rhs) {
    BSONType diffType = Value::getWidestNumeric(rhs.getType(), lhs.getType());

    if (diffType == BSONType::numberDecimal) {
        Decimal128 right = rhs.coerceToDecimal();
        Decimal128 left = lhs.coerceToDecimal();
        return Value(left.subtract(right));
    } else if (diffType == BSONType::numberDouble) {
        double right = rhs.coerceToDouble();
        double left = lhs.coerceToDouble();
        return Value(left - right);
    } else if (diffType == BSONType::numberLong) {
        long long result;

        // If there is an overflow, convert the values to doubles.
        if (overflow::sub(lhs.coerceToLong(), rhs.coerceToLong(), &result)) {
            return Value(lhs.coerceToDouble() - rhs.coerceToDouble());
        }
        return Value(result);
    } else if (diffType == BSONType::numberInt) {
        long long right = rhs.coerceToLong();
        long long left = lhs.coerceToLong();
        return Value::createIntOrLong(left - right);
    } else if (lhs.nullish() || rhs.nullish()) {
        return Value(BSONNULL);
    } else if (lhs.getType() == BSONType::date) {
        BSONType rhsType = rhs.getType();
        switch (rhsType) {
            case BSONType::date:
                return Value(durationCount<Milliseconds>(lhs.getDate() - rhs.getDate()));
            case BSONType::numberInt:
            case BSONType::numberLong: {
                long long longDiff = lhs.getDate().toMillisSinceEpoch();
                if (overflow::sub(longDiff, rhs.coerceToLong(), &longDiff)) {
                    return Status(ErrorCodes::Overflow, str::stream() << "date overflow");
                }
                return Value(Date_t::fromMillisSinceEpoch(longDiff));
            }
            case BSONType::numberDouble: {
                using limits = std::numeric_limits<long long>;
                long long longDiff = lhs.getDate().toMillisSinceEpoch();
                double doubleRhs = rhs.coerceToDouble();
                // check the doubleRhs should not exceed int64 limit and result will not overflow
                if (doubleRhs >= static_cast<double>(limits::min()) &&
                    doubleRhs < static_cast<double>(limits::max()) &&
                    !overflow::sub(longDiff, llround(doubleRhs), &longDiff)) {
                    return Value(Date_t::fromMillisSinceEpoch(longDiff));
                }
                return Status(ErrorCodes::Overflow, str::stream() << "date overflow");
            }
            case BSONType::numberDecimal: {
                long long longDiff = lhs.getDate().toMillisSinceEpoch();
                Decimal128 decimalRhs = rhs.coerceToDecimal();
                std::uint32_t signalingFlags = Decimal128::SignalingFlag::kNoFlag;
                std::int64_t longRhs = decimalRhs.toLong(&signalingFlags);
                if (signalingFlags != Decimal128::SignalingFlag::kNoFlag ||
                    overflow::sub(longDiff, longRhs, &longDiff)) {
                    return Status(ErrorCodes::Overflow, str::stream() << "date overflow");
                }
                return Value(Date_t::fromMillisSinceEpoch(longDiff));
            }
            default:
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "can't $subtract " << typeName(rhs.getType()) << " from Date");
        }
    } else {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream() << "can't $subtract " << typeName(rhs.getType()) << " from "
                                    << typeName(lhs.getType()));
    }
}

namespace {

void assertFlagsValid(uint32_t flags,
                      const std::string& opName,
                      long long numericValue,
                      long long precisionValue) {
    uassert(51080,
            str::stream() << "invalid conversion from Decimal128 result in " << opName
                          << " resulting from arguments: [" << numericValue << ", "
                          << precisionValue << "]",
            !Decimal128::hasFlag(flags, Decimal128::kInvalid));
}

Value evaluateRoundOrTrunc(const Document& root,
                           const std::vector<boost::intrusive_ptr<Expression>>& children,
                           const std::string& opName,
                           Decimal128::RoundingMode roundingMode,
                           double (*doubleOp)(double),
                           Variables* variables) {
    constexpr auto maxPrecision = 100LL;
    constexpr auto minPrecision = -20LL;
    auto numericArg = Value(children[0]->evaluate(root, variables));
    if (numericArg.nullish()) {
        return Value(BSONNULL);
    }
    uassert(51081,
            str::stream() << opName << " only supports numeric types, not "
                          << typeName(numericArg.getType()),
            numericArg.numeric());

    long long precisionValue = 0;
    if (children.size() > 1) {
        auto precisionArg = Value(children[1]->evaluate(root, variables));
        if (precisionArg.nullish()) {
            return Value(BSONNULL);
        }
        precisionValue = precisionArg.coerceToLong();
        uassert(51082,
                str::stream() << "precision argument to  " << opName << " must be a integral value",
                precisionArg.integral());
        uassert(51083,
                str::stream() << "cannot apply " << opName << " with precision value "
                              << precisionValue << " value must be in [-20, 100]",
                minPrecision <= precisionValue && precisionValue <= maxPrecision);
    }

    // Construct 10^-precisionValue, which will be used as the quantize reference.
    auto quantum = Decimal128(0LL, Decimal128::kExponentBias - precisionValue, 0LL, 1LL);
    switch (numericArg.getType()) {
        case BSONType::numberDecimal: {
            if (numericArg.getDecimal().isInfinite()) {
                return numericArg;
            }
            auto out = numericArg.getDecimal().quantize(quantum, roundingMode);
            return Value(out);
        }
        case BSONType::numberDouble: {
            auto dec = Decimal128(numericArg.getDouble(), Decimal128::kRoundTo34Digits);
            if (dec.isInfinite()) {
                return numericArg;
            }
            auto out = dec.quantize(quantum, roundingMode);
            return Value(out.toDouble());
        }
        case BSONType::numberInt:
        case BSONType::numberLong: {
            if (precisionValue >= 0) {
                return numericArg;
            }
            auto numericArgll = numericArg.getLong();
            auto out =
                Decimal128(static_cast<int64_t>(numericArgll)).quantize(quantum, roundingMode);
            uint32_t flags = 0;
            auto outll = out.toLong(&flags);
            assertFlagsValid(flags, opName, numericArgll, precisionValue);
            if (numericArg.getType() == BSONType::numberLong ||
                outll > std::numeric_limits<int>::max()) {
                // Even if the original was an int to begin with - it has to be a long now.
                return Value(static_cast<long long>(outll));
            }
            return Value(static_cast<int>(outll));
        }
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace

Value evaluate(const ExpressionRound& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    return evaluateRoundOrTrunc(
        root, children, expr.getOpName(), Decimal128::kRoundTiesToEven, &std::round, variables);
}

Value evaluate(const ExpressionTrunc& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    return evaluateRoundOrTrunc(
        root, children, expr.getOpName(), Decimal128::kRoundTowardZero, &std::trunc, variables);
}

namespace {

template <typename T, typename valueFuncFn>
Value evaluateSingleNumericArg(const T& expr,
                               const Document& root,
                               Variables* variables,
                               valueFuncFn valueFunc) {
    Value arg = expr.getChildren()[0]->evaluate(root, variables);
    if (arg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(28765,
            str::stream() << expr.getOpName() << " only supports numeric types, not "
                          << typeName(arg.getType()),
            arg.numeric());

    return valueFunc(arg);
}

}  // namespace

Value evaluate(const ExpressionAbs& expr, const Document& root, Variables* variables) {
    return evaluateSingleNumericArg(expr, root, variables, [](const Value& numericArg) {
        BSONType type = numericArg.getType();
        if (type == BSONType::numberDouble) {
            return Value(std::abs(numericArg.getDouble()));
        } else if (type == BSONType::numberDecimal) {
            return Value(numericArg.getDecimal().toAbs());
        } else {
            long long num = numericArg.getLong();
            uassert(28680,
                    "can't take $abs of long long min",
                    num != std::numeric_limits<long long>::min());
            long long absVal = std::abs(num);
            return type == BSONType::numberLong ? Value(absVal) : Value::createIntOrLong(absVal);
        }
    });
}

Value evaluate(const ExpressionCeil& expr, const Document& root, Variables* variables) {
    return evaluateSingleNumericArg(expr, root, variables, [](const Value& numericArg) {
        // There's no point in taking the ceiling of integers or longs, it will have no effect.
        switch (numericArg.getType()) {
            case BSONType::numberDouble:
                return Value(std::ceil(numericArg.getDouble()));
            case BSONType::numberDecimal:
                // Round toward the nearest decimal with a zero exponent in the positive direction.
                return Value(numericArg.getDecimal().quantize(Decimal128::kNormalizedZero,
                                                              Decimal128::kRoundTowardPositive));
            default:
                return numericArg;
        }
    });
}

Value evaluate(const ExpressionExp& expr, const Document& root, Variables* variables) {
    return evaluateSingleNumericArg(expr, root, variables, [](const Value& numericArg) {
        // $exp always returns either a double or a decimal number, as e is irrational.
        if (numericArg.getType() == BSONType::numberDecimal) {
            return Value(numericArg.coerceToDecimal().exp());
        }

        return Value(exp(numericArg.coerceToDouble()));
    });
}

namespace {

/**
 * Helper for ExpressionPow to determine whether base^exp can be represented in a 64 bit int.
 *
 * 'base' and 'exp' are both integers. Assumes 'exp' is in the range [0, 63].
 */
bool representableAsLong(long long base, long long exp) {
    tassert(11103505,
            fmt::format("Expected exp to be in the [0, 63] range, but found {}", exp),
            0 <= exp && exp <= 63);
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
}

}  // namespace

Value evaluate(const ExpressionPow& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value baseVal = children[0]->evaluate(root, variables);
    Value expVal = children[1]->evaluate(root, variables);
    if (baseVal.nullish() || expVal.nullish()) {
        return Value(BSONNULL);
    }

    BSONType baseType = baseVal.getType();
    BSONType expType = expVal.getType();

    uassert(28762,
            str::stream() << "$pow's base must be numeric, not " << typeName(baseType),
            baseVal.numeric());
    uassert(28763,
            str::stream() << "$pow's exponent must be numeric, not " << typeName(expType),
            expVal.numeric());

    auto checkNonZeroAndNeg = [](bool isZeroAndNeg) {
        uassert(28764, "$pow cannot take a base of 0 and a negative exponent", !isZeroAndNeg);
    };

    // If either argument is decimal, return a decimal.
    if (baseType == BSONType::numberDecimal || expType == BSONType::numberDecimal) {
        Decimal128 baseDecimal = baseVal.coerceToDecimal();
        Decimal128 expDecimal = expVal.coerceToDecimal();
        checkNonZeroAndNeg(baseDecimal.isZero() && expDecimal.isNegative());
        return Value(baseDecimal.power(expDecimal));
    }

    // pow() will cast args to doubles.
    double baseDouble = baseVal.coerceToDouble();
    double expDouble = expVal.coerceToDouble();
    checkNonZeroAndNeg(baseDouble == 0 && expDouble < 0);

    // If either argument is a double, return a double.
    if (baseType == BSONType::numberDouble || expType == BSONType::numberDouble) {
        return Value(std::pow(baseDouble, expDouble));
    }

    // If either number is a long, return a long. If both numbers are ints, then return an int if
    // the result fits or a long if it is too big.
    const auto formatResult = [baseType, expType](long long res) {
        if (baseType == BSONType::numberLong || expType == BSONType::numberLong) {
            return Value(res);
        }
        return Value::createIntOrLong(res);
    };

    const long long baseLong = baseVal.getLong();
    const long long expLong = expVal.getLong();

    // Use this when the result cannot be represented as a long.
    const auto computeDoubleResult = [baseLong, expLong]() {
        return Value(std::pow(baseLong, expLong));
    };

    // Avoid doing repeated multiplication or using std::pow if the base is -1, 0, or 1.
    if (baseLong == 0) {
        if (expLong == 0) {
            // 0^0 = 1.
            return formatResult(1);
        } else if (expLong > 0) {
            // 0^x where x > 0 is 0.
            return formatResult(0);
        }

        // We should have checked earlier that 0 to a negative power is banned.
        MONGO_UNREACHABLE;
    } else if (baseLong == 1) {
        return formatResult(1);
    } else if (baseLong == -1) {
        // -1^0 = -1^2 = -1^4 = -1^6 ... = 1
        // -1^1 = -1^3 = -1^5 = -1^7 ... = -1
        return formatResult((expLong % 2 == 0) ? 1 : -1);
    } else if (expLong > 63 || expLong < 0) {
        // If the base is not 0, 1, or -1 and the exponent is too large, or negative,
        // the result cannot be represented as a long.
        return computeDoubleResult();
    }

    // It's still possible that the result cannot be represented as a long. If that's the case,
    // return a double.
    if (!representableAsLong(baseLong, expLong)) {
        return computeDoubleResult();
    }

    // Use repeated multiplication, since pow() casts args to doubles which could result in
    // loss of precision if arguments are very large.
    const auto computeWithRepeatedMultiplication = [](long long base, long long exp) {
        long long result = 1;

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
            tassert(11103507,
                    fmt::format("Expected exp to be equal to 1, but found {}", exp),
                    exp == 1);
            result *= base;
        }

        return result;
    };

    return formatResult(computeWithRepeatedMultiplication(baseLong, expLong));
}

Value evaluate(const ExpressionFloor& expr, const Document& root, Variables* variables) {
    return evaluateSingleNumericArg(expr, root, variables, [](const Value& numericArg) {
        // There's no point in taking the floor of integers or longs, it will have no effect.
        switch (numericArg.getType()) {
            case BSONType::numberDouble:
                return Value(std::floor(numericArg.getDouble()));
            case BSONType::numberDecimal:
                // Round toward the nearest decimal with a zero exponent in the negative direction.
                return Value(numericArg.getDecimal().quantize(Decimal128::kNormalizedZero,
                                                              Decimal128::kRoundTowardNegative));
            default:
                return numericArg;
        }
    });
}

Value evaluate(const ExpressionLn& expr, const Document& root, Variables* variables) {
    return evaluateSingleNumericArg(expr, root, variables, [](const Value& numericArg) {
        if (numericArg.getType() == BSONType::numberDecimal) {
            Decimal128 argDecimal = numericArg.getDecimal();
            if (argDecimal.isGreater(Decimal128::kNormalizedZero))
                return Value(argDecimal.log());
            // Fall through for error case.
        }
        double argDouble = numericArg.coerceToDouble();
        uassert(28766,
                str::stream() << "$ln's argument must be a positive number, but is " << argDouble,
                argDouble > 0 || std::isnan(argDouble));
        return Value(std::log(argDouble));
    });
}

Value evaluate(const ExpressionLog10& expr, const Document& root, Variables* variables) {
    return evaluateSingleNumericArg(expr, root, variables, [](const Value& numericArg) {
        if (numericArg.getType() == BSONType::numberDecimal) {
            Decimal128 argDecimal = numericArg.getDecimal();
            if (argDecimal.isGreater(Decimal128::kNormalizedZero))
                return Value(argDecimal.log10());
            // Fall through for error case.
        }

        double argDouble = numericArg.coerceToDouble();
        uassert(28761,
                str::stream() << "$log10's argument must be a positive number, but is "
                              << argDouble,
                argDouble > 0 || std::isnan(argDouble));
        return Value(std::log10(argDouble));
    });
}

Value evaluate(const ExpressionSqrt& expr, const Document& root, Variables* variables) {
    return evaluateSingleNumericArg(expr, root, variables, [](const Value& numericArg) {
        auto checkArg = [](bool nonNegative) {
            uassert(28714, "$sqrt's argument must be greater than or equal to 0", nonNegative);
        };

        if (numericArg.getType() == BSONType::numberDecimal) {
            Decimal128 argDec = numericArg.getDecimal();
            checkArg(!argDec.isLess(Decimal128::kNormalizedZero));  // NaN returns Nan without error
            return Value(argDec.sqrt());
        }
        double argDouble = numericArg.coerceToDouble();
        checkArg(!(argDouble < 0));  // NaN returns Nan without error
        return Value(sqrt(argDouble));
    });
}

Value evaluate(const ExpressionBitNot& expr, const Document& root, Variables* variables) {
    return evaluateSingleNumericArg(expr, root, variables, [&expr](const Value& numericArg) {
        BSONType type = numericArg.getType();

        if (type == BSONType::numberInt) {
            return Value(~numericArg.getInt());
        } else if (type == BSONType::numberLong) {
            return Value(~numericArg.getLong());
        } else {
            uasserted(ErrorCodes::TypeMismatch,
                      str::stream() << expr.getOpName() << " only supports int and long, not: "
                                    << typeName(type) << ".");
        }
    });
}

namespace {

static constexpr double kDoublePi = 3.141592653589793;
static constexpr double kDoublePiOver180 = kDoublePi / 180.0;
static constexpr double kDouble180OverPi = 180.0 / kDoublePi;

Value doDegreeRadiansConversion(const Value& numericArg,
                                Decimal128 decimalFactor,
                                double doubleFactor) {
    switch (numericArg.getType()) {
        case BSONType::numberDecimal:
            return Value(numericArg.getDecimal().multiply(decimalFactor));
        default:
            return Value(numericArg.coerceToDouble() * doubleFactor);
    }
}

}  // namespace

Value evaluate(const ExpressionDegreesToRadians& expr, const Document& root, Variables* variables) {
    return evaluateSingleNumericArg(expr, root, variables, [](const Value& numericArg) {
        return doDegreeRadiansConversion(numericArg, Decimal128::kPiOver180, kDoublePiOver180);
    });
}

Value evaluate(const ExpressionRadiansToDegrees& expr, const Document& root, Variables* variables) {
    return evaluateSingleNumericArg(expr, root, variables, [](const Value& numericArg) {
        return doDegreeRadiansConversion(numericArg, Decimal128::k180OverPi, kDouble180OverPi);
    });
}

Value evaluate(const ExpressionArcTangent2& expr, const Document& root, Variables* variables) {
    Value arg1 = expr.getChildren()[0]->evaluate(root, variables);
    if (arg1.nullish()) {
        return Value(BSONNULL);
    }
    uassert(51044,
            str::stream() << expr.getOpName() << " only supports numeric types, not "
                          << typeName(arg1.getType()),
            arg1.numeric());

    Value arg2 = expr.getChildren()[1]->evaluate(root, variables);
    if (arg2.nullish()) {
        return Value(BSONNULL);
    }
    uassert(51045,
            str::stream() << expr.getOpName() << " only supports numeric types, not "
                          << typeName(arg2.getType()),
            arg2.numeric());

    auto totalType = BSONType::numberDouble;
    // If the type of either argument is NumberDecimal, we promote to Decimal128.
    if (arg1.getType() == BSONType::numberDecimal || arg2.getType() == BSONType::numberDecimal) {
        totalType = BSONType::numberDecimal;
    }
    switch (totalType) {
        case BSONType::numberDecimal: {
            auto dec = arg1.coerceToDecimal();
            return Value(dec.atan2(arg2.coerceToDecimal()));
        }
        case BSONType::numberDouble: {
            return Value(std::atan2(arg1.coerceToDouble(), arg2.coerceToDouble()));
        }
        default:
            MONGO_UNREACHABLE;
    }
}

namespace {

bool isnan(double d) {
    return std::isnan(d);
}

bool isnan(Decimal128 d) {
    return d.isNaN();
}

template <typename T, typename doubleFuncFn, typename decimalFuncFn>
Value evaluateBoundedTrigonometric(const T& expr,
                                   const Document& root,
                                   Variables* variables,
                                   doubleFuncFn doubleFunc,
                                   decimalFuncFn decimalFunc) {
    return evaluateSingleNumericArg(
        expr, root, variables, [&expr, doubleFunc, decimalFunc](const Value& numericArg) {
            switch (numericArg.getType()) {
                case BSONType::numberDouble: {
                    auto input = numericArg.getDouble();
                    if (isnan(input)) {
                        return numericArg;
                    }
                    expr.assertBounds(input);
                    return Value(doubleFunc(input));
                }
                case BSONType::numberDecimal: {
                    auto input = numericArg.getDecimal();
                    if (isnan(input)) {
                        return numericArg;
                    }
                    expr.assertBounds(input);
                    return Value(decimalFunc(input));
                }
                default: {
                    auto input = static_cast<double>(numericArg.getLong());
                    if (isnan(input)) {
                        return numericArg;
                    }
                    expr.assertBounds(input);
                    return Value(doubleFunc(input));
                }
            }
        });
}

template <typename T, typename doubleFuncFn, typename decimalFuncFn>
Value evaluateUnboundedTrigonometric(const T& expr,
                                     const Document& root,
                                     Variables* variables,
                                     doubleFuncFn doubleFunc,
                                     decimalFuncFn decimalFunc) {
    return evaluateSingleNumericArg(
        expr, root, variables, [doubleFunc, decimalFunc](const Value& numericArg) {
            switch (numericArg.getType()) {
                case BSONType::numberDouble:
                    return Value(doubleFunc(numericArg.getDouble()));
                case BSONType::numberDecimal:
                    return Value(decimalFunc(numericArg.getDecimal()));
                default: {
                    auto num = static_cast<double>(numericArg.getLong());
                    return Value(doubleFunc(num));
                }
            }
        });
}

}  // namespace

Value evaluate(const ExpressionCosine& expr, const Document& root, Variables* variables) {
    return evaluateBoundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::cos(arg); },
        [](const Decimal128& arg) { return arg.cos(); });
}

Value evaluate(const ExpressionSine& expr, const Document& root, Variables* variables) {
    return evaluateBoundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::sin(arg); },
        [](const Decimal128& arg) { return arg.sin(); });
}

Value evaluate(const ExpressionTangent& expr, const Document& root, Variables* variables) {
    return evaluateBoundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::tan(arg); },
        [](const Decimal128& arg) { return arg.tan(); });
}

Value evaluate(const ExpressionArcCosine& expr, const Document& root, Variables* variables) {
    return evaluateBoundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::acos(arg); },
        [](const Decimal128& arg) { return arg.acos(); });
}

Value evaluate(const ExpressionArcSine& expr, const Document& root, Variables* variables) {
    return evaluateBoundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::asin(arg); },
        [](const Decimal128& arg) { return arg.asin(); });
}

Value evaluate(const ExpressionHyperbolicArcTangent& expr,
               const Document& root,
               Variables* variables) {
    return evaluateBoundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::atanh(arg); },
        [](const Decimal128& arg) { return arg.atanh(); });
}

Value evaluate(const ExpressionHyperbolicArcCosine& expr,
               const Document& root,
               Variables* variables) {
    return evaluateBoundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::acosh(arg); },
        [](const Decimal128& arg) { return arg.acosh(); });
}

Value evaluate(const ExpressionArcTangent& expr, const Document& root, Variables* variables) {
    return evaluateUnboundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::atan(arg); },
        [](const Decimal128& arg) { return arg.atan(); });
}

Value evaluate(const ExpressionHyperbolicArcSine& expr,
               const Document& root,
               Variables* variables) {
    return evaluateUnboundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::asinh(arg); },
        [](const Decimal128& arg) { return arg.asinh(); });
}

Value evaluate(const ExpressionHyperbolicCosine& expr, const Document& root, Variables* variables) {
    return evaluateUnboundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::cosh(arg); },
        [](const Decimal128& arg) { return arg.cosh(); });
}

Value evaluate(const ExpressionHyperbolicSine& expr, const Document& root, Variables* variables) {
    return evaluateUnboundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::sinh(arg); },
        [](const Decimal128& arg) { return arg.sinh(); });
}

Value evaluate(const ExpressionHyperbolicTangent& expr,
               const Document& root,
               Variables* variables) {
    return evaluateUnboundedTrigonometric(
        expr,
        root,
        variables,
        [](double arg) { return std::tanh(arg); },
        [](const Decimal128& arg) { return arg.tanh(); });
}

}  // namespace exec::expression
}  // namespace mongo
