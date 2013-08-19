/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <sstream>
#include <boost/static_assert.hpp>

#include "mongo/pch.h" // for malloc/realloc/INFINITY pulled from bson

#include "mongo/bson/bsontypes.h"
#include "mongo/util/safe_num.h"

namespace mongo {

    SafeNum::SafeNum(const BSONElement& element) {
        switch (element.type()) {
        case NumberInt:
            _type = NumberInt;
            _value.int32Val = element.Int();
            break;
        case NumberLong:
            _type = NumberLong;
            _value.int64Val = element.Long();
            break;
        case NumberDouble:
            _type = NumberDouble;
            _value.doubleVal = element.Double();
            break;
        default:
            _type = EOO;
        }
    }

    std::string SafeNum::debugString() const {
        ostringstream os;
        switch (_type) {
        case NumberInt:
            os << "(NumberInt)" << _value.int32Val;
            break;
        case NumberLong:
            os << "(NumberLong)" << _value.int64Val;
            break;
        case NumberDouble:
            os << "(NumberDouble)" << _value.doubleVal;
            break;
        case EOO:
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

        // If none of the sides is a double, compare them as long's.
        if (_type != NumberDouble && rhs._type != NumberDouble) {
            return getLongLong(*this) == getLongLong(rhs);
        }

        // If both sides are doubles, compare them as so.
        if (_type == NumberDouble && rhs._type == NumberDouble) {
            return _value.doubleVal == rhs._value.doubleVal;
        }

        // If we're mixing integers and doubles, we should be carefull. Some integers are
        // too big to be accuratelly represented in a double. If we're within a safe range
        // we compare both sides as doubles.
        const double lhsDouble = getDouble(*this);
        const double rhsDouble = getDouble(rhs);
        if (lhsDouble > -maxIntInDouble && lhsDouble < maxIntInDouble &&
            rhsDouble > -maxIntInDouble && rhsDouble < maxIntInDouble) {
            return lhsDouble == rhsDouble;
        }

        return false;
    }

    bool SafeNum::isIdentical(const SafeNum& rhs) const {
        if (_type != rhs._type) {
            return false;
        }

        switch (_type) {
        case NumberInt:
            return _value.int32Val == rhs._value.int32Val;
        case NumberLong:
            return _value.int64Val == rhs._value.int64Val;
        case NumberDouble:
            return _value.doubleVal == rhs._value.doubleVal;
        case EOO:
            // EOO doesn't match anything, including itself.
        default:
            return false;
        }
    }

    long long SafeNum::getLongLong(const SafeNum& snum) {
        switch (snum._type) {
        case NumberInt:
            return snum._value.int32Val;
        case NumberLong:
            return snum._value.int64Val;
        default:
            return 0;
        }
    }

    double SafeNum::getDouble(const SafeNum& snum) {
        switch (snum._type) {
        case NumberInt:
            return snum._value.int32Val;
        case NumberLong:
            return snum._value.int64Val;
        case NumberDouble:
            return snum._value.doubleVal;
        default:
            return 0.0;
        }
    }

    namespace {

        SafeNum addInt32Int32(int lInt32, int rInt32) {
            // NOTE: Please see "Secure Coding in C and C++", Second Edition, page 264-265 for
            // details on this algorithm (for an alternative resources, see
            //
            // https://www.securecoding.cert.org/confluence/display/seccode/INT32-C.+Ensure+that+operations+on+signed+integers+do+not+result+in+overflow?showComments=false).
            //
            // We are using the "Downcast from a larger type" algorithm here. We always perform
            // the arithmetic in 64-bit mode, which can never overflow for 32-bit
            // integers. Then, if we fall within the allowable range of int, we downcast,
            // otherwise, we retain the 64-bit result.

            // This algorithm is only correct if sizeof(long long) > sizeof(int)
            BOOST_STATIC_ASSERT(sizeof(long long) > sizeof(int));

            const long long int result =
                static_cast<long long int>(lInt32) +
                static_cast<long long int>(rInt32);

            if (result <= std::numeric_limits<int>::max() &&
                result >= std::numeric_limits<int>::min()) {
                return SafeNum(static_cast<int>(result));
            }

            return SafeNum(result);
        }

        SafeNum addInt64Int64(long long lInt64, long long rInt64) {
            // NOTE: Please see notes in addInt32Int32 above for references. In this case, since we
            // have no larger integer size, if our precondition test detects overflow we must
            // return an invalid SafeNum. Otherwise, the operation is safely performed by standard
            // arithmetic.
            if (((rInt64 > 0) && (lInt64 > (std::numeric_limits<long long>::max() - rInt64))) ||
                ((rInt64 < 0) && (lInt64 < (std::numeric_limits<long long>::min() - rInt64)))) {
                return SafeNum();
            }

            return SafeNum(lInt64 + rInt64);
        }

        SafeNum addFloats(double lDouble, double rDouble) {
            double sum = lDouble + rDouble;
            return SafeNum(sum);
        }

        SafeNum mulInt32Int32(int lInt32, int rInt32) {
            // NOTE: Please see "Secure Coding in C and C++", Second Edition, page 264-265 for
            // details on this algorithm (for an alternative resources, see
            //
            // https://www.securecoding.cert.org/confluence/display/seccode/INT32-C.+Ensure+that+operations+on+signed+integers+do+not+result+in+overflow?showComments=false).
            //
            // We are using the "Downcast from a larger type" algorithm here. We always perform
            // the arithmetic in 64-bit mode, which can never overflow for 32-bit
            // integers. Then, if we fall within the allowable range of int, we downcast,
            // otherwise, we retain the 64-bit result.

            // This algorithm is only correct if sizeof(long long) >= (2 * sizeof(int))
            BOOST_STATIC_ASSERT(sizeof(long long) >= (2 * sizeof(int)));

            const long long int result =
                static_cast<long long int>(lInt32) *
                static_cast<long long int>(rInt32);

            if (result <= std::numeric_limits<int>::max() &&
                result >= std::numeric_limits<int>::min()) {
                return SafeNum(static_cast<int>(result));
            }

            return SafeNum(result);
        }

        SafeNum mulInt64Int64(long long lInt64, long long rInt64) {
            // NOTE: Please see notes in mulInt32Int32 above for references. In this case,
            // since we have no larger integer size, if our precondition test detects overflow
            // we must return an invalid SafeNum. Otherwise, the operation is safely performed
            // by standard arithmetic.

            if (lInt64 > 0) {
                if (rInt64 > 0) {
                    if (lInt64 > (std::numeric_limits<long long>::max() / rInt64)) {
                        return SafeNum();
                    }
                }
                else {
                    if (rInt64 < (std::numeric_limits<long long>::min() / lInt64)) {
                        return SafeNum();
                    }
                }
            }
            else {
                if (rInt64 > 0) {
                    if (lInt64 < (std::numeric_limits<long long>::min() / rInt64)) {
                        return SafeNum();
                    }
                }
                else {
                    if ( (lInt64 != 0) &&
                         (rInt64 < (std::numeric_limits<long long>::max() / lInt64))) {
                        return SafeNum();
                    }
                }
            }

            const long long result = lInt64 * rInt64;
            return SafeNum(result);
        }

        SafeNum mulFloats(double lDouble, double rDouble) {
            const double product = lDouble * rDouble;
            return SafeNum(product);
        }

    } // namespace

    SafeNum SafeNum::addInternal(const SafeNum& lhs, const SafeNum& rhs) {
        BSONType lType = lhs._type;
        BSONType rType = rhs._type;

        if (lType == NumberInt && rType == NumberInt) {
            return addInt32Int32(lhs._value.int32Val, rhs._value.int32Val);
        }

        if (lType == NumberInt && rType == NumberLong) {
            return addInt64Int64(lhs._value.int32Val, rhs._value.int64Val);
        }

        if (lType == NumberLong && rType == NumberInt) {
            return addInt64Int64(lhs._value.int64Val, rhs._value.int32Val);
        }

        if (lType == NumberLong && rType == NumberLong) {
            return addInt64Int64(lhs._value.int64Val, rhs._value.int64Val);
        }

        if ((lType == NumberInt || lType == NumberLong || lType == NumberDouble) &&
            (rType == NumberInt || rType == NumberLong || rType == NumberDouble)) {
            return addFloats(getDouble(lhs), getDouble(rhs));
        }

        return SafeNum();
    }

    SafeNum SafeNum::mulInternal(const SafeNum& lhs, const SafeNum& rhs) {
        BSONType lType = lhs._type;
        BSONType rType = rhs._type;

        if (lType == NumberInt && rType == NumberInt) {
            return mulInt32Int32(lhs._value.int32Val, rhs._value.int32Val);
        }

        if (lType == NumberInt && rType == NumberLong) {
            return mulInt64Int64(lhs._value.int32Val, rhs._value.int64Val);
        }

        if (lType == NumberLong && rType == NumberInt) {
            return mulInt64Int64(lhs._value.int64Val, rhs._value.int32Val);
        }

        if (lType == NumberLong && rType == NumberLong) {
            return mulInt64Int64(lhs._value.int64Val, rhs._value.int64Val);
        }

        if ((lType == NumberInt || lType == NumberLong || lType == NumberDouble) &&
            (rType == NumberInt || rType == NumberLong || rType == NumberDouble)) {
            return mulFloats(getDouble(lhs), getDouble(rhs));
        }

        return SafeNum();
    }

    SafeNum SafeNum::andInternal(const SafeNum& lhs, const SafeNum& rhs) {
        const BSONType lType = lhs._type;
        const BSONType rType = rhs._type;

        if (lType == NumberInt && rType == NumberInt) {
            return (lhs._value.int32Val & rhs._value.int32Val);
        }

        if (lType == NumberInt && rType == NumberLong) {
            return (static_cast<long long int>(lhs._value.int32Val) & rhs._value.int64Val);
        }

        if (lType == NumberLong && rType == NumberInt) {
            return (lhs._value.int64Val & static_cast<long long int>(rhs._value.int32Val));
        }

        if (lType == NumberLong && rType == NumberLong) {
            return (lhs._value.int64Val & rhs._value.int64Val);
        }

        return SafeNum();
    }

    SafeNum SafeNum::orInternal(const SafeNum& lhs, const SafeNum& rhs) {
        const BSONType lType = lhs._type;
        const BSONType rType = rhs._type;

        if (lType == NumberInt && rType == NumberInt) {
            return (lhs._value.int32Val | rhs._value.int32Val);
        }

        if (lType == NumberInt && rType == NumberLong) {
            return (static_cast<long long int>(lhs._value.int32Val) | rhs._value.int64Val);
        }

        if (lType == NumberLong && rType == NumberInt) {
            return (lhs._value.int64Val | static_cast<long long int>(rhs._value.int32Val));
        }

        if (lType == NumberLong && rType == NumberLong) {
            return (lhs._value.int64Val | rhs._value.int64Val);
        }

        return SafeNum();
    }

    SafeNum SafeNum::xorInternal(const SafeNum& lhs, const SafeNum& rhs) {
        const BSONType lType = lhs._type;
        const BSONType rType = rhs._type;

        if (lType == NumberInt && rType == NumberInt) {
            return (lhs._value.int32Val ^ rhs._value.int32Val);
        }

        if (lType == NumberInt && rType == NumberLong) {
            return (static_cast<long long int>(lhs._value.int32Val) ^ rhs._value.int64Val);
        }

        if (lType == NumberLong && rType == NumberInt) {
            return (lhs._value.int64Val ^ static_cast<long long int>(rhs._value.int32Val));
        }

        if (lType == NumberLong && rType == NumberLong) {
            return (lhs._value.int64Val ^ rhs._value.int64Val);
        }

        return SafeNum();
    }

} // namespace mongo
