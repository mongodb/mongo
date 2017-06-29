// bsonelement.h

/*    Copyright 2009 10gen Inc.
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

#include <cmath>
#include <cstdint>
#include <string.h>  // strlen
#include <string>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/string_data_comparator_interface.h"
#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/config.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/strnlen.h"

namespace mongo {
class BSONObj;
class BSONElement;
class BSONObjBuilder;
class Timestamp;

typedef BSONElement be;
typedef BSONObj bo;
typedef BSONObjBuilder bob;

/** l and r MUST have same type when called: check that first.
    If comparator is non-null, it is used for all comparisons between two strings.
*/
int compareElementValues(const BSONElement& l,
                         const BSONElement& r,
                         const StringData::ComparatorInterface* comparator = nullptr);

/** BSONElement represents an "element" in a BSONObj.  So for the object { a : 3, b : "abc" },
    'a : 3' is the first element (key+value).

    The BSONElement object points into the BSONObj's data.  Thus the BSONObj must stay in scope
    for the life of the BSONElement.

    internals:
    <type><fieldName    ><value>
    -------- size() ------------
    -fieldNameSize-
    value()
    type()
*/
class BSONElement {
public:
    // Declared in bsonobj_comparator_interface.h.
    class ComparatorInterface;

    /**
     * Operator overloads for relops return a DeferredComparison which can subsequently be evaluated
     * by a BSONObj::ComparatorInterface.
     */
    using DeferredComparison = BSONComparatorInterfaceBase<BSONElement>::DeferredComparison;

    /** These functions, which start with a capital letter, throw a MsgAssertionException if the
        element is not of the required type. Example:

        std::string foo = obj["foo"].String(); // std::exception if not a std::string type or DNE
    */
    std::string String() const {
        return chk(mongo::String).str();
    }
    const StringData checkAndGetStringData() const {
        return chk(mongo::String).valueStringData();
    }
    Date_t Date() const {
        return chk(mongo::Date).date();
    }
    double Number() const {
        return chk(isNumber()).number();
    }
    Decimal128 Decimal() const {
        return chk(NumberDecimal)._numberDecimal();
    }
    double Double() const {
        return chk(NumberDouble)._numberDouble();
    }
    long long Long() const {
        return chk(NumberLong)._numberLong();
    }
    int Int() const {
        return chk(NumberInt)._numberInt();
    }
    bool Bool() const {
        return chk(mongo::Bool).boolean();
    }
    std::vector<BSONElement> Array() const;  // see implementation for detailed comments
    mongo::OID OID() const {
        return chk(jstOID).__oid();
    }
    void Null() const {
        chk(isNull());
    }  // throw MsgAssertionException if not null
    void OK() const {
        chk(ok());
    }  // throw MsgAssertionException if element DNE

    /** @return the embedded object associated with this field.
        Note the returned object is a reference to within the parent bson object. If that
        object is out of scope, this pointer will no longer be valid. Call getOwned() on the
        returned BSONObj if you need your own copy.
        throws UserException if the element is not of type object.
    */
    BSONObj Obj() const;

    /** populate v with the value of the element.  If type does not match, throw exception.
        useful in templates -- see also BSONObj::Vals().
    */
    void Val(Date_t& v) const {
        v = Date();
    }
    void Val(long long& v) const {
        v = Long();
    }
    void Val(Decimal128& v) const {
        v = Decimal();
    }
    void Val(bool& v) const {
        v = Bool();
    }
    void Val(BSONObj& v) const;
    void Val(mongo::OID& v) const {
        v = OID();
    }
    void Val(int& v) const {
        v = Int();
    }
    void Val(double& v) const {
        v = Double();
    }
    void Val(std::string& v) const {
        v = String();
    }

    /** Use ok() to check if a value is assigned:
        if( myObj["foo"].ok() ) ...
    */
    bool ok() const {
        return !eoo();
    }

    /**
     * True if this element has a value (ie not EOO).
     *
     * Makes it easier to check for a field's existence and use it:
     * if (auto elem = myObj["foo"]) {
     *     // Use elem
     * }
     * else {
     *     // default behavior
     * }
     */
    explicit operator bool() const {
        return ok();
    }

