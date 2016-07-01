/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/value.h"

#include <boost/functional/hash.hpp>
#include <cmath>
#include <limits>

#include "mongo/base/compare_numbers.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/hex.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
using namespace mongoutils;
using boost::intrusive_ptr;
using std::min;
using std::numeric_limits;
using std::ostream;
using std::string;
using std::stringstream;
using std::vector;

void ValueStorage::verifyRefCountingIfShould() const {
    switch (type) {
        case MinKey:
        case MaxKey:
        case jstOID:
        case Date:
        case bsonTimestamp:
        case EOO:
        case jstNULL:
        case Undefined:
        case Bool:
        case NumberInt:
        case NumberLong:
        case NumberDouble:
            // the above types never reference external data
            verify(!refCounter);
            break;

        case String:
        case RegEx:
        case Code:
        case Symbol:
            // the above types reference data when not using short-string optimization
            verify(refCounter == !shortStr);
            break;

        case NumberDecimal:
        case BinData:  // TODO this should probably support short-string optimization
        case Array:    // TODO this should probably support empty-is-NULL optimization
        case DBRef:
        case CodeWScope:
            // the above types always reference external data.
            verify(refCounter);
            verify(bool(genericRCPtr));
            break;

        case Object:
            // Objects either hold a NULL ptr or should be ref-counting
            verify(refCounter == bool(genericRCPtr));
            break;
    }
}

void ValueStorage::putString(StringData s) {
    // Note: this also stores data portion of BinData
    const size_t sizeNoNUL = s.size();
    if (sizeNoNUL <= sizeof(shortStrStorage)) {
        shortStr = true;
        shortStrSize = s.size();
        s.copyTo(shortStrStorage, false);  // no NUL

        // All memory is zeroed before this is called, so we know that
        // the nulTerminator field will definitely contain a NUL byte.
        dassert(((sizeNoNUL < sizeof(shortStrStorage)) && (shortStrStorage[sizeNoNUL] == '\0')) ||
                (((shortStrStorage + sizeNoNUL) == &nulTerminator) && (nulTerminator == '\0')));
    } else {
        putRefCountable(RCString::create(s));
    }
}

void ValueStorage::putDocument(const Document& d) {
    putRefCountable(d._storage);
}

void ValueStorage::putVector(const RCVector* vec) {
    fassert(16485, vec);
    putRefCountable(vec);
}

void ValueStorage::putRegEx(const BSONRegEx& re) {
    const size_t patternLen = re.pattern.size();
    const size_t flagsLen = re.flags.size();
    const size_t totalLen = patternLen + 1 /*middle NUL*/ + flagsLen;

    // Need to copy since putString doesn't support scatter-gather.
    std::unique_ptr<char[]> buf(new char[totalLen]);
    re.pattern.copyTo(buf.get(), true);
    re.flags.copyTo(buf.get() + patternLen + 1, false);  // no NUL
    putString(StringData(buf.get(), totalLen));
}

Document ValueStorage::getDocument() const {
    if (!genericRCPtr)
        return Document();

    dassert(typeid(*genericRCPtr) == typeid(const DocumentStorage));
    const DocumentStorage* documentPtr = static_cast<const DocumentStorage*>(genericRCPtr);
    return Document(documentPtr);
}

// not in header because document is fwd declared
Value::Value(const BSONObj& obj) : _storage(Object, Document(obj)) {}

