/* Copyright 2013 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/element.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"

namespace mongo {
namespace mutablebson {

    // Many of the methods of Element are actually implemented in document.cpp, since they need
    // access to the firewalled implementation of Document.

    Status Element::pushFront(Element e) {
        return addChild(e, true);
    }

    Status Element::pushBack(Element e) {
        return addChild(e, false);
    }

    Status Element::popFront() {
        Element left = leftChild();
        if (!left.ok())
            return Status(ErrorCodes::EmptyArrayOperation, "popFront on empty");
        return left.remove();
    }

    Status Element::popBack() {
        Element right = rightChild();
        if (!right.ok())
            return Status(ErrorCodes::EmptyArrayOperation, "popBack on empty");
        return right.remove();
    }

    Status Element::appendDouble(const StringData& fieldName, double value) {
        return pushBack(getDocument().makeElementDouble(fieldName, value));
    }

    Status Element::appendString(const StringData& fieldName, const StringData& value) {
        return pushBack(getDocument().makeElementString(fieldName, value));
    }

    Status Element::appendObject(const StringData& fieldName, const BSONObj& value) {
        return pushBack(getDocument().makeElementObject(fieldName, value));
    }

    Status Element::appendArray(const StringData& fieldName, const BSONObj& value) {
        return pushBack(getDocument().makeElementArray(fieldName, value));
    }

    Status Element::appendBinary(const StringData& fieldName,
                                 uint32_t len, mongo::BinDataType binType,
                                 const void* data) {
        return pushBack(getDocument().makeElementBinary(fieldName, len, binType, data));
    }

    Status Element::appendUndefined(const StringData& fieldName) {
        return pushBack(getDocument().makeElementUndefined(fieldName));
    }

    Status Element::appendOID(const StringData& fieldName, const OID value) {
        return pushBack(getDocument().makeElementOID(fieldName, value));
    }

    Status Element::appendBool(const StringData& fieldName, bool value) {
        return pushBack(getDocument().makeElementBool(fieldName, value));
    }

    Status Element::appendDate(const StringData& fieldName, Date_t value) {
        return pushBack(getDocument().makeElementDate(fieldName, value));
    }

    Status Element::appendNull(const StringData& fieldName) {
        return pushBack(getDocument().makeElementNull(fieldName));
    }

    Status Element::appendRegex(const StringData& fieldName,
                                const StringData& re, const StringData& flags) {
        return pushBack(getDocument().makeElementRegex(fieldName, re, flags));
    }

    Status Element::appendDBRef(const StringData& fieldName,
                                const StringData& ns, const OID oid) {
        return pushBack(getDocument().makeElementDBRef(fieldName, ns, oid));
    }

    Status Element::appendCode(const StringData& fieldName, const StringData& value) {
        return pushBack(getDocument().makeElementCode(fieldName, value));
    }

    Status Element::appendSymbol(const StringData& fieldName, const StringData& value) {
        return pushBack(getDocument().makeElementSymbol(fieldName, value));
    }

    Status Element::appendCodeWithScope(const StringData& fieldName,
                                        const StringData& code, const BSONObj& scope) {
        return pushBack(getDocument().makeElementCodeWithScope(fieldName, code, scope));
    }

    Status Element::appendInt(const StringData& fieldName, int32_t value) {
        return pushBack(getDocument().makeElementInt(fieldName, value));
    }

    Status Element::appendTimestamp(const StringData& fieldName, OpTime value) {
        return pushBack(getDocument().makeElementTimestamp(fieldName, value));
    }

    Status Element::appendLong(const StringData& fieldName, int64_t value) {
        return pushBack(getDocument().makeElementLong(fieldName, value));
    }

    Status Element::appendMinKey(const StringData& fieldName) {
        return pushBack(getDocument().makeElementMinKey(fieldName));
    }

    Status Element::appendMaxKey(const StringData& fieldName) {
        return pushBack(getDocument().makeElementMaxKey(fieldName));
    }

    Status Element::appendElement(const BSONElement& value) {
        return pushBack(getDocument().makeElement(value));
    }

    Status Element::appendSafeNum(const StringData& fieldName, SafeNum value) {
        return pushBack(getDocument().makeElementSafeNum(fieldName, value));
    }

    std::string Element::toString() const {
        if (!ok())
            return "INVALID-MUTABLE-ELEMENT";

        if (hasValue())
            return getValue().toString();

        const BSONType type = getType();

        // The only types that sometimes don't have a value are Object and Array nodes.
        dassert((type == mongo::Object) || (type == mongo::Array));

        if (type == mongo::Object) {
            BSONObjBuilder builder;
            writeTo(&builder);
            BSONObj obj = builder.obj();
            return obj.firstElement().toString();
        } else {
            // It must be an array.
            BSONObjBuilder builder;
            BSONArrayBuilder arrayBuilder(builder.subarrayStart(getFieldName()));
            writeArrayTo(&arrayBuilder);
            arrayBuilder.done();
            BSONObj obj = builder.obj();
            return obj.firstElement().toString();
        }
    }

} // namespace mutablebson
} // namespace mongo