    std::string toString(bool includeFieldName = true, bool full = false) const;
    void toString(StringBuilder& s,
                  bool includeFieldName = true,
                  bool full = false,
                  bool redactValues = false,
                  int depth = 0) const;
    std::string jsonString(JsonStringFormat format,
                           bool includeFieldNames = true,
                           int pretty = 0) const;
    operator std::string() const {
        return toString();
    }

    /** Returns the type of the element */
    BSONType type() const {
        const signed char typeByte = ConstDataView(data).read<signed char>();
        return static_cast<BSONType>(typeByte);
    }

    /** retrieve a field within this element
        throws exception if *this is not an embedded object
    */
    BSONElement operator[](StringData field) const;

    /** See canonicalizeBSONType in bsontypes.h */
    int canonicalType() const {
        return canonicalizeBSONType(type());
    }

    /** Indicates if it is the end-of-object element, which is present at the end of
        every BSON object.
    */
    bool eoo() const {
        return type() == EOO;
    }

    /** Size of the element.
        @param maxLen If maxLen is specified, don't scan more than maxLen bytes to calculate size.
    */
    int size(int maxLen) const;
    int size() const;

    /** Wrap this element up as a singleton object. */
    BSONObj wrap() const;

    /** Wrap this element up as a singleton object with a new name. */
    BSONObj wrap(StringData newName) const;

    /** field name of the element.  e.g., for
        name : "Joe"
        "name" is the fieldname
    */
    const char* fieldName() const {
        if (eoo())
            return "";  // no fieldname for it.
        return data + 1;
    }

    /**
     * NOTE: size includes the NULL terminator.
     */
    int fieldNameSize() const {
        if (fieldNameSize_ == -1)
            fieldNameSize_ = (int)strlen(fieldName()) + 1;
        return fieldNameSize_;
    }

    const StringData fieldNameStringData() const {
        return StringData(fieldName(), eoo() ? 0 : fieldNameSize() - 1);
    }

    /** raw data of the element's value (so be careful). */
    const char* value() const {
        return (data + fieldNameSize() + 1);
    }
    /** size in bytes of the element's value (when applicable). */
    int valuesize() const {
        return size() - fieldNameSize() - 1;
    }

    bool isBoolean() const {
        return type() == mongo::Bool;
    }

    /** @return value of a boolean element.
        You must assure element is a boolean before
        calling. */
    bool boolean() const {
        return *value() ? true : false;
    }

    bool booleanSafe() const {
        return isBoolean() && boolean();
    }

    /** Retrieve a java style date value from the element.
        Ensure element is of type Date before calling.
        @see Bool(), trueValue()
    */
    Date_t date() const {
        return Date_t::fromMillisSinceEpoch(ConstDataView(value()).read<LittleEndian<long long>>());
    }

    /** Convert the value to boolean, regardless of its type, in a javascript-like fashion
        (i.e., treats zero and null and eoo as false).
    */
    bool trueValue() const;

    /** True if element is of a numeric type. */
    bool isNumber() const;

    /** Return double value for this field. MUST be NumberDouble type. */
    double _numberDouble() const {
        return ConstDataView(value()).read<LittleEndian<double>>();
    }

    /** Return int value for this field. MUST be NumberInt type. */
    int _numberInt() const {
        return ConstDataView(value()).read<LittleEndian<int>>();
    }

    /** Return decimal128 value for this field. MUST be NumberDecimal type. */
    Decimal128 _numberDecimal() const {
        uint64_t low = ConstDataView(value()).read<LittleEndian<long long>>();
        uint64_t high = ConstDataView(value() + sizeof(long long)).read<LittleEndian<long long>>();
        return Decimal128(Decimal128::Value({low, high}));
    }

    /** Return long long value for this field. MUST be NumberLong type. */
    long long _numberLong() const {
        return ConstDataView(value()).read<LittleEndian<long long>>();
    }