Value::Value(const BSONElement& elem) : _storage(elem.type()) {
    switch (elem.type()) {
        // These are all type-only, no data
        case EOO:
        case MinKey:
        case MaxKey:
        case Undefined:
        case jstNULL:
            break;

        case NumberDouble:
            _storage.doubleValue = elem.Double();
            break;

        case Code:
        case Symbol:
        case String:
            _storage.putString(elem.valueStringData());
            break;

        case Object: {
            _storage.putDocument(Document(elem.embeddedObject()));
            break;
        }

        case Array: {
            intrusive_ptr<RCVector> vec(new RCVector);
            BSONForEach(sub, elem.embeddedObject()) {
                vec->vec.push_back(Value(sub));
            }
            _storage.putVector(vec.get());
            break;
        }

        case jstOID:
            static_assert(sizeof(_storage.oid) == OID::kOIDSize,
                          "sizeof(_storage.oid) == OID::kOIDSize");
            memcpy(_storage.oid, elem.OID().view().view(), OID::kOIDSize);
            break;

        case Bool:
            _storage.boolValue = elem.boolean();
            break;

        case Date:
            _storage.dateValue = elem.date().toMillisSinceEpoch();
            break;

        case RegEx: {
            _storage.putRegEx(BSONRegEx(elem.regex(), elem.regexFlags()));
            break;
        }

        case NumberInt:
            _storage.intValue = elem.numberInt();
            break;

        case bsonTimestamp:
            _storage.timestampValue = elem.timestamp().asULL();
            break;

        case NumberLong:
            _storage.longValue = elem.numberLong();
            break;

        case NumberDecimal:
            _storage.putDecimal(elem.numberDecimal());
            break;

        case CodeWScope: {
            StringData code(elem.codeWScopeCode(), elem.codeWScopeCodeLen() - 1);
            _storage.putCodeWScope(BSONCodeWScope(code, elem.codeWScopeObject()));
            break;
        }

        case BinData: {
            int len;
            const char* data = elem.binData(len);
            _storage.putBinData(BSONBinData(data, len, elem.binDataType()));
            break;
        }

        case DBRef:
            _storage.putDBRef(BSONDBRef(elem.dbrefNS(), elem.dbrefOID()));
            break;
    }
}

Value::Value(const BSONArray& arr) : _storage(Array) {
    intrusive_ptr<RCVector> vec(new RCVector);
    BSONForEach(sub, arr) {
        vec->vec.push_back(Value(sub));
    }
    _storage.putVector(vec.get());
}

Value::Value(const vector<BSONObj>& arr) : _storage(Array) {
    intrusive_ptr<RCVector> vec(new RCVector);
    vec->vec.reserve(arr.size());
    for (auto&& obj : arr) {
        vec->vec.push_back(Value(obj));
    }
    _storage.putVector(vec.get());
}

Value Value::createIntOrLong(long long longValue) {
    int intValue = longValue;
    if (intValue != longValue) {
        // it is too large to be an int and should remain a long
        return Value(longValue);
    }

    // should be an int since all arguments were int and it fits
    return Value(intValue);
}

Decimal128 Value::getDecimal() const {
    BSONType type = getType();
    if (type == NumberInt)
        return Decimal128(static_cast<int32_t>(_storage.intValue));
    if (type == NumberLong)
        return Decimal128(static_cast<int64_t>(_storage.longValue));
    if (type == NumberDouble)
        return Decimal128(_storage.doubleValue);
    invariant(type == NumberDecimal);
    return _storage.getDecimal();
}

double Value::getDouble() const {
    BSONType type = getType();
    if (type == NumberInt)
        return _storage.intValue;
    if (type == NumberLong)
        return static_cast<double>(_storage.longValue);
    if (type == NumberDecimal)
        return _storage.getDecimal().toDouble();

    verify(type == NumberDouble);
    return _storage.doubleValue;
}

Document Value::getDocument() const {
    verify(getType() == Object);
    return _storage.getDocument();
}

Value Value::operator[](size_t index) const {
    if (getType() != Array || index >= getArrayLength())
        return Value();

    return getArray()[index];
}

Value Value::operator[](StringData name) const {
    if (getType() != Object)
        return Value();

    return getDocument()[name];
}

BSONObjBuilder& operator<<(BSONObjBuilderValueStream& builder, const Value& val) {
    switch (val.getType()) {
        case EOO:
            return builder.builder();  // nothing appended
        case MinKey:
            return builder << MINKEY;
        case MaxKey:
            return builder << MAXKEY;
        case jstNULL:
            return builder << BSONNULL;
        case Undefined:
            return builder << BSONUndefined;
        case jstOID:
            return builder << val.getOid();
        case NumberInt:
            return builder << val.getInt();
        case NumberLong:
            return builder << val.getLong();
        case NumberDouble:
            return builder << val.getDouble();
        case NumberDecimal:
            return builder << val.getDecimal();
        case String:
            return builder << val.getStringData();
        case Bool:
            return builder << val.getBool();
        case Date:
            return builder << Date_t::fromMillisSinceEpoch(val.getDate());
        case bsonTimestamp:
            return builder << val.getTimestamp();
        case Object:
            return builder << val.getDocument();
        case Symbol:
            return builder << BSONSymbol(val.getStringData());
        case Code:
            return builder << BSONCode(val.getStringData());
        case RegEx:
            return builder << BSONRegEx(val.getRegex(), val.getRegexFlags());

        case DBRef:
            return builder << BSONDBRef(val._storage.getDBRef()->ns, val._storage.getDBRef()->oid);

        case BinData:
            return builder << BSONBinData(val.getStringData().rawData(),  // looking for void*
                                          val.getStringData().size(),
                                          val._storage.binDataType());

        case CodeWScope:
            return builder << BSONCodeWScope(val._storage.getCodeWScope()->code,
                                             val._storage.getCodeWScope()->scope);

        case Array: {
            const vector<Value>& array = val.getArray();
            const size_t n = array.size();
            BSONArrayBuilder arrayBuilder(builder.subarrayStart());
            for (size_t i = 0; i < n; i++) {
                array[i].addToBsonArray(&arrayBuilder);
            }
            arrayBuilder.doneFast();
            return builder.builder();
        }
    }
    verify(false);
}

