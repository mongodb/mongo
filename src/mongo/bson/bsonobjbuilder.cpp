/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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
 *    must comply with the GNU Affero General Public License in all respects for
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

void BSONObjBuilder::appendMinForType(StringData fieldName, int t) {
    switch (t) {
        // Shared canonical types
        case NumberInt:
        case NumberDouble:
        case NumberLong:
        case NumberDecimal:
            append(fieldName, std::numeric_limits<double>::quiet_NaN());
            return;
        case Symbol:
        case String:
            append(fieldName, "");
            return;
        case Date:
            // min varies with V0 and V1 indexes, so we go one type lower.
            appendBool(fieldName, true);
            // appendDate( fieldName , numeric_limits<long long>::min() );
            return;
        case bsonTimestamp:
            appendTimestamp(fieldName, 0);
            return;
        case Undefined:  // shared with EOO
            appendUndefined(fieldName);
            return;

        // Separate canonical types
        case MinKey:
            appendMinKey(fieldName);
            return;
        case MaxKey:
            appendMaxKey(fieldName);
            return;
        case jstOID: {
            OID o;
            appendOID(fieldName, &o);
            return;
        }
        case Bool:
            appendBool(fieldName, false);
            return;
        case jstNULL:
            appendNull(fieldName);
            return;
        case Object:
            append(fieldName, BSONObj());
            return;
        case Array:
            appendArray(fieldName, BSONObj());
            return;
        case BinData:
            appendBinData(fieldName, 0, BinDataGeneral, (const char*)0);
            return;
        case RegEx:
            appendRegex(fieldName, "");
            return;
        case DBRef: {
            OID o;
            appendDBRef(fieldName, "", o);
            return;
        }
        case Code:
            appendCode(fieldName, "");
            return;
        case CodeWScope:
            appendCodeWScope(fieldName, "", BSONObj());
            return;
    };
    log() << "type not supported for appendMinElementForType: " << t;
    uassert(10061, "type not supported for appendMinElementForType", false);
}

void BSONObjBuilder::appendMaxForType(StringData fieldName, int t) {
    switch (t) {
        // Shared canonical types
        case NumberInt:
        case NumberDouble:
        case NumberLong:
        case NumberDecimal:
            append(fieldName, std::numeric_limits<double>::infinity());
            return;
        case Symbol:
        case String:
            appendMinForType(fieldName, Object);
            return;
        case Date:
            appendDate(fieldName,
                       Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::max()));
            return;
        case bsonTimestamp:
            append(fieldName, Timestamp::max());
            return;
        case Undefined:  // shared with EOO
            appendUndefined(fieldName);
            return;

        // Separate canonical types
        case MinKey:
            appendMinKey(fieldName);
            return;
        case MaxKey:
            appendMaxKey(fieldName);
            return;
        case jstOID: {
            OID o = OID::max();
            appendOID(fieldName, &o);
            return;
        }
        case Bool:
            appendBool(fieldName, true);
            return;
        case jstNULL:
            appendNull(fieldName);
            return;
        case Object:
            appendMinForType(fieldName, Array);
            return;
        case Array:
            appendMinForType(fieldName, BinData);
            return;
        case BinData:
            appendMinForType(fieldName, jstOID);
            return;
        case RegEx:
            appendMinForType(fieldName, DBRef);
            return;
        case DBRef:
            appendMinForType(fieldName, Code);
            return;
        case Code:
            appendMinForType(fieldName, CodeWScope);
            return;
        case CodeWScope:
            // This upper bound may change if a new bson type is added.
            appendMinForType(fieldName, MaxKey);
            return;
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
BSONObjBuilder& BSONObjBuilder::appendElements(BSONObj x) {
    if (!x.isEmpty())
        _b.appendBuf(x.objdata() + 4,   // skip over leading length
                     x.objsize() - 5);  // ignore leading length and trailing \0
    return *this;
}

/* add all the fields from the object specified to this object if they don't exist */
BSONObjBuilder& BSONObjBuilder::appendElementsUnique(BSONObj x) {
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

void BSONObjBuilder::appendKeys(const BSONObj& keyPattern, const BSONObj& values) {
    BSONObjIterator i(keyPattern);
    BSONObjIterator j(values);

    while (i.more() && j.more()) {
        appendAs(j.next(), i.next().fieldName());
    }

    verify(!i.more());
    verify(!j.more());
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

}  // namespace mongo
