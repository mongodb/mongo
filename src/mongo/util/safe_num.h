/*    Copyright 2012 10gen Inc.
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

#include <iosfwd>
#include <string>

#include "mongo/db/jsobj.h"

namespace mongo {

namespace mutablebson {
class Element;
class Document;
}

/**
 * SafeNum holds and does arithmetic on a number in a safe way, handling overflow
 * and casting for the user. 32-bit integers will overflow into 64-bit integers. But
 * 64-bit integers will NOT overflow to doubles. Also, this class does NOT
 * downcast. Doubles will NOT overflow to decimal, but mixed type arithmetic with a decimal
 * will. This class should be as conservative as possible about upcasting, but
 * should never lose precision.
 *
 * This class does not throw any exceptions, so the user should call type() before
 * using a SafeNum to ensure that it is valid.  A SafeNum could be invalid
 * from creation (if, for example, a non-numeric BSONElement was passed to the
 * constructor) or due to overflow.  NAN is a valid value.
 *
 * Usage example:
 *
 *      SafeNum counter(doc["count"]);
 *
 *      SafeNum newValue = counter + 10;
 *      // check if valid
 *      if (newValue.type() == EOO) {
 *          return;
 *      }
 *      // append SafeNum to a BSONObj
 *      bsonObjBuilder.append(newValue);
 *
 */
class SafeNum {
public:
    SafeNum();
    ~SafeNum();

    //
    // construction support
    //

    // Copy ctor and assignment are allowed.
    SafeNum(const SafeNum& rhs);
    SafeNum& operator=(const SafeNum& rhs);

    // These implicit conversions are allowed.
    SafeNum(const BSONElement& element);
    SafeNum(int32_t num);
    SafeNum(int64_t num);
    SafeNum(double num);
    SafeNum(Decimal128 num);

    // Other/Implicit conversions are not allowed.
    template <typename T>
    SafeNum(T t) = delete;

    // TODO: add Paul's mutablebson::Element ctor

    //
    // comparison support
    //

    /**
     * Returns true if the numeric quantity of 'rhs' and 'this' are the same. That is,
     * an int32(10), an int64(10), a double(10), and a decimal(10) are equivalent. An
     * EOO-typed safe num is equivalent only to another EOO-typed instance. Otherwise,
     * returns false.
     */
    bool isEquivalent(const SafeNum& rhs) const;
    bool operator==(const SafeNum& rhs) const;
    bool operator!=(const SafeNum& rhs) const;

    /**
     * Returns true if 'rsh' is equivalent to 'this' (see isEquivalent) _and_ both
     * types are exactly the same. An EOO-typed safe num is never identical to
     * anything else, even another EOO-typed instance. Otherwise, returns false.
     */
    bool isIdentical(const SafeNum& rhs) const;

    //
    // arithmetic support
    //

    /**
     * Sums the 'rhs' -- right-hand side -- safe num with this, taking care of
     * upconversions and overflow (see class header).
     */
    SafeNum operator+(const SafeNum& rhs) const;
    SafeNum& operator+=(const SafeNum& rhs);

    /**
     * Multiplies the 'rhs' -- right-hand side -- safe num with this, taking care of
     * upconversions and overflow (see class header).
     */
    SafeNum operator*(const SafeNum& rhs) const;
    SafeNum& operator*=(const SafeNum& rhs);

    //
    // logical operation support. Note that these operations are only supported for
    // integral types. Attempts to apply with either side holding a double or decimal
    // value will result in an EOO typed safenum.
    //

    // Bitwise 'and' support
    SafeNum bitAnd(const SafeNum& rhs) const;
    SafeNum operator&(const SafeNum& rhs) const;
    SafeNum& operator&=(const SafeNum& rhs);

    // Bitwise 'or' support
    SafeNum bitOr(const SafeNum& rhs) const;
    SafeNum operator|(const SafeNum& rhs) const;
    SafeNum& operator|=(const SafeNum& rhs);

    // Bitwise 'xor' support
    SafeNum bitXor(const SafeNum& rhs) const;
    SafeNum operator^(const SafeNum& rhs) const;
    SafeNum& operator^=(const SafeNum& rhs);


    //
    // output support
    //

    friend class mutablebson::Element;
    friend class mutablebson::Document;

    // TODO: output to builder

    //
    // accessors
    //
    bool isValid() const;
    BSONType type() const;
    std::string debugString() const;

    //
    // Below exposed for testing purposes. Treat as private.
    //

    // Maximum integer that can be converted accuratelly into a double, assuming a
    // double precission IEEE 754 representation.
    // TODO use numeric_limits to make this portable
    static const int64_t maxIntInDouble = 9007199254740992LL;  // 2^53

private:
    // One of the following: NumberInt, NumberLong, NumberDouble, NumberDecimal, or EOO.
    BSONType _type;

    // Value of the safe num. Indeterminate if _type is EOO.
    union {
        int32_t int32Val;
        int64_t int64Val;
        double doubleVal;
        Decimal128::Value decimalVal;
    } _value;

    /**
     * Returns the sum of 'lhs' and 'rhs', taking into consideration their types. The
     * type of the result would upcast, if necessary and permitted. Otherwise, returns
     * an EOO-type instance.
     */
    static SafeNum addInternal(const SafeNum& lhs, const SafeNum& rhs);

    /**
     * Returns the product of 'lhs' and 'rhs', taking into consideration their types. The
     * type of the result would upcast, if necessary and permitted. Otherwise, returns an
     * EOO-type instance.
     */
    static SafeNum mulInternal(const SafeNum& lhs, const SafeNum& rhs);

    /** Returns the bitwise 'and' of lhs and rhs, taking into consideration their types. If
     *  the operation is invalid for the underlying types, returns an EOO instance.
     */
    static SafeNum andInternal(const SafeNum& lhs, const SafeNum& rhs);

    /** Returns the bitwise 'or' of lhs and rhs, taking into consideration their types. If
     *  the operation is invalid for the underlying types, returns an EOO instance.
     */
    static SafeNum orInternal(const SafeNum& lhs, const SafeNum& rhs);

    /** Returns the bitwise 'xor' of lhs and rhs, taking into consideration their types. If
     *  the operation is invalid for the underlying types, returns an EOO instance.
     */
    static SafeNum xorInternal(const SafeNum& lhs, const SafeNum& rhs);

    /**
     * Extracts the value of 'snum' in a int64_t format. It assumes 'snum' is an NumberInt
     * or a NumberLong.
     */
    static int64_t getInt64(const SafeNum& snum);

    /**
     * Extracts the value of 'snum' in a double format. It assumes 'snum' is a valid
     * SafeNum, i.e., that _type is not EOO.
     */
    static double getDouble(const SafeNum& snum);

    /**
     * Extracts the value of 'snum' in a Decimal128 format.  It assumes 'snum' is an
     * NumberInt, NumberDouble, or NumberLong.  Integral values are converted exactly.
     * NumberDouble is converted to 15 digits of precision, as defined in Decimal128.
     */
    static Decimal128 getDecimal(const SafeNum& snum);
};

// Convenience method for unittest code. Please use accessors otherwise.
std::ostream& operator<<(std::ostream& os, const SafeNum& snum);

}  // namespace mongo

#include "mongo/util/safe_num-inl.h"