void Value::addToBsonObj(BSONObjBuilder* pBuilder, StringData fieldName) const {
    *pBuilder << fieldName << *this;
}

void Value::addToBsonArray(BSONArrayBuilder* pBuilder) const {
    if (!missing()) {  // don't want to increment builder's counter
        *pBuilder << *this;
    }
}

bool Value::coerceToBool() const {
    // TODO Unify the implementation with BSONElement::trueValue().
    switch (getType()) {
        case CodeWScope:
        case MinKey:
        case DBRef:
        case Code:
        case MaxKey:
        case String:
        case Object:
        case Array:
        case BinData:
        case jstOID:
        case Date:
        case RegEx:
        case Symbol:
        case bsonTimestamp:
            return true;

        case EOO:
        case jstNULL:
        case Undefined:
            return false;

        case Bool:
            return _storage.boolValue;
        case NumberInt:
            return _storage.intValue;
        case NumberLong:
            return _storage.longValue;
        case NumberDouble:
            return _storage.doubleValue;
        case NumberDecimal:
            return !_storage.getDecimal().isZero();
    }
    verify(false);
}

int Value::coerceToInt() const {
    switch (getType()) {
        case NumberInt:
            return _storage.intValue;

        case NumberLong:
            return static_cast<int>(_storage.longValue);

        case NumberDouble:
            return static_cast<int>(_storage.doubleValue);

        case NumberDecimal:
            return (_storage.getDecimal()).toInt();

        default:
            uassert(16003,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to int",
                    false);
    }  // switch(getType())
}

long long Value::coerceToLong() const {
    switch (getType()) {
        case NumberLong:
            return _storage.longValue;

        case NumberInt:
            return static_cast<long long>(_storage.intValue);

        case NumberDouble:
            return static_cast<long long>(_storage.doubleValue);

        case NumberDecimal:
            return (_storage.getDecimal()).toLong();

        default:
            uassert(16004,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to long",
                    false);
    }  // switch(getType())
}

double Value::coerceToDouble() const {
    switch (getType()) {
        case NumberDouble:
            return _storage.doubleValue;

        case NumberInt:
            return static_cast<double>(_storage.intValue);

        case NumberLong:
            return static_cast<double>(_storage.longValue);

        case NumberDecimal:
            return (_storage.getDecimal()).toDouble();

        default:
            uassert(16005,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to double",
                    false);
    }  // switch(getType())
}

Decimal128 Value::coerceToDecimal() const {
    switch (getType()) {
        case NumberDecimal:
            return _storage.getDecimal();

        case NumberInt:
            return Decimal128(static_cast<int32_t>(_storage.intValue));

        case NumberLong:
            return Decimal128(static_cast<int64_t>(_storage.longValue));

        case NumberDouble:
            return Decimal128(_storage.doubleValue);

        default:
            uassert(16008,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to decimal",
                    false);
    }  // switch(getType())
}

long long Value::coerceToDate() const {
    switch (getType()) {
        case Date:
            return getDate();

        case bsonTimestamp:
            return getTimestamp().getSecs() * 1000LL;

        default:
            uassert(16006,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to Date",
                    false);
    }  // switch(getType())
}

