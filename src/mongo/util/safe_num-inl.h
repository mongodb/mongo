/*    Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

namespace mongo {

inline SafeNum::SafeNum() : _type(EOO) {}

inline SafeNum::~SafeNum() {}

inline SafeNum::SafeNum(const SafeNum& rhs) : _type(rhs._type), _value(rhs._value) {}

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

inline SafeNum::SafeNum(Decimal128 num) : _type(NumberDecimal) {
    _value.decimalVal = num.getValue();
}

inline bool SafeNum::operator==(const SafeNum& rhs) const {
    return isEquivalent(rhs);
}

inline bool SafeNum::operator!=(const SafeNum& rhs) const {
    return !isEquivalent(rhs);
}

inline SafeNum SafeNum::operator+(const SafeNum& rhs) const {
    return addInternal(*this, rhs);
}

inline SafeNum& SafeNum::operator+=(const SafeNum& rhs) {
    return * this = addInternal(*this, rhs);
}

inline SafeNum SafeNum::operator*(const SafeNum& rhs) const {
    return mulInternal(*this, rhs);
}

inline SafeNum& SafeNum::operator*=(const SafeNum& rhs) {
    return * this = mulInternal(*this, rhs);
}

inline SafeNum SafeNum::bitAnd(const SafeNum& rhs) const {
    return andInternal(*this, rhs);
}

inline SafeNum SafeNum::operator&(const SafeNum& rhs) const {
    return bitAnd(rhs);
}

inline SafeNum& SafeNum::operator&=(const SafeNum& rhs) {
    return * this = bitAnd(rhs);
}

inline SafeNum SafeNum::bitOr(const SafeNum& rhs) const {
    return orInternal(*this, rhs);
}

inline SafeNum SafeNum::operator|(const SafeNum& rhs) const {
    return bitOr(rhs);
}

inline SafeNum& SafeNum::operator|=(const SafeNum& rhs) {
    return * this = bitOr(rhs);
}

inline SafeNum SafeNum::bitXor(const SafeNum& rhs) const {
    return xorInternal(*this, rhs);
}

inline SafeNum SafeNum::operator^(const SafeNum& rhs) const {
    return bitXor(rhs);
}

inline SafeNum& SafeNum::operator^=(const SafeNum& rhs) {
    return * this = bitXor(rhs);
}

inline bool SafeNum::isValid() const {
    return _type != EOO;
}

inline BSONType SafeNum::type() const {
    return _type;
}

}  // namespace mongo