    /** Retrieve int value for the element safely.  Zero returned if not a number. */
    int numberInt() const;
    /** Retrieve long value for the element safely.  Zero returned if not a number.
     *  Behavior is not defined for double values that are NaNs, or too large/small
     *  to be represented by long longs */
    long long numberLong() const;

    /** Like numberLong() but with well-defined behavior for doubles that
     *  are NaNs, or too large/small to be represented as long longs.
     *  NaNs -> 0
     *  very large doubles -> LLONG_MAX
     *  very small doubles -> LLONG_MIN  */
    long long safeNumberLong() const;

    /** Retrieve decimal value for the element safely. */
    Decimal128 numberDecimal() const;

    /** Retrieve the numeric value of the element.  If not of a numeric type, returns 0.
        Note: casts to double, data loss may occur with large (>52 bit) NumberLong values.
    */
    double numberDouble() const;
    /** Retrieve the numeric value of the element.  If not of a numeric type, returns 0.
        Note: casts to double, data loss may occur with large (>52 bit) NumberLong values.
    */
    double number() const {
        return numberDouble();
    }

    /** Retrieve the object ID stored in the object.
        You must ensure the element is of type jstOID first. */
    mongo::OID __oid() const {
        return OID::from(value());
    }

    /** True if element is null. */
    bool isNull() const {
        return type() == jstNULL;
    }

    /** Size (length) of a std::string element.
        You must assure of type std::string first.
        @return std::string size including terminating null
    */
    int valuestrsize() const {
        return ConstDataView(value()).read<LittleEndian<int>>();
    }

    // for objects the size *includes* the size of the size field
    size_t objsize() const {
        return ConstDataView(value()).read<LittleEndian<uint32_t>>();
    }

    /** Get a string's value.  Also gives you start of the real data for an embedded object.
        You must assure data is of an appropriate type first -- see also valuestrsafe().
    */
    const char* valuestr() const {
        return value() + 4;
    }

    /** Get the std::string value of the element.  If not a std::string returns "". */
    const char* valuestrsafe() const {
        return type() == mongo::String ? valuestr() : "";
    }
    /** Get the std::string value of the element.  If not a std::string returns "". */
    std::string str() const {
        return type() == mongo::String ? std::string(valuestr(), valuestrsize() - 1)
                                       : std::string();
    }

    /**
     * Returns a StringData pointing into this element's data.  Does not validate that the
     * element is actually of type String.
     */
    const StringData valueStringData() const {
        return StringData(valuestr(), valuestrsize() - 1);
    }

    /** Get javascript code of a CodeWScope data element. */
    const char* codeWScopeCode() const {
        massert(16177, "not codeWScope", type() == CodeWScope);
        return value() + 4 + 4;  // two ints precede code (see BSON spec)
    }

    /** Get length of the code part of the CodeWScope object
     *  This INCLUDES the null char at the end */
    int codeWScopeCodeLen() const {
        massert(16178, "not codeWScope", type() == CodeWScope);
        return ConstDataView(value() + 4).read<LittleEndian<int>>();
    }

    /** Get the scope SavedContext of a CodeWScope data element.
     *
     *  This function is DEPRECATED, since it can error if there are
     *  null chars in the codeWScopeCode. However, some existing indexes
     *  may be based on an incorrect ordering derived from this function,
     *  so it may still need to be used in certain cases.
     *   */
    const char* codeWScopeScopeDataUnsafe() const {
        // This can error if there are null chars in the codeWScopeCode
        return codeWScopeCode() + strlen(codeWScopeCode()) + 1;
    }

    /* Get the scope SavedContext of a CodeWScope data element.
     *
     * This is the corrected version of codeWScopeScopeDataUnsafe(),
     * but note that existing uses might rely on the behavior of
     * that function so be careful in choosing which version to use.
     */
    const char* codeWScopeScopeData() const {
        return codeWScopeCode() + codeWScopeCodeLen();
    }

    /** Get the embedded object this element holds. */
    BSONObj embeddedObject() const;