time_t Value::coerceToTimeT() const {
    long long millis = coerceToDate();
    if (millis < 0) {
        // We want the division below to truncate toward -inf rather than 0
        // eg Dec 31, 1969 23:59:58.001 should be -2 seconds rather than -1
        // This is needed to get the correct values from coerceToTM
        if (-1999 / 1000 != -2) {  // this is implementation defined
            millis -= 1000 - 1;
        }
    }
    const long long seconds = millis / 1000;

    uassert(16421,
            "Can't handle date values outside of time_t range",
            seconds >= std::numeric_limits<time_t>::min() &&
                seconds <= std::numeric_limits<time_t>::max());

    return static_cast<time_t>(seconds);
}
tm Value::coerceToTm() const {
    // See implementation in Date_t.
    // Can't reuse that here because it doesn't support times before 1970
    time_t dtime = coerceToTimeT();
    tm out;

#if defined(_WIN32)  // Both the argument order and the return values differ
    bool itWorked = gmtime_s(&out, &dtime) == 0;
#else
    bool itWorked = gmtime_r(&dtime, &out) != NULL;
#endif

    if (!itWorked) {
        if (dtime < 0) {
            // Windows docs say it doesn't support these, but empirically it seems to work
            uasserted(16422, "gmtime failed - your system doesn't support dates before 1970");
        } else {
            uasserted(16423, str::stream() << "gmtime failed to convert time_t of " << dtime);
        }
    }

    return out;
}

static string tmToISODateString(const tm& time) {
    char buf[128];
    size_t len = strftime(buf, 128, "%Y-%m-%dT%H:%M:%S", &time);
    verify(len > 0);
    verify(len < 128);
    return buf;
}

string Value::coerceToString() const {
    switch (getType()) {
        case NumberDouble:
            return str::stream() << _storage.doubleValue;

        case NumberInt:
            return str::stream() << _storage.intValue;

        case NumberLong:
            return str::stream() << _storage.longValue;

        case NumberDecimal:
            return str::stream() << _storage.getDecimal().toString();

        case Code:
        case Symbol:
        case String:
            return getStringData().toString();

        case bsonTimestamp:
            return getTimestamp().toStringPretty();

        case Date:
            return tmToISODateString(coerceToTm());

        case EOO:
        case jstNULL:
        case Undefined:
            return "";

        default:
            uassert(16007,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to String",
                    false);
    }  // switch(getType())
}

Timestamp Value::coerceToTimestamp() const {
    switch (getType()) {
        case bsonTimestamp:
            return getTimestamp();

        default:
            uassert(16378,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to timestamp",
                    false);
    }  // switch(getType())
}

// Helper function for Value::compare.
// Better than l-r for cases where difference > MAX_INT
template <typename T>
inline static int cmp(const T& left, const T& right) {
    if (left < right) {
        return -1;
    } else if (left == right) {
        return 0;
    } else {
        dassert(left > right);
        return 1;
    }
}

