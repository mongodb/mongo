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

#include "mongo/pch.h" // for malloc/realloc/INFINITY pulled from bson

#include "mongo/bson/bsontypes.h"
#include "mongo/util/safe_num.h"

namespace mongo {

    SafeNum::SafeNum() : _type(EOO) {
    }

    SafeNum::SafeNum(const SafeNum& rhs) : _type(rhs._type), _value(rhs._value) {
    }

    SafeNum& SafeNum::operator=(const SafeNum& rhs) {
        _type = rhs._type;
        _value = rhs._value;
        return *this;
    }

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

    SafeNum::SafeNum(int num) : _type(NumberInt) {
        _value.int32Val = num;
    }

    SafeNum::SafeNum(long long int num) : _type(NumberLong) {
        _value.int64Val = num;
    }

    SafeNum::SafeNum(double num) : _type(NumberDouble) {
        _value.doubleVal = num;
    }

    SafeNum SafeNum::operator+(const SafeNum& rhs) const {
        return addInternal(*this, rhs);
    }

    SafeNum& SafeNum::operator+=(const SafeNum& rhs) {
        return *this = addInternal(*this, rhs);
    }

    SafeNum SafeNum::operator&(const SafeNum& rhs) const {
        return andInternal(*this, rhs);
    }

    SafeNum& SafeNum::operator&=(const SafeNum& rhs) {
        return *this = andInternal(*this, rhs);
    }

    SafeNum SafeNum::operator|(const SafeNum& rhs) const {
        return orInternal(*this, rhs);
    }

    SafeNum& SafeNum::operator|=(const SafeNum& rhs) {
        return *this = orInternal(*this, rhs);
    }

    bool SafeNum::operator==(const SafeNum& rhs) const {
        return isEquivalent(rhs);
    }

    bool SafeNum::operator!=(const SafeNum& rhs) const {
        return ! isEquivalent(rhs);
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

    //
    // addition support
    //

    SafeNum addInt32Int32(int lInt32, int rInt32) {
        int sum = lInt32 + rInt32;
        if ((sum < 0 && lInt32 > 0 && rInt32 > 0) ||
            (sum > 0 && lInt32 < 0 && rInt32 < 0)) {
            long long int result = static_cast<long long int>(lInt32) +
                                   static_cast<long long int>(rInt32);
            return SafeNum(result);
        }

        return SafeNum(sum);
    }

    SafeNum addInt64Int64(long long lInt64, long long rInt64) {
        long long sum = lInt64 + rInt64;
        if ((sum < 0 && lInt64 > 0 && rInt64 > 0) ||
            (sum > 0 && lInt64 < 0 && rInt64 < 0)) {
            return SafeNum();
        }

        return SafeNum(sum);
    }

    SafeNum addFloats(double lDouble, double rDouble) {
        double sum = lDouble + rDouble;
        return SafeNum(sum);
    }

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

} // namespace mongo