    /* uasserts if not an object */
    BSONObj embeddedObjectUserCheck() const;

    BSONObj codeWScopeObject() const;

    /** Get raw binary data.  Element must be of type BinData. Doesn't handle type 2 specially */
    const char* binData(int& len) const {
        // BinData: <int len> <byte subtype> <byte[len] data>
        verify(type() == BinData);
        len = valuestrsize();
        return value() + 5;
    }
    /** Get binary data.  Element must be of type BinData. Handles type 2 */
    const char* binDataClean(int& len) const {
        // BinData: <int len> <byte subtype> <byte[len] data>
        if (binDataType() != ByteArrayDeprecated) {
            return binData(len);
        } else {
            // Skip extra size
            len = valuestrsize() - 4;
            return value() + 5 + 4;
        }
    }

    BinDataType binDataType() const {
        // BinData: <int len> <byte subtype> <byte[len] data>
        verify(type() == BinData);
        unsigned char c = (value() + 4)[0];
        return (BinDataType)c;
    }

    std::vector<uint8_t> _binDataVector() const {
        if (binDataType() != ByteArrayDeprecated) {
            return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(value()) + 5,
                                        reinterpret_cast<const uint8_t*>(value()) + 5 +
                                            valuestrsize());
        } else {
            // Skip the extra int32 size
            return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(value()) + 4,
                                        reinterpret_cast<const uint8_t*>(value()) + 4 +
                                            valuestrsize() - 4);
        }
    }

    /** Retrieve the regex std::string for a Regex element */
    const char* regex() const {
        verify(type() == RegEx);
        return value();
    }

    /** Retrieve the regex flags (options) for a Regex element */
    const char* regexFlags() const {
        const char* p = regex();
        return p + strlen(p) + 1;
    }

    //
    // Comparison API.
    //
    // BSONElement instances can be compared via a raw bytewise comparison or a logical comparison.
    //
    // Logical comparison can be done either using woCompare() or with operator overloads. Most
    // callers should prefer operator overloads. Note that the operator overloads return a
    // DeferredComparison, which must subsequently be evaluated by a
    // BSONElement::ComparatorInterface. See bsonelement_comparator_interface.h for details.
    //

    /**
     * Compares the raw bytes of the two BSONElements, including the field names. This will treat
     * different types (e.g. integers and doubles) as distinct values, even if they have the same
     * field name and bit pattern in the value portion of the BSON element.
     */
    bool binaryEqual(const BSONElement& rhs) const;

    /**
     * Compares the raw bytes of the two BSONElements, excluding the field names. This will treat
     * different types (e.g integers and doubles) as distinct values, even if they have the same bit
     * pattern in the value portion of the BSON element.
     */
    bool binaryEqualValues(const BSONElement& rhs) const;

    /** Well ordered comparison.
        @return <0: l<r. 0:l==r. >0:l>r
        order by type, field name, and field value.
        If considerFieldName is true, pay attention to the field name.
        If comparator is non-null, it is used for all comparisons between two strings.
    */
    int woCompare(const BSONElement& e,
                  bool considerFieldName = true,
                  const StringData::ComparatorInterface* comparator = nullptr) const;

    DeferredComparison operator<(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kLT, *this, other);
    }

    DeferredComparison operator<=(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kLTE, *this, other);
    }

    DeferredComparison operator>(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kGT, *this, other);
    }

    DeferredComparison operator>=(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kGTE, *this, other);
    }

    DeferredComparison operator==(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kEQ, *this, other);
    }

    DeferredComparison operator!=(const BSONElement& other) const {
        return DeferredComparison(DeferredComparison::Type::kNE, *this, other);
    }

    const char* rawdata() const {
        return data;
    }

    /** Constructs an empty element */
    BSONElement();

    /** True if this element may contain subobjects. */
    bool mayEncapsulate() const {
        switch (type()) {
            case Object:
            case mongo::Array:
            case CodeWScope:
                return true;
            default:
                return false;
        }
    }

    /** True if this element can be a BSONObj */
    bool isABSONObj() const {
        switch (type()) {
            case Object:
            case mongo::Array:
                return true;
            default:
                return false;
        }
    }

    Timestamp timestamp() const {
        if (type() == mongo::Date || type() == bsonTimestamp) {
            return Timestamp(ConstDataView(value()).read<LittleEndian<unsigned long long>>().value);
        }
        return Timestamp();
    }

    const std::array<unsigned char, 16> uuid() const {
        int len = 0;
        const char* data = nullptr;
        if (type() == BinData && binDataType() == BinDataType::newUUID)
            data = binData(len);
        uassert(ErrorCodes::InvalidUUID,
                "uuid must be a 16-byte binary field with UUID (4) subtype",
                len == 16);
        std::array<unsigned char, 16> result;
        memcpy(&result, data, len);
        return result;
    }

    const std::array<unsigned char, 16> md5() const {
        int len = 0;
        const char* data = nullptr;
        if (type() == BinData && binDataType() == BinDataType::MD5Type)
            data = binData(len);
        uassert(40437, "md5 must be a 16-byte binary field with MD5 (5) subtype", len == 16);
        std::array<unsigned char, 16> result;
        memcpy(&result, data, len);
        return result;
    }


    Date_t timestampTime() const {
        unsigned long long t = ConstDataView(value() + 4).read<LittleEndian<unsigned int>>();
        return Date_t::fromMillisSinceEpoch(t * 1000);
    }
    unsigned int timestampInc() const {
        return ConstDataView(value()).read<LittleEndian<unsigned int>>();
    }

    unsigned long long timestampValue() const {
        return ConstDataView(value()).read<LittleEndian<unsigned long long>>();
    }

    const char* dbrefNS() const {
        uassert(10063, "not a dbref", type() == DBRef);
        return value() + 4;
    }

    const mongo::OID dbrefOID() const {
        uassert(10064, "not a dbref", type() == DBRef);
        const char* start = value();
        start += 4 + ConstDataView(start).read<LittleEndian<int>>();
        return mongo::OID::from(start);
    }

    // @param maxLen don't scan more than maxLen bytes
    explicit BSONElement(const char* d, int maxLen) : data(d) {
        if (eoo()) {
            totalSize = 1;
            fieldNameSize_ = 0;
        } else {
            totalSize = -1;
            fieldNameSize_ = -1;
            if (maxLen != -1) {
                size_t size = strnlen(fieldName(), maxLen - 1);
                uassert(10333, "Invalid field name", size < size_t(maxLen - 1));
                fieldNameSize_ = size + 1;
            }
        }
    }

    explicit BSONElement(const char* d) : data(d) {
        fieldNameSize_ = -1;
        totalSize = -1;
        if (eoo()) {
            fieldNameSize_ = 0;
            totalSize = 1;
        }
    }

    struct FieldNameSizeTag {};  // For disambiguation with ctor taking 'maxLen' above.

    /** Construct a BSONElement where you already know the length of the name. The value
     *  passed here includes the null terminator. The data pointed to by 'd' must not
     *  represent an EOO. You may pass -1 to indicate that you don't actually know the
     *  size.
     */
    BSONElement(const char* d, int fieldNameSize, FieldNameSizeTag)
        : data(d),
          fieldNameSize_(fieldNameSize)  // internal size includes null terminator
          ,
          totalSize(-1) {}

    std::string _asCode() const;

    template <typename T>
    bool coerce(T* out) const;