int Value::compare(const Value& rL,
                   const Value& rR,
                   const StringData::ComparatorInterface* stringComparator) {
    // Note, this function needs to behave identically to BSON's compareElementValues().
    // Additionally, any changes here must be replicated in hash_combine().
    BSONType lType = rL.getType();
    BSONType rType = rR.getType();

    int ret = lType == rType ? 0  // fast-path common case
                             : cmp(canonicalizeBSONType(lType), canonicalizeBSONType(rType));

    if (ret)
        return ret;

    switch (lType) {
        // Order of types is the same as in compareElementValues() to make it easier to verify

        // These are valueless types
        case EOO:
        case Undefined:
        case jstNULL:
        case MaxKey:
        case MinKey:
            return ret;

        case Bool:
            return rL.getBool() - rR.getBool();

        case bsonTimestamp:  // unsigned
            return cmp(rL._storage.timestampValue, rR._storage.timestampValue);

        case Date:  // signed
            return cmp(rL._storage.dateValue, rR._storage.dateValue);

        // Numbers should compare by equivalence even if different types

        case NumberDecimal: {
            switch (rType) {
                case NumberDecimal:
                    return compareDecimals(rL._storage.getDecimal(), rR._storage.getDecimal());
                case NumberInt:
                    return compareDecimalToInt(rL._storage.getDecimal(), rR._storage.intValue);
                case NumberLong:
                    return compareDecimalToLong(rL._storage.getDecimal(), rR._storage.longValue);
                case NumberDouble:
                    return compareDecimalToDouble(rL._storage.getDecimal(),
                                                  rR._storage.doubleValue);
                default:
                    invariant(false);
            }
        }

        case NumberInt: {
            // All types can precisely represent all NumberInts, so it is safe to simply convert to
            // whatever rhs's type is.
            switch (rType) {
                case NumberInt:
                    return compareInts(rL._storage.intValue, rR._storage.intValue);
                case NumberLong:
                    return compareLongs(rL._storage.intValue, rR._storage.longValue);
                case NumberDouble:
                    return compareDoubles(rL._storage.intValue, rR._storage.doubleValue);
                case NumberDecimal:
                    return compareIntToDecimal(rL._storage.intValue, rR._storage.getDecimal());
                default:
                    invariant(false);
            }
        }

        case NumberLong: {
            switch (rType) {
                case NumberLong:
                    return compareLongs(rL._storage.longValue, rR._storage.longValue);
                case NumberInt:
                    return compareLongs(rL._storage.longValue, rR._storage.intValue);
                case NumberDouble:
                    return compareLongToDouble(rL._storage.longValue, rR._storage.doubleValue);
                case NumberDecimal:
                    return compareLongToDecimal(rL._storage.longValue, rR._storage.getDecimal());
                default:
                    invariant(false);
            }
        }

        case NumberDouble: {
            switch (rType) {
                case NumberDouble:
                    return compareDoubles(rL._storage.doubleValue, rR._storage.doubleValue);
                case NumberInt:
                    return compareDoubles(rL._storage.doubleValue, rR._storage.intValue);
                case NumberLong:
                    return compareDoubleToLong(rL._storage.doubleValue, rR._storage.longValue);
                case NumberDecimal:
                    return compareDoubleToDecimal(rL._storage.doubleValue,
                                                  rR._storage.getDecimal());
                default:
                    invariant(false);
            }
        }

        case jstOID:
            return memcmp(rL._storage.oid, rR._storage.oid, OID::kOIDSize);

        case String: {
            if (!stringComparator) {
                return rL.getStringData().compare(rR.getStringData());
            }

            return stringComparator->compare(rL.getStringData(), rR.getStringData());
        }

        case Code:
        case Symbol:
            return rL.getStringData().compare(rR.getStringData());

        case Object:
            return Document::compare(rL.getDocument(), rR.getDocument(), stringComparator);

        case Array: {
            const vector<Value>& lArr = rL.getArray();
            const vector<Value>& rArr = rR.getArray();

            const size_t elems = std::min(lArr.size(), rArr.size());
            for (size_t i = 0; i < elems; i++) {
                // compare the two corresponding elements
                ret = Value::compare(lArr[i], rArr[i], stringComparator);
                if (ret)
                    return ret;  // values are unequal
            }

            // if we get here we are either equal or one is prefix of the other
            return cmp(lArr.size(), rArr.size());
        }

        case DBRef: {
            intrusive_ptr<const RCDBRef> l = rL._storage.getDBRef();
            intrusive_ptr<const RCDBRef> r = rR._storage.getDBRef();
            ret = cmp(l->ns.size(), r->ns.size());
            if (ret)
                return ret;

            return l->oid.compare(r->oid);
        }

        case BinData: {
            ret = cmp(rL.getStringData().size(), rR.getStringData().size());
            if (ret)
                return ret;

            // Need to compare as an unsigned char rather than enum since BSON uses memcmp
            ret = cmp(rL._storage.binSubType, rR._storage.binSubType);
            if (ret)
                return ret;

            return rL.getStringData().compare(rR.getStringData());
        }

        case RegEx:  // same as String in this impl but keeping order same as compareElementValues
            return rL.getStringData().compare(rR.getStringData());

        case CodeWScope: {
            intrusive_ptr<const RCCodeWScope> l = rL._storage.getCodeWScope();
            intrusive_ptr<const RCCodeWScope> r = rR._storage.getCodeWScope();

            ret = l->code.compare(r->code);
            if (ret)
                return ret;

            return l->scope.woCompare(r->scope);
        }
    }
    verify(false);
}

