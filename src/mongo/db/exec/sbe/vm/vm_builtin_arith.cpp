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
#include "mongo/db/query/random_utils.h"

namespace mongo {
namespace sbe {
namespace vm {
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAbs(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericAbs(tagOperand, valOperand);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCeil(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericCeil(tagOperand, valOperand);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinFloor(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericFloor(tagOperand, valOperand);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinExp(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericExp(tagOperand, valOperand);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinLn(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericLn(tagOperand, valOperand);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinLog10(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericLog10(tagOperand, valOperand);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSqrt(ArityType arity) {
    invariant(arity == 1);

    auto [_, tagOperand, valOperand] = getFromStack(0);

    return genericSqrt(tagOperand, valOperand);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinPow(ArityType arity) {
    invariant(arity == 2);
    auto [baseOwned, baseTag, baseValue] = getFromStack(0);
    auto [exponentOwned, exponentTag, exponentValue] = getFromStack(1);

    return genericPow(baseTag, baseValue, exponentTag, exponentValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAcos(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAcos(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAcosh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAcosh(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAsin(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAsin(operandTag, operandValue);
}
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAsinh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAsinh(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAtan(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAtan(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAtanh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericAtanh(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinAtan2(ArityType arity) {
    auto [owned1, operandTag1, operandValue1] = getFromStack(0);
    auto [owned2, operandTag2, operandValue2] = getFromStack(1);
    return genericAtan2(operandTag1, operandValue1, operandTag2, operandValue2);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCos(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericCos(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCosh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericCosh(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinDegreesToRadians(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericDegreesToRadians(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinRadiansToDegrees(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericRadiansToDegrees(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSin(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericSin(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSinh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericSinh(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinTan(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericTan(operandTag, operandValue);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinTanh(ArityType arity) {
    auto [_, operandTag, operandValue] = getFromStack(0);
    return genericTanh(operandTag, operandValue);
}

/**
 * Converts a number to int32 assuming the input fits the range. This is used for $round "place"
 * argument, which is checked to be a whole number between -20 and 100, but could still be a
 * non-int32 type.
 */
int32_t ByteCode::convertNumericToInt32(const value::TypeTags tag, const value::Value val) {
    switch (tag) {
        case value::TypeTags::NumberInt32: {
            return value::bitcastTo<int32_t>(val);
        }
        case value::TypeTags::NumberInt64: {
            return static_cast<int32_t>(value::bitcastTo<int64_t>(val));
        }
        case value::TypeTags::NumberDouble: {
            return static_cast<int32_t>(value::bitcastTo<double>(val));
        }
        case value::TypeTags::NumberDecimal: {
            Decimal128 dec = value::bitcastTo<Decimal128>(val);
            return dec.toInt(Decimal128::kRoundTiesToEven);
        }
        default:
            MONGO_UNREACHABLE;
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::genericRoundTrunc(
    std::string funcName,
    Decimal128::RoundingMode roundingMode,
    int32_t place,
    value::TypeTags numTag,
    value::Value numVal) {

    // Construct 10^-precisionValue, which will be used as the quantize reference. This is passed to
    // decimal.quantize() to indicate the precision of our rounding.
    const auto quantum = Decimal128(0LL, Decimal128::kExponentBias - place, 0LL, 1LL);

    switch (numTag) {
        case value::TypeTags::NumberDecimal: {
            auto dec = value::bitcastTo<Decimal128>(numVal);
            if (!dec.isInfinite()) {
                dec = dec.quantize(quantum, roundingMode);
            }
            auto [resultTag, resultValue] = value::makeCopyDecimal(dec);
            return {true, resultTag, resultValue};
        }
        case value::TypeTags::NumberDouble: {
            auto asDec = Decimal128(value::bitcastTo<double>(numVal), Decimal128::kRoundTo34Digits);
            if (!asDec.isInfinite()) {
                asDec = asDec.quantize(quantum, roundingMode);
            }
            return {
                false, value::TypeTags::NumberDouble, value::bitcastFrom<double>(asDec.toDouble())};
        }
        case value::TypeTags::NumberInt32:
        case value::TypeTags::NumberInt64: {
            if (place >= 0) {
                return {false, numTag, numVal};
            }
            auto numericArgll = numTag == value::TypeTags::NumberInt32
                ? static_cast<int64_t>(value::bitcastTo<int32_t>(numVal))
                : value::bitcastTo<int64_t>(numVal);
            auto out = Decimal128(numericArgll).quantize(quantum, roundingMode);
            uint32_t flags = 0;
            auto outll = out.toLong(&flags);
            uassert(5155302,
                    "Invalid conversion to long during " + funcName + ".",
                    !Decimal128::hasFlag(flags, Decimal128::kInvalid));
            if (numTag == value::TypeTags::NumberInt64 ||
                outll > std::numeric_limits<int32_t>::max()) {
                // Even if the original was an int to begin with - it has to be a long now.
                return {false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(outll)};
            }
            return {false,
                    value::TypeTags::NumberInt32,
                    value::bitcastFrom<int32_t>(static_cast<int32_t>(outll))};
        }
        default:
            return {false, value::TypeTags::Nothing, 0};
    }
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::scalarRoundTrunc(
    std::string funcName, Decimal128::RoundingMode roundingMode, ArityType arity) {
    invariant(arity == 1 || arity == 2);
    int32_t place = 0;
    const auto [_, numTag, numVal] = getFromStack(0);
    if (arity == 2) {
        const auto [placeOwn, placeTag, placeVal] = getFromStack(1);
        if (!value::isNumber(placeTag)) {
            return {false, value::TypeTags::Nothing, 0};
        }
        place = convertNumericToInt32(placeTag, placeVal);
    }

    return genericRoundTrunc(funcName, roundingMode, place, numTag, numVal);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinTrunc(ArityType arity) {
    return scalarRoundTrunc("$trunc", Decimal128::kRoundTowardZero, arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinRand(ArityType arity) {
    double num = random_utils::getRNG().nextCanonicalDouble();
    return {true, value::TypeTags::NumberDouble, value::bitcastFrom<double>(num)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinRound(ArityType arity) {
    return scalarRoundTrunc("$round", Decimal128::kRoundTiesToEven, arity);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinDoubleDoubleSum(ArityType arity) {
    invariant(arity >= 1);

    value::TypeTags resultTag = value::TypeTags::NumberInt32;
    bool haveDate = false;

    // Sweep across all tags and pick the result type.
    for (ArityType idx = 0; idx < arity; ++idx) {
        auto [own, tag, val] = getFromStack(idx);
        if (tag == value::TypeTags::Date) {
            if (haveDate) {
                uassert(4848404, "only one date allowed in an $add expression", !haveDate);
            }
            // Date is a simple 64 bit integer.
            haveDate = true;
            tag = value::TypeTags::NumberInt64;
        }
        if (value::isNumber(tag)) {
            resultTag = value::getWidestNumericalType(resultTag, tag);
        } else if (tag == value::TypeTags::Nothing || tag == value::TypeTags::Null) {
            // What to do about null and nothing?
            return {false, value::TypeTags::Nothing, 0};
        } else {
            // What to do about non-numeric types like arrays and objects?
            return {false, value::TypeTags::Nothing, 0};
        }
    }

    if (resultTag == value::TypeTags::NumberDecimal) {
        Decimal128 sum;
        for (ArityType idx = 0; idx < arity; ++idx) {
            auto [own, tag, val] = getFromStack(idx);
            if (tag == value::TypeTags::Date) {
                sum = sum.add(Decimal128(value::bitcastTo<int64_t>(val)));
            } else {
                sum = sum.add(value::numericCast<Decimal128>(tag, val));
            }
        }
        if (haveDate) {
            return {false, value::TypeTags::Date, value::bitcastFrom<int64_t>(sum.toLong())};
        } else {
            auto [tag, val] = value::makeCopyDecimal(sum);
            return {true, tag, val};
        }
    } else {
        DoubleDoubleSummation sum;
        for (ArityType idx = 0; idx < arity; ++idx) {
            auto [own, tag, val] = getFromStack(idx);
            if (tag == value::TypeTags::NumberInt32) {
                sum.addInt(value::numericCast<int32_t>(tag, val));
            } else if (tag == value::TypeTags::NumberInt64) {
                sum.addLong(value::numericCast<int64_t>(tag, val));
            } else if (tag == value::TypeTags::NumberDouble) {
                sum.addDouble(value::numericCast<double>(tag, val));
            } else if (tag == value::TypeTags::Date) {
                sum.addLong(value::bitcastTo<int64_t>(val));
            }
        }
        if (haveDate) {
            uassert(ErrorCodes::Overflow, "date overflow in $add", sum.fitsLong());
            return {false, value::TypeTags::Date, value::bitcastFrom<int64_t>(sum.getLong())};
        } else {
            switch (resultTag) {
                case value::TypeTags::NumberInt32: {
                    auto result = sum.getLong();
                    if (sum.fitsLong() && result >= std::numeric_limits<int32_t>::min() &&
                        result <= std::numeric_limits<int32_t>::max()) {
                        return {false,
                                value::TypeTags::NumberInt32,
                                value::bitcastFrom<int32_t>(result)};
                    }
                    [[fallthrough]];  // To the larger type
                }
                case value::TypeTags::NumberInt64: {
                    if (sum.fitsLong()) {
                        return {false,
                                value::TypeTags::NumberInt64,
                                value::bitcastFrom<int64_t>(sum.getLong())};
                    }
                    [[fallthrough]];  // To the larger type.
                }
                case value::TypeTags::NumberDouble: {
                    return {false,
                            value::TypeTags::NumberDouble,
                            value::bitcastFrom<double>(sum.getDouble())};
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }
    return {false, value::TypeTags::Nothing, 0};
}  // ByteCode::builtinDoubleDoubleSum

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinConvertSimpleSumToDoubleDoubleSum(
    ArityType arity) {
    invariant(arity == 1);
    auto [_, simpleSumTag, simpleSumVal] = getFromStack(0);
    return builtinConvertSimpleSumToDoubleDoubleSumImpl(simpleSumTag, simpleSumVal);
}

FastTuple<bool, value::TypeTags, value::Value>
ByteCode::builtinConvertSimpleSumToDoubleDoubleSumImpl(value::TypeTags simpleSumTag,
                                                       value::Value simpleSumVal) {
    auto [accTag, accVal] = genericInitializeDoubleDoubleSumState();
    value::ValueGuard accGuard{accTag, accVal};
    value::Array* accumulator = value::getArrayView(accVal);

    aggDoubleDoubleSumImpl(accumulator, simpleSumTag, simpleSumVal);

    accGuard.reset();
    return {true, accTag, accVal};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
