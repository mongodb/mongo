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

#include "mongo/util/safe_num.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"

#include <limits>
#include <sstream>

namespace mongo {

using std::ostringstream;

SafeNum::SafeNum(const BSONElement& element) {
    switch (element.type()) {
        case BSONType::numberInt:
            _type = BSONType::numberInt;
            _value.int32Val = element.Int();
            break;
        case BSONType::numberLong:
            _type = BSONType::numberLong;
            _value.int64Val = element.Long();
            break;
        case BSONType::numberDouble:
            _type = BSONType::numberDouble;
            _value.doubleVal = element.Double();
            break;
        case BSONType::numberDecimal:
            _type = BSONType::numberDecimal;
            _value.decimalVal = element.Decimal().getValue();
            break;
        default:
            _type = BSONType::eoo;
    }
}

std::string SafeNum::debugString() const {
    ostringstream os;
    switch (_type) {
        case BSONType::numberInt:
            os << "(NumberInt)" << _value.int32Val;
            break;
        case BSONType::numberLong:
            os << "(NumberLong)" << _value.int64Val;
            break;
        case BSONType::numberDouble:
            os << "(NumberDouble)" << _value.doubleVal;
            break;
        case BSONType::numberDecimal:
            os << "(NumberDecimal)" << getDecimal(*this).toString();
            break;
        case BSONType::eoo:
            os << "(EOO)";
            break;
        default:
            os << "(unknown type)";
    }

    return os.str();
}

std::ostream& operator<<(std::ostream& os, const SafeNum& snum) {
    return os << snum.debugString();
}

//
// comparison support
//

bool SafeNum::isEquivalent(const SafeNum& rhs) const {
    if (!isValid() && !rhs.isValid()) {
        return true;
    }

    // EOO is not equivalent to anything else.
    if (!isValid() || !rhs.isValid()) {
        return false;
    }

    // If the types of either side are mixed, we'll try to find the shortest type we
    // can upconvert to that would not sacrifice the accuracy in the process.

    // If one side is a decimal, compare both sides as decimals.
    if (_type == BSONType::numberDecimal || rhs._type == BSONType::numberDecimal) {
        // Note: isEqual is faster than using compareDecimals, however it does not handle
        // comparing NaN as equal (differing from BSONElement::woCompare).  This case
        // is not handled for double comparison above eihter.
        return getDecimal(*this).isEqual(getDecimal(rhs));
    }

    // If none of the sides is a double, compare them as long's.
    if (_type != BSONType::numberDouble && rhs._type != BSONType::numberDouble) {
        return getInt64(*this) == getInt64(rhs);
    }

    // If both sides are doubles, compare them as so.
    if (_type == BSONType::numberDouble && rhs._type == BSONType::numberDouble) {
        return _value.doubleVal == rhs._value.doubleVal;
    }

    // If we're mixing integers and doubles, we should be careful. Some integers are
    // too big to be accuratelly represented in a double. If we're within a safe range
    // we compare both sides as doubles.
    const double lhsDouble = getDouble(*this);
    const double rhsDouble = getDouble(rhs);
    if (lhsDouble > -maxIntInDouble && lhsDouble < maxIntInDouble && rhsDouble > -maxIntInDouble &&
        rhsDouble < maxIntInDouble) {
        return lhsDouble == rhsDouble;
    }

    return false;
}

bool SafeNum::isIdentical(const SafeNum& rhs) const {
    if (_type != rhs._type) {
        return false;
    }

    switch (_type) {
        case BSONType::numberInt:
            return _value.int32Val == rhs._value.int32Val;
        case BSONType::numberLong:
            return _value.int64Val == rhs._value.int64Val;
        case BSONType::numberDouble:
            return _value.doubleVal == rhs._value.doubleVal;
        case BSONType::numberDecimal:
            return Decimal128(_value.decimalVal).isEqual(Decimal128(rhs._value.decimalVal));
        case BSONType::eoo:
        // EOO doesn't match anything, including itself.
        default:
            return false;
    }
}

void SafeNum::toBSON(StringData fieldName, BSONObjBuilder* bob) const {
    switch (_type) {
        case BSONType::numberInt:
            bob->append(fieldName, _value.int32Val);
            return;
        case BSONType::numberLong:
            bob->append(fieldName, _value.int64Val);
            return;
        case BSONType::numberDouble:
            bob->append(fieldName, _value.doubleVal);
            return;
        case BSONType::numberDecimal:
            bob->append(fieldName, Decimal128(_value.decimalVal));
            return;
        default:
            MONGO_UNREACHABLE;
    }
}

int64_t SafeNum::getInt64(const SafeNum& snum) {
    switch (snum._type) {
        case BSONType::numberInt:
            return snum._value.int32Val;
        case BSONType::numberLong:
            return snum._value.int64Val;
        default:
            return 0;
    }
}

double SafeNum::getDouble(const SafeNum& snum) {
    switch (snum._type) {
        case BSONType::numberInt:
            return snum._value.int32Val;
        case BSONType::numberLong:
            return snum._value.int64Val;
        case BSONType::numberDouble:
            return snum._value.doubleVal;
        case BSONType::numberDecimal:
            return Decimal128(snum._value.decimalVal).toDouble();
        default:
            return 0.0;
    }
}

Decimal128 SafeNum::getDecimal(const SafeNum& snum) {
    switch (snum._type) {
        case BSONType::numberInt:
            return Decimal128(snum._value.int32Val);
        case BSONType::numberLong:
            return Decimal128(snum._value.int64Val);
        case BSONType::numberDouble:
            return Decimal128(snum._value.doubleVal, Decimal128::kRoundTo15Digits);
        case BSONType::numberDecimal:
            return Decimal128(snum._value.decimalVal);
        default:
            return Decimal128::kNormalizedZero;
    }
}

namespace {

SafeNum addInt32Int32(int32_t lInt32, int32_t rInt32) {
    // NOTE: Please see "Secure Coding in C and C++", Second Edition, page 264-265 for
    // details on this algorithm (for an alternative resources, see
    //
    // https://www.securecoding.cert.org/confluence/display/seccode/
    // INT32-C.+Ensure+that+operations+on+signed+integers+do+not+result+in+overflow?
    // showComments=false).
    //
    // We are using the "Downcast from a larger type" algorithm here. We always perform
    // the arithmetic in 64-bit mode, which can never overflow for 32-bit
    // integers. Then, if we fall within the allowable range of int32_t, we downcast,
    // otherwise, we retain the 64-bit result.
    const int64_t result = static_cast<int64_t>(lInt32) + static_cast<int64_t>(rInt32);

    if (result <= std::numeric_limits<int32_t>::max() &&
        result >= std::numeric_limits<int32_t>::min()) {
        return SafeNum(static_cast<int32_t>(result));
    }

    return SafeNum(result);
}

SafeNum addInt64Int64(int64_t lInt64, int64_t rInt64) {
    // NOTE: Please see notes in addInt32Int32 above for references. In this case, since we
    // have no larger integer size, if our precondition test detects overflow we must
    // return an invalid SafeNum. Otherwise, the operation is safely performed by standard
    // arithmetic.
    if (((rInt64 > 0) && (lInt64 > (std::numeric_limits<int64_t>::max() - rInt64))) ||
        ((rInt64 < 0) && (lInt64 < (std::numeric_limits<int64_t>::min() - rInt64)))) {
        return SafeNum();
    }

    return SafeNum(lInt64 + rInt64);
}

SafeNum addFloats(double lDouble, double rDouble) {
    double sum = lDouble + rDouble;
    return SafeNum(sum);
}

SafeNum addDecimals(Decimal128 lDecimal, Decimal128 rDecimal) {
    return SafeNum(lDecimal.add(rDecimal));
}

SafeNum mulInt32Int32(int32_t lInt32, int32_t rInt32) {
    // NOTE: Please see "Secure Coding in C and C++", Second Edition, page 264-265 for
    // details on this algorithm (for an alternative resources, see
    //
    // https://www.securecoding.cert.org/confluence/display/seccode/
    // INT32-C.+Ensure+that+operations+on+signed+integers+do+not+result+in+overflow?
    // showComments=false).
    //
    // We are using the "Downcast from a larger type" algorithm here. We always perform
    // the arithmetic in 64-bit mode, which can never overflow for 32-bit
    // integers. Then, if we fall within the allowable range of int32_t, we downcast,
    // otherwise, we retain the 64-bit result.
    const int64_t result = static_cast<int64_t>(lInt32) * static_cast<int64_t>(rInt32);

    if (result <= std::numeric_limits<int32_t>::max() &&
        result >= std::numeric_limits<int32_t>::min()) {
        return SafeNum(static_cast<int32_t>(result));
    }

    return SafeNum(result);
}

SafeNum mulInt64Int64(int64_t lInt64, int64_t rInt64) {
    // NOTE: Please see notes in mulInt32Int32 above for references. In this case,
    // since we have no larger integer size, if our precondition test detects overflow
    // we must return an invalid SafeNum. Otherwise, the operation is safely performed
    // by standard arithmetic.

    if (lInt64 > 0) {
        if (rInt64 > 0) {
            if (lInt64 > (std::numeric_limits<int64_t>::max() / rInt64)) {
                return SafeNum();
            }
        } else {
            if (rInt64 < (std::numeric_limits<int64_t>::min() / lInt64)) {
                return SafeNum();
            }
        }
    } else {
        if (rInt64 > 0) {
            if (lInt64 < (std::numeric_limits<int64_t>::min() / rInt64)) {
                return SafeNum();
            }
        } else {
            if ((lInt64 != 0) && (rInt64 < (std::numeric_limits<int64_t>::max() / lInt64))) {
                return SafeNum();
            }
        }
    }

    const int64_t result = lInt64 * rInt64;
    return SafeNum(result);
}

SafeNum mulFloats(double lDouble, double rDouble) {
    const double product = lDouble * rDouble;
    return SafeNum(product);
}

SafeNum mulDecimals(Decimal128 lDecimal, Decimal128 rDecimal) {
    return SafeNum(lDecimal.multiply(rDecimal));
}

}  // namespace

SafeNum SafeNum::addInternal(const SafeNum& lhs, const SafeNum& rhs) {
    BSONType lType = lhs._type;
    BSONType rType = rhs._type;

    if (lType == BSONType::numberInt && rType == BSONType::numberInt) {
        return addInt32Int32(lhs._value.int32Val, rhs._value.int32Val);
    }

    if (lType == BSONType::numberInt && rType == BSONType::numberLong) {
        return addInt64Int64(lhs._value.int32Val, rhs._value.int64Val);
    }

    if (lType == BSONType::numberLong && rType == BSONType::numberInt) {
        return addInt64Int64(lhs._value.int64Val, rhs._value.int32Val);
    }

    if (lType == BSONType::numberLong && rType == BSONType::numberLong) {
        return addInt64Int64(lhs._value.int64Val, rhs._value.int64Val);
    }

    if (lType == BSONType::numberDecimal || rType == BSONType::numberDecimal) {
        return addDecimals(getDecimal(lhs), getDecimal(rhs));
    }

    if ((lType == BSONType::numberInt || lType == BSONType::numberLong ||
         lType == BSONType::numberDouble) &&
        (rType == BSONType::numberInt || rType == BSONType::numberLong ||
         rType == BSONType::numberDouble)) {
        return addFloats(getDouble(lhs), getDouble(rhs));
    }

    return SafeNum();
}

SafeNum SafeNum::mulInternal(const SafeNum& lhs, const SafeNum& rhs) {
    BSONType lType = lhs._type;
    BSONType rType = rhs._type;

    if (lType == BSONType::numberInt && rType == BSONType::numberInt) {
        return mulInt32Int32(lhs._value.int32Val, rhs._value.int32Val);
    }

    if (lType == BSONType::numberInt && rType == BSONType::numberLong) {
        return mulInt64Int64(lhs._value.int32Val, rhs._value.int64Val);
    }

    if (lType == BSONType::numberLong && rType == BSONType::numberInt) {
        return mulInt64Int64(lhs._value.int64Val, rhs._value.int32Val);
    }

    if (lType == BSONType::numberLong && rType == BSONType::numberLong) {
        return mulInt64Int64(lhs._value.int64Val, rhs._value.int64Val);
    }

    if (lType == BSONType::numberDecimal || rType == BSONType::numberDecimal) {
        return mulDecimals(getDecimal(lhs), getDecimal(rhs));
    }

    if ((lType == BSONType::numberInt || lType == BSONType::numberLong ||
         lType == BSONType::numberDouble) &&
        (rType == BSONType::numberInt || rType == BSONType::numberLong ||
         rType == BSONType::numberDouble)) {
        return mulFloats(getDouble(lhs), getDouble(rhs));
    }

    return SafeNum();
}

SafeNum SafeNum::andInternal(const SafeNum& lhs, const SafeNum& rhs) {
    const BSONType lType = lhs._type;
    const BSONType rType = rhs._type;

    if (lType == BSONType::numberInt && rType == BSONType::numberInt) {
        return (lhs._value.int32Val & rhs._value.int32Val);
    }

    if (lType == BSONType::numberInt && rType == BSONType::numberLong) {
        return (static_cast<int64_t>(lhs._value.int32Val) & rhs._value.int64Val);
    }

    if (lType == BSONType::numberLong && rType == BSONType::numberInt) {
        return (lhs._value.int64Val & static_cast<int64_t>(rhs._value.int32Val));
    }

    if (lType == BSONType::numberLong && rType == BSONType::numberLong) {
        return (lhs._value.int64Val & rhs._value.int64Val);
    }

    return SafeNum();
}

SafeNum SafeNum::orInternal(const SafeNum& lhs, const SafeNum& rhs) {
    const BSONType lType = lhs._type;
    const BSONType rType = rhs._type;

    if (lType == BSONType::numberInt && rType == BSONType::numberInt) {
        return (lhs._value.int32Val | rhs._value.int32Val);
    }

    if (lType == BSONType::numberInt && rType == BSONType::numberLong) {
        return (static_cast<int64_t>(lhs._value.int32Val) | rhs._value.int64Val);
    }

    if (lType == BSONType::numberLong && rType == BSONType::numberInt) {
        return (lhs._value.int64Val | static_cast<int64_t>(rhs._value.int32Val));
    }

    if (lType == BSONType::numberLong && rType == BSONType::numberLong) {
        return (lhs._value.int64Val | rhs._value.int64Val);
    }

    return SafeNum();
}

SafeNum SafeNum::xorInternal(const SafeNum& lhs, const SafeNum& rhs) {
    const BSONType lType = lhs._type;
    const BSONType rType = rhs._type;

    if (lType == BSONType::numberInt && rType == BSONType::numberInt) {
        return (lhs._value.int32Val ^ rhs._value.int32Val);
    }

    if (lType == BSONType::numberInt && rType == BSONType::numberLong) {
        return (static_cast<int64_t>(lhs._value.int32Val) ^ rhs._value.int64Val);
    }

    if (lType == BSONType::numberLong && rType == BSONType::numberInt) {
        return (lhs._value.int64Val ^ static_cast<int64_t>(rhs._value.int32Val));
    }

    if (lType == BSONType::numberLong && rType == BSONType::numberLong) {
        return (lhs._value.int64Val ^ rhs._value.int64Val);
    }

    return SafeNum();
}

}  // namespace mongo