void Value::hash_combine(size_t& seed) const {
    BSONType type = getType();

    boost::hash_combine(seed, canonicalizeBSONType(type));

    switch (type) {
        // Order of types is the same as in Value::compare() and compareElementValues().

        // These are valueless types
        case EOO:
        case Undefined:
        case jstNULL:
        case MaxKey:
        case MinKey:
            return;

        case Bool:
            boost::hash_combine(seed, getBool());
            break;

        case bsonTimestamp:
        case Date:
            static_assert(sizeof(_storage.dateValue) == sizeof(_storage.timestampValue),
                          "sizeof(_storage.dateValue) == sizeof(_storage.timestampValue)");
            boost::hash_combine(seed, _storage.dateValue);
            break;

        case mongo::NumberDecimal: {
            const Decimal128 dcml = getDecimal();
            if (dcml.toAbs().isGreater(Decimal128(std::numeric_limits<double>::max(),
                                                  Decimal128::kRoundTo34Digits,
                                                  Decimal128::kRoundTowardZero)) &&
                !dcml.isInfinite() && !dcml.isNaN()) {
                // Normalize our decimal to force equivalent decimals
                // in the same cohort to hash to the same value
                Decimal128 dcmlNorm(dcml.normalize());
                boost::hash_combine(seed, dcmlNorm.getValue().low64);
                boost::hash_combine(seed, dcmlNorm.getValue().high64);
                break;
            }
            // Else, fall through and convert the decimal to a double and hash.
            // At this point the decimal fits into the range of doubles, is infinity, or is NaN,
            // which doubles have a cheaper representation for.
        }
        // This converts all numbers to doubles, which ignores the low-order bits of
        // NumberLongs > 2**53 and precise decimal numbers without double representations,
        // but that is ok since the hash will still be the same for equal numbers and is
        // still likely to be different for different numbers. (Note: this issue only
        // applies for decimals when they are inside of the valid double range. See
        // the above case.)
        // SERVER-16851
        case NumberDouble:
        case NumberLong:
        case NumberInt: {
            const double dbl = getDouble();
            if (std::isnan(dbl)) {
                boost::hash_combine(seed, numeric_limits<double>::quiet_NaN());
            } else {
                boost::hash_combine(seed, dbl);
            }
            break;
        }

        case jstOID:
            getOid().hash_combine(seed);
            break;

        case Code:
        case Symbol:
        case String: {
            StringData sd = getStringData();
            MurmurHash3_x86_32(sd.rawData(), sd.size(), seed, &seed);
            break;
        }

        case Object:
            getDocument().hash_combine(seed);
            break;

        case Array: {
            const vector<Value>& vec = getArray();
            for (size_t i = 0; i < vec.size(); i++)
                vec[i].hash_combine(seed);
            break;
        }

        case DBRef:
            boost::hash_combine(seed, _storage.getDBRef()->ns);
            _storage.getDBRef()->oid.hash_combine(seed);
            break;


        case BinData: {
            StringData sd = getStringData();
            MurmurHash3_x86_32(sd.rawData(), sd.size(), seed, &seed);
            boost::hash_combine(seed, _storage.binDataType());
            break;
        }

        case RegEx: {
            StringData sd = getStringData();
            MurmurHash3_x86_32(sd.rawData(), sd.size(), seed, &seed);
            break;
        }

        case CodeWScope: {
            intrusive_ptr<const RCCodeWScope> cws = _storage.getCodeWScope();
            boost::hash_combine(seed, StringData::Hasher()(cws->code));
            boost::hash_combine(seed, BSONObj::Hasher()(cws->scope));
            break;
        }
    }
}

BSONType Value::getWidestNumeric(BSONType lType, BSONType rType) {
    if (lType == NumberDouble) {
        switch (rType) {
            case NumberDecimal:
                return NumberDecimal;

            case NumberDouble:
            case NumberLong:
            case NumberInt:
                return NumberDouble;

            default:
                break;
        }
    } else if (lType == NumberLong) {
        switch (rType) {
            case NumberDecimal:
                return NumberDecimal;

            case NumberDouble:
                return NumberDouble;

            case NumberLong:
            case NumberInt:
                return NumberLong;

            default:
                break;
        }
    } else if (lType == NumberInt) {
        switch (rType) {
            case NumberDecimal:
                return NumberDecimal;

            case NumberDouble:
                return NumberDouble;

            case NumberLong:
                return NumberLong;

            case NumberInt:
                return NumberInt;

            default:
                break;
        }
    } else if (lType == NumberDecimal) {
        switch (rType) {
            case NumberInt:
            case NumberLong:
            case NumberDouble:
            case NumberDecimal:
                return NumberDecimal;

            default:
                break;
        }
    }

    // Reachable, but callers must subsequently err out in this case.
    return Undefined;
}

bool Value::integral() const {
    switch (getType()) {
        case NumberInt:
            return true;
        case NumberLong:
            return (_storage.longValue <= numeric_limits<int>::max() &&
                    _storage.longValue >= numeric_limits<int>::min());
        case NumberDouble:
            return (_storage.doubleValue <= numeric_limits<int>::max() &&
                    _storage.doubleValue >= numeric_limits<int>::min() &&
                    _storage.doubleValue == static_cast<int>(_storage.doubleValue));
        case NumberDecimal: {
            // If we are able to convert the decimal to an int32_t without an rounding errors,
            // then it is integral.
            uint32_t signalingFlags = Decimal128::kNoFlag;
            (void)_storage.getDecimal().toInt(&signalingFlags);
            return signalingFlags == Decimal128::kNoFlag;
        }
        default:
            return false;
    }
}

