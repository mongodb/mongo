/*    Copyright 2013 10gen Inc.
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

#pragma once

namespace mongo {

    inline SafeNum::SafeNum() : _type(EOO) {
    }

    inline SafeNum::~SafeNum() {
    }

    inline SafeNum::SafeNum(const SafeNum& rhs) : _type(rhs._type), _value(rhs._value) {
    }

    inline SafeNum& SafeNum::operator=(const SafeNum& rhs) {
        _type = rhs._type;
        _value = rhs._value;
        return *this;
    }

    inline SafeNum::SafeNum(int num) : _type(NumberInt) {
        _value.int32Val = num;
    }

    inline SafeNum::SafeNum(long long int num) : _type(NumberLong) {
        _value.int64Val = num;
    }

    inline SafeNum::SafeNum(double num) : _type(NumberDouble) {
        _value.doubleVal = num;
    }

    inline bool SafeNum::operator==(const SafeNum& rhs) const {
        return isEquivalent(rhs);
    }

    inline bool SafeNum::operator!=(const SafeNum& rhs) const {
        return ! isEquivalent(rhs);
    }

    inline SafeNum SafeNum::operator+(const SafeNum& rhs) const {
        return addInternal(*this, rhs);
    }

    inline SafeNum& SafeNum::operator+=(const SafeNum& rhs) {
        return *this = addInternal(*this, rhs);
    }

    inline SafeNum SafeNum::operator*(const SafeNum& rhs) const {
        return mulInternal(*this, rhs);
    }

    inline SafeNum& SafeNum::operator*=(const SafeNum& rhs) {
        return *this = mulInternal(*this, rhs);
    }

    inline SafeNum SafeNum::bitAnd(const SafeNum& rhs) const {
        return andInternal(*this, rhs);
    }

    inline SafeNum SafeNum::operator&(const SafeNum& rhs) const {
        return bitAnd(rhs);
    }

    inline SafeNum& SafeNum::operator&=(const SafeNum& rhs) {
        return *this = bitAnd(rhs);
    }

    inline SafeNum SafeNum::bitOr(const SafeNum& rhs) const {
        return orInternal(*this, rhs);
    }

    inline SafeNum SafeNum::operator|(const SafeNum& rhs) const {
        return bitOr(rhs);
    }

    inline SafeNum& SafeNum::operator|=(const SafeNum& rhs) {
        return *this = bitOr(rhs);
    }

    inline SafeNum SafeNum::bitXor(const SafeNum& rhs) const {
        return xorInternal(*this, rhs);
    }

    inline SafeNum SafeNum::operator^(const SafeNum& rhs) const {
        return bitXor(rhs);
    }

    inline SafeNum& SafeNum::operator^=(const SafeNum& rhs) {
        return *this = bitXor(rhs);
    }

    inline bool SafeNum::isValid() const {
        return _type != EOO;
    }

    inline BSONType SafeNum::type() const {
        return _type;
    }

} // namespace mongo
