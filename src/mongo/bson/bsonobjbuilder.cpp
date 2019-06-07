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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/db/jsobj.h"

#include <boost/lexical_cast.hpp>

#include "mongo/bson/timestamp.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

BSONObjBuilder& BSONObjBuilder::appendMinForType(StringData fieldName, int t) {
    switch (t) {
        // Shared canonical types
        case NumberInt:
        case NumberDouble:
        case NumberLong:
        case NumberDecimal:
            append(fieldName, std::numeric_limits<double>::quiet_NaN());
            return *this;
        case Symbol:
        case String:
            append(fieldName, "");
            return *this;
        case Date:
            // min varies with V0 and V1 indexes, so we go one type lower.
            appendBool(fieldName, true);
            // appendDate( fieldName , numeric_limits<long long>::min() );
            return *this;
        case bsonTimestamp:
            appendTimestamp(fieldName, 0);
            return *this;
        case Undefined:  // shared with EOO
            appendUndefined(fieldName);
            return *this;

        // Separate canonical types
        case MinKey:
            appendMinKey(fieldName);
            return *this;
        case MaxKey:
            appendMaxKey(fieldName);
            return *this;
        case jstOID: {
            OID o;
            appendOID(fieldName, &o);
            return *this;
        }
        case Bool:
            appendBool(fieldName, false);
            return *this;
        case jstNULL:
            appendNull(fieldName);
            return *this;
        case Object:
            append(fieldName, BSONObj());
            return *this;
        case Array:
            appendArray(fieldName, BSONObj());
            return *this;
        case BinData:
            appendBinData(fieldName, 0, BinDataGeneral, (const char*)0);
            return *this;
        case RegEx:
            appendRegex(fieldName, "");
            return *this;
        case DBRef: {
            OID o;
            appendDBRef(fieldName, "", o);
            return *this;
        }
        case Code:
            appendCode(fieldName, "");
            return *this;
        case CodeWScope:
            appendCodeWScope(fieldName, "", BSONObj());
            return *this;
    };
    log() << "type not supported for appendMinElementForType: " << t;
    uassert(10061, "type not supported for appendMinElementForType", false);
}

BSONObjBuilder& BSONObjBuilder::appendMaxForType(StringData fieldName, int t) {
    switch (t) {
        // Shared canonical types
        case NumberInt:
        case NumberDouble:
        case NumberLong:
        case NumberDecimal:
            append(fieldName, std::numeric_limits<double>::infinity());
            return *this;
        case Symbol:
        case String:
            appendMinForType(fieldName, Object);
            return *this;
        case Date:
            appendDate(fieldName,
                       Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::max()));
            return *this;
        case bsonTimestamp:
            append(fieldName, Timestamp::max());
            return *this;
        case Undefined:  // shared with EOO
            appendUndefined(fieldName);
            return *this;

        // Separate canonical types
        case MinKey:
            appendMinKey(fieldName);
            return *this;
        case MaxKey:
            appendMaxKey(fieldName);
            return *this;
        case jstOID: {
            OID o = OID::max();
            appendOID(fieldName, &o);
            return *this;
        }
        case Bool:
            appendBool(fieldName, true);
            return *this;
        case jstNULL:
            appendNull(fieldName);
            return *this;
        case Object:
            appendMinForType(fieldName, Array);
            return *this;
        case Array:
            appendMinForType(fieldName, BinData);
            return *this;
        case BinData:
            appendMinForType(fieldName, jstOID);
            return *this;
        case RegEx:
            appendMinForType(fieldName, DBRef);
            return *this;
        case DBRef:
            appendMinForType(fieldName, Code);
            return *this;
        case Code:
            appendMinForType(fieldName, CodeWScope);
            return *this;
        case CodeWScope:
            // This upper bound may change if a new bson type is added.
            appendMinForType(fieldName, MaxKey);
            return *this;
    }
    log() << "type not supported for appendMaxElementForType: " << t;
    uassert(14853, "type not supported for appendMaxElementForType", false);
}

BSONObjBuilder& BSONObjBuilder::appendDate(StringData fieldName, Date_t dt) {
    _b.appendNum((char)Date);
    _b.appendStr(fieldName);
    _b.appendNum(dt.toMillisSinceEpoch());
    return *this;
}

/* add all the fields from the object specified to this object */
BSONObjBuilder& BSONObjBuilder::appendElements(const BSONObj& x) {
    if (!x.isEmpty())
        _b.appendBuf(x.objdata() + 4,   // skip over leading length
                     x.objsize() - 5);  // ignore leading length and trailing \0
    return *this;
}

/* add all the fields from the object specified to this object if they don't exist */
BSONObjBuilder& BSONObjBuilder::appendElementsUnique(const BSONObj& x) {
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
    return *this;
}

BSONObjIterator BSONObjBuilder::iterator() const {
    const char* s = _b.buf() + _offset;
    const char* e = _b.buf() + _b.len();
    return BSONObjIterator(s, e);
}

bool BSONObjBuilder::hasField(StringData name) const {
    BSONObjIterator i = iterator();
    while (i.more())
        if (name == i.next().fieldName())
            return true;
    return false;
}

BSONObjBuilder::~BSONObjBuilder() {
    // If 'done' has not already been called, and we have a reference to an owning
    // BufBuilder but do not own it ourselves, then we must call _done to write in the
    // length. Otherwise, we own this memory and its lifetime ends with us, therefore
    // we can elide the write.
    if (!_doneCalled && _b.buf() && _buf.getSize() == 0) {
        _done();
    }
}


const string BSONObjBuilder::numStrs[] = {
    "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",  "10", "11", "12", "13", "14",
    "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29",
    "30", "31", "32", "33", "34", "35", "36", "37", "38", "39", "40", "41", "42", "43", "44",
    "45", "46", "47", "48", "49", "50", "51", "52", "53", "54", "55", "56", "57", "58", "59",
    "60", "61", "62", "63", "64", "65", "66", "67", "68", "69", "70", "71", "72", "73", "74",
    "75", "76", "77", "78", "79", "80", "81", "82", "83", "84", "85", "86", "87", "88", "89",
    "90", "91", "92", "93", "94", "95", "96", "97", "98", "99",
};

// This is to ensure that BSONObjBuilder doesn't try to use numStrs before the strings have
// been constructed I've tested just making numStrs a char[][], but the overhead of
// constructing the strings each time was too high numStrsReady will be 0 until after
// numStrs is initialized because it is a static variable
bool BSONObjBuilder::numStrsReady = (numStrs[0].size() > 0);

template <typename Alloc>
void _BufBuilder<Alloc>::grow_reallocate(int minSize) {
    if (minSize > BufferMaxSize) {
        std::stringstream ss;
        ss << "BufBuilder attempted to grow() to " << minSize << " bytes, past the 64MB limit.";
        msgasserted(13548, ss.str().c_str());
    }

    int a = 64;
    while (a < minSize)
        a = a * 2;

    _buf.realloc(a);
    size = a;
}

template class _BufBuilder<SharedBufferAllocator>;
template class _BufBuilder<StackAllocator>;
template class StringBuilderImpl<SharedBufferAllocator>;
template class StringBuilderImpl<StackAllocator>;

}  // namespace mongo