size_t Value::getApproximateSize() const {
    switch (getType()) {
        case Code:
        case RegEx:
        case Symbol:
        case BinData:
        case String:
            return sizeof(Value) + (_storage.shortStr
                                        ? 0  // string stored inline, so no extra mem usage
                                        : sizeof(RCString) + _storage.getString().size());

        case Object:
            return sizeof(Value) + getDocument().getApproximateSize();

        case Array: {
            size_t size = sizeof(Value);
            size += sizeof(RCVector);
            const size_t n = getArray().size();
            for (size_t i = 0; i < n; ++i) {
                size += getArray()[i].getApproximateSize();
            }
            return size;
        }

        case CodeWScope:
            return sizeof(Value) + sizeof(RCCodeWScope) + _storage.getCodeWScope()->code.size() +
                _storage.getCodeWScope()->scope.objsize();

        case DBRef:
            return sizeof(Value) + sizeof(RCDBRef) + _storage.getDBRef()->ns.size();

        case NumberDecimal:
            return sizeof(Value) + sizeof(RCDecimal);

        // These types are always contained within the Value
        case EOO:
        case MinKey:
        case MaxKey:
        case NumberDouble:
        case jstOID:
        case Bool:
        case Date:
        case NumberInt:
        case bsonTimestamp:
        case NumberLong:
        case jstNULL:
        case Undefined:
            return sizeof(Value);
    }
    verify(false);
}

string Value::toString() const {
    // TODO use StringBuilder when operator << is ready
    stringstream out;
    out << *this;
    return out.str();
}

ostream& operator<<(ostream& out, const Value& val) {
    switch (val.getType()) {
        case EOO:
            return out << "MISSING";
        case MinKey:
            return out << "MinKey";
        case MaxKey:
            return out << "MaxKey";
        case jstOID:
            return out << val.getOid();
        case String:
            return out << '"' << val.getString() << '"';
        case RegEx:
            return out << '/' << val.getRegex() << '/' << val.getRegexFlags();
        case Symbol:
            return out << "Symbol(\"" << val.getSymbol() << "\")";
        case Code:
            return out << "Code(\"" << val.getCode() << "\")";
        case Bool:
            return out << (val.getBool() ? "true" : "false");
        case NumberDecimal:
            return out << val.getDecimal().toString();
        case NumberDouble:
            return out << val.getDouble();
        case NumberLong:
            return out << val.getLong();
        case NumberInt:
            return out << val.getInt();
        case jstNULL:
            return out << "null";
        case Undefined:
            return out << "undefined";
        case Date:
            return out << tmToISODateString(val.coerceToTm());
        case bsonTimestamp:
            return out << val.getTimestamp().toString();
        case Object:
            return out << val.getDocument().toString();
        case Array: {
            out << "[";
            const size_t n = val.getArray().size();
            for (size_t i = 0; i < n; i++) {
                if (i)
                    out << ", ";
                out << val.getArray()[i];
            }
            out << "]";
            return out;
        }

        case CodeWScope:
            return out << "CodeWScope(\"" << val._storage.getCodeWScope()->code << "\", "
                       << val._storage.getCodeWScope()->scope << ')';

        case BinData:
            return out << "BinData(" << val._storage.binDataType() << ", \""
                       << toHex(val._storage.getString().rawData(), val._storage.getString().size())
                       << "\")";

        case DBRef:
            return out << "DBRef(\"" << val._storage.getDBRef()->ns << "\", "
                       << val._storage.getDBRef()->oid << ')';
    }

    // Not in default case to trigger better warning if a case is missing
    verify(false);
}

