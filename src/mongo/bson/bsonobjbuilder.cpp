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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

template <class Derived, class B>
Derived& BSONObjBuilderBase<Derived, B>::appendMinForType(StringData fieldName, int t) {
    switch (t) {
        // Shared canonical types
        case NumberInt:
        case NumberDouble:
        case NumberLong:
        case NumberDecimal:
            append(fieldName, std::numeric_limits<double>::quiet_NaN());
            return static_cast<Derived&>(*this);
        case Symbol:
        case String:
            append(fieldName, "");
            return static_cast<Derived&>(*this);
        case Date:
            appendDate(fieldName, Date_t::min());
            return static_cast<Derived&>(*this);
        case bsonTimestamp:
            appendTimestamp(fieldName, 0);
            return static_cast<Derived&>(*this);
        case Undefined:  // shared with EOO
            appendUndefined(fieldName);
            return static_cast<Derived&>(*this);

        // Separate canonical types
        case MinKey:
            appendMinKey(fieldName);
            return static_cast<Derived&>(*this);
        case MaxKey:
            appendMaxKey(fieldName);
            return static_cast<Derived&>(*this);
        case jstOID: {
            OID o;
            appendOID(fieldName, &o);
            return static_cast<Derived&>(*this);
        }
        case Bool:
            appendBool(fieldName, false);
            return static_cast<Derived&>(*this);
        case jstNULL:
            appendNull(fieldName);
            return static_cast<Derived&>(*this);
        case Object:
            append(fieldName, BSONObj());
            return static_cast<Derived&>(*this);
        case Array:
            appendArray(fieldName, BSONObj());
            return static_cast<Derived&>(*this);
        case BinData:
            appendBinData(fieldName, 0, BinDataGeneral, (const char*)nullptr);
            return static_cast<Derived&>(*this);
        case RegEx:
            appendRegex(fieldName, "");
            return static_cast<Derived&>(*this);
        case DBRef: {
            OID o;
            appendDBRef(fieldName, "", o);
            return static_cast<Derived&>(*this);
        }
        case Code:
            appendCode(fieldName, "");
            return static_cast<Derived&>(*this);
        case CodeWScope:
            appendCodeWScope(fieldName, "", BSONObj());
            return static_cast<Derived&>(*this);
    };
    LOGV2(20101, "type not supported for appendMinElementForType: {t}", "t"_attr = t);
    uassert(10061, "type not supported for appendMinElementForType", false);
}

template <class Derived, class B>
Derived& BSONObjBuilderBase<Derived, B>::appendMaxForType(StringData fieldName, int t) {
    switch (t) {
        // Shared canonical types
        case NumberInt:
        case NumberDouble:
        case NumberLong:
        case NumberDecimal:
            append(fieldName, std::numeric_limits<double>::infinity());
            return static_cast<Derived&>(*this);
        case Symbol:
        case String:
            appendMinForType(fieldName, Object);
            return static_cast<Derived&>(*this);
        case Date:
            appendDate(fieldName, Date_t::max());
            return static_cast<Derived&>(*this);
        case bsonTimestamp:
            append(fieldName, Timestamp::max());
            return static_cast<Derived&>(*this);
        case Undefined:  // shared with EOO
            appendUndefined(fieldName);
            return static_cast<Derived&>(*this);

        // Separate canonical types
        case MinKey:
            appendMinKey(fieldName);
            return static_cast<Derived&>(*this);
        case MaxKey:
            appendMaxKey(fieldName);
            return static_cast<Derived&>(*this);
        case jstOID: {
            OID o = OID::max();
            appendOID(fieldName, &o);
            return static_cast<Derived&>(*this);
        }
        case Bool:
            appendBool(fieldName, true);
            return static_cast<Derived&>(*this);
        case jstNULL:
            appendNull(fieldName);
            return static_cast<Derived&>(*this);
        case Object:
            appendMinForType(fieldName, Array);
            return static_cast<Derived&>(*this);
        case Array:
            appendMinForType(fieldName, BinData);
            return static_cast<Derived&>(*this);
        case BinData:
            appendMinForType(fieldName, jstOID);
            return static_cast<Derived&>(*this);
        case RegEx:
            appendMinForType(fieldName, DBRef);
            return static_cast<Derived&>(*this);
        case DBRef:
            appendMinForType(fieldName, Code);
            return static_cast<Derived&>(*this);
        case Code:
            appendMinForType(fieldName, CodeWScope);
            return static_cast<Derived&>(*this);
        case CodeWScope:
            // This upper bound may change if a new bson type is added.
            appendMinForType(fieldName, MaxKey);
            return static_cast<Derived&>(*this);
    }
    LOGV2(20102, "type not supported for appendMaxElementForType: {t}", "t"_attr = t);
    uassert(14853, "type not supported for appendMaxElementForType", false);
}

template <class Derived, class B>
Derived& BSONObjBuilderBase<Derived, B>::appendDate(StringData fieldName, Date_t dt) {
    _b.appendNum((char)Date);
    _b.appendStr(fieldName);
    _b.appendNum(dt.toMillisSinceEpoch());
    return static_cast<Derived&>(*this);
}

/* add all the fields from the object specified to this object */
template <class Derived, class B>
Derived& BSONObjBuilderBase<Derived, B>::appendElements(const BSONObj& x) {
    if (!x.isEmpty())
        _b.appendBuf(x.objdata() + 4,   // skip over leading length
                     x.objsize() - 5);  // ignore leading length and trailing \0
    return static_cast<Derived&>(*this);
}

/* add all the fields from the object specified to this object if they don't exist */
template <class Derived, class B>
Derived& BSONObjBuilderBase<Derived, B>::appendElementsUnique(const BSONObj& x) {
    std::set<std::string> have;
    {
        BSONObjIterator i = iterator();
        while (i.more())
            have.insert(i.next().fieldName());
    }

    BSONObjIterator it(x);
    while (it.more()) {
        BSONElement e = it.next();
        if (have.count(e.fieldName()))
            continue;
        append(e);
    }
    return static_cast<Derived&>(*this);
}

template <class Derived, class B>
BSONObjIterator BSONObjBuilderBase<Derived, B>::iterator() const {
    const char* s = _b.buf() + _offset;
    const char* e = _b.buf() + _b.len();
    return BSONObjIterator(s, e);
}

template <class Derived, class B>
bool BSONObjBuilderBase<Derived, B>::hasField(StringData name) const {
    BSONObjIterator i = iterator();
    while (i.more())
        if (name == i.next().fieldName())
            return true;
    return false;
}

// Explicit instantiations
template class BSONObjBuilderBase<BSONObjBuilder, BufBuilder>;
template class BSONObjBuilderBase<UniqueBSONObjBuilder, UniqueBufBuilder>;
template class BSONArrayBuilderBase<BSONArrayBuilder, BSONObjBuilder>;
template class BSONArrayBuilderBase<UniqueBSONArrayBuilder, UniqueBSONObjBuilder>;

}  // namespace mongo