private:
    const char* data;
    mutable int fieldNameSize_;  // cached value

    mutable int totalSize; /* caches the computed size */

    friend class BSONObjIterator;
    friend class BSONObj;
    const BSONElement& chk(int t) const {
        if (t != type()) {
            StringBuilder ss;
            if (eoo())
                ss << "field not found, expected type " << t;
            else
                ss << "wrong type for field (" << fieldName() << ") " << type() << " != " << t;
            msgasserted(13111, ss.str());
        }
        return *this;
    }
    const BSONElement& chk(bool expr) const {
        massert(13118, "unexpected or missing type value in BSON object", expr);
        return *this;
    }
};

inline bool BSONElement::trueValue() const {
    // NOTE Behavior changes must be replicated in Value::coerceToBool().
    switch (type()) {
        case NumberLong:
            return _numberLong() != 0;
        case NumberDouble:
            return _numberDouble() != 0;
        case NumberDecimal:
            return _numberDecimal().isNotEqual(Decimal128(0));
        case NumberInt:
            return _numberInt() != 0;
        case mongo::Bool:
            return boolean();
        case EOO:
        case jstNULL:
        case Undefined:
            return false;
        default:
            return true;
    }
}

/** @return true if element is of a numeric type. */
inline bool BSONElement::isNumber() const {
    switch (type()) {
        case NumberLong:
        case NumberDouble:
        case NumberDecimal:
        case NumberInt:
            return true;
        default:
            return false;
    }
}