void Value::serializeForSorter(BufBuilder& buf) const {
    buf.appendChar(getType());
    switch (getType()) {
        // type-only types
        case EOO:
        case MinKey:
        case MaxKey:
        case jstNULL:
        case Undefined:
            break;

        // simple types
        case jstOID:
            buf.appendStruct(_storage.oid);
            break;
        case NumberInt:
            buf.appendNum(_storage.intValue);
            break;
        case NumberLong:
            buf.appendNum(_storage.longValue);
            break;
        case NumberDouble:
            buf.appendNum(_storage.doubleValue);
            break;
        case NumberDecimal:
            buf.appendNum(_storage.getDecimal());
            break;
        case Bool:
            buf.appendChar(_storage.boolValue);
            break;
        case Date:
            buf.appendNum(_storage.dateValue);
            break;
        case bsonTimestamp:
            buf.appendStruct(getTimestamp());
            break;

        // types that are like strings
        case String:
        case Symbol:
        case Code: {
            StringData str = getStringData();
            buf.appendNum(int(str.size()));
            buf.appendStr(str, /*NUL byte*/ false);
            break;
        }

        case BinData: {
            StringData str = getStringData();
            buf.appendChar(_storage.binDataType());
            buf.appendNum(int(str.size()));
            buf.appendStr(str, /*NUL byte*/ false);
            break;
        }

        case RegEx:
            buf.appendStr(getRegex(), /*NUL byte*/ true);
            buf.appendStr(getRegexFlags(), /*NUL byte*/ true);
            break;

        case Object:
            getDocument().serializeForSorter(buf);
            break;

        case DBRef:
            buf.appendStruct(_storage.getDBRef()->oid);
            buf.appendStr(_storage.getDBRef()->ns, /*NUL byte*/ true);
            break;

        case CodeWScope: {
            intrusive_ptr<const RCCodeWScope> cws = _storage.getCodeWScope();
            buf.appendNum(int(cws->code.size()));
            buf.appendStr(cws->code, /*NUL byte*/ false);
            cws->scope.serializeForSorter(buf);
            break;
        }

        case Array: {
            const vector<Value>& array = getArray();
            const int numElems = array.size();
            buf.appendNum(numElems);
            for (int i = 0; i < numElems; i++)
                array[i].serializeForSorter(buf);
            break;
        }
    }
}

Value Value::deserializeForSorter(BufReader& buf, const SorterDeserializeSettings& settings) {
    const BSONType type = BSONType(buf.read<signed char>());  // need sign extension for MinKey
    switch (type) {
        // type-only types
        case EOO:
        case MinKey:
        case MaxKey:
        case jstNULL:
        case Undefined:
            return Value(ValueStorage(type));

        // simple types
        case jstOID:
            return Value(OID::from(buf.skip(OID::kOIDSize)));
        case NumberInt:
            return Value(buf.read<LittleEndian<int>>().value);
        case NumberLong:
            return Value(buf.read<LittleEndian<long long>>().value);
        case NumberDouble:
            return Value(buf.read<LittleEndian<double>>().value);
        case NumberDecimal:
            return Value(Decimal128(buf.read<LittleEndian<Decimal128::Value>>().value));
        case Bool:
            return Value(bool(buf.read<char>()));
        case Date:
            return Value(Date_t::fromMillisSinceEpoch(buf.read<LittleEndian<long long>>().value));
        case bsonTimestamp:
            return Value(buf.read<Timestamp>());

        // types that are like strings
        case String:
        case Symbol:
        case Code: {
            int size = buf.read<LittleEndian<int>>();
            const char* str = static_cast<const char*>(buf.skip(size));
            return Value(ValueStorage(type, StringData(str, size)));
        }

        case BinData: {
            BinDataType bdt = BinDataType(buf.read<unsigned char>());
            int size = buf.read<LittleEndian<int>>();
            const void* data = buf.skip(size);
            return Value(BSONBinData(data, size, bdt));
        }

        case RegEx: {
            StringData regex = buf.readCStr();
            StringData flags = buf.readCStr();
            return Value(BSONRegEx(regex, flags));
        }

        case Object:
            return Value(
                Document::deserializeForSorter(buf, Document::SorterDeserializeSettings()));

        case DBRef: {
            OID oid = OID::from(buf.skip(OID::kOIDSize));
            StringData ns = buf.readCStr();
            return Value(BSONDBRef(ns, oid));
        }

        case CodeWScope: {
            int size = buf.read<LittleEndian<int>>();
            const char* str = static_cast<const char*>(buf.skip(size));
            BSONObj bson = BSONObj::deserializeForSorter(buf, BSONObj::SorterDeserializeSettings());
            return Value(BSONCodeWScope(StringData(str, size), bson));
        }

        case Array: {
            const int numElems = buf.read<LittleEndian<int>>();
            vector<Value> array;
            array.reserve(numElems);
            for (int i = 0; i < numElems; i++)
                array.push_back(deserializeForSorter(buf, settings));
            return Value(std::move(array));
        }
    }
    verify(false);
}
}