inline Decimal128 BSONElement::numberDecimal() const {
    switch (type()) {
        case NumberDouble:
            return Decimal128(_numberDouble());
        case NumberInt:
            return Decimal128(_numberInt());
        case NumberLong:
            return Decimal128(static_cast<int64_t>(_numberLong()));
        case NumberDecimal:
            return _numberDecimal();
        default:
            return Decimal128::kNormalizedZero;
    }
}

inline double BSONElement::numberDouble() const {
    switch (type()) {
        case NumberDouble:
            return _numberDouble();
        case NumberInt:
            return _numberInt();
        case NumberLong:
            return _numberLong();
        case NumberDecimal:
            return _numberDecimal().toDouble();
        default:
            return 0;
    }
}

/** Retrieve int value for the element safely.  Zero returned if not a number. Converted to int if
 * another numeric type. */
inline int BSONElement::numberInt() const {
    switch (type()) {
        case NumberDouble:
            return (int)_numberDouble();
        case NumberInt:
            return _numberInt();
        case NumberLong:
            return (int)_numberLong();
        case NumberDecimal:
            return _numberDecimal().toInt();
        default:
            return 0;
    }
}

/** Retrieve long value for the element safely.  Zero returned if not a number. */
inline long long BSONElement::numberLong() const {
    switch (type()) {
        case NumberDouble:
            return (long long)_numberDouble();
        case NumberInt:
            return _numberInt();
        case NumberLong:
            return _numberLong();
        case NumberDecimal:
            return _numberDecimal().toLong();
        default:
            return 0;
    }
}

/** Like numberLong() but with well-defined behavior for doubles and decimals that
 *  are NaNs, or too large/small to be represented as long longs.
 *  NaNs -> 0
 *  very large values -> LLONG_MAX
 *  very small values -> LLONG_MIN  */
inline long long BSONElement::safeNumberLong() const {
    switch (type()) {
        case NumberDouble: {
            double d = numberDouble();
            if (std::isnan(d)) {
                return 0;
            }
            if (d > (double)std::numeric_limits<long long>::max()) {
                return std::numeric_limits<long long>::max();
            }
            if (d < std::numeric_limits<long long>::min()) {
                return std::numeric_limits<long long>::min();
            }
            return numberLong();
        }
        case NumberDecimal: {
            Decimal128 d = numberDecimal();
            if (d.isNaN()) {
                return 0;
            }
            if (d.isGreater(Decimal128(std::numeric_limits<int64_t>::max()))) {
                return static_cast<long long>(std::numeric_limits<int64_t>::max());
            }
            if (d.isLess(Decimal128(std::numeric_limits<int64_t>::min()))) {
                return static_cast<long long>(std::numeric_limits<int64_t>::min());
            }
            return numberLong();
        }
        default:
            return numberLong();
    }
}

inline BSONElement::BSONElement() {
    // This needs to be 2 elements because we check the strlen of data + 1 and GCC sees that as
    // accessing beyond the end of a constant string, even though we always check whether the
    // element is an eoo.
    static const char kEooElement[2] = {'\0', '\0'};
    data = kEooElement;
    fieldNameSize_ = 0;
    totalSize = 1;
}
}
