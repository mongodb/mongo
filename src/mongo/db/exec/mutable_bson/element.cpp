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

#include "mongo/db/exec/mutable_bson/element.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/exec/mutable_bson/document.h"

#include <string_view>

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

Status Element::appendDouble(std::string_view fieldName, double value) {
    return pushBack(getDocument().makeElementDouble(fieldName, value));
}

Status Element::appendString(std::string_view fieldName, std::string_view value) {
    return pushBack(getDocument().makeElementString(fieldName, value));
}

Status Element::appendObject(std::string_view fieldName, const BSONObj& value) {
    return pushBack(getDocument().makeElementObject(fieldName, value));
}

Status Element::appendArray(std::string_view fieldName, const BSONObj& value) {
    return pushBack(getDocument().makeElementArray(fieldName, value));
}

Status Element::appendBinary(std::string_view fieldName,
                             uint32_t len,
                             mongo::BinDataType binType,
                             const void* data) {
    return pushBack(getDocument().makeElementBinary(fieldName, len, binType, data));
}

Status Element::appendUndefined(std::string_view fieldName) {
    return pushBack(getDocument().makeElementUndefined(fieldName));
}

Status Element::appendOID(std::string_view fieldName, const OID value) {
    return pushBack(getDocument().makeElementOID(fieldName, value));
}

Status Element::appendBool(std::string_view fieldName, bool value) {
    return pushBack(getDocument().makeElementBool(fieldName, value));
}

Status Element::appendDate(std::string_view fieldName, Date_t value) {
    return pushBack(getDocument().makeElementDate(fieldName, value));
}

Status Element::appendNull(std::string_view fieldName) {
    return pushBack(getDocument().makeElementNull(fieldName));
}

Status Element::appendRegex(std::string_view fieldName,
                            std::string_view re,
                            std::string_view flags) {
    return pushBack(getDocument().makeElementRegex(fieldName, re, flags));
}

Status Element::appendDBRef(std::string_view fieldName, std::string_view ns, const OID oid) {
    return pushBack(getDocument().makeElementDBRef(fieldName, ns, oid));
}

Status Element::appendCode(std::string_view fieldName, std::string_view value) {
    return pushBack(getDocument().makeElementCode(fieldName, value));
}

Status Element::appendSymbol(std::string_view fieldName, std::string_view value) {
    return pushBack(getDocument().makeElementSymbol(fieldName, value));
}

Status Element::appendCodeWithScope(std::string_view fieldName,
                                    std::string_view code,
                                    const BSONObj& scope) {
    return pushBack(getDocument().makeElementCodeWithScope(fieldName, code, scope));
}

Status Element::appendInt(std::string_view fieldName, int32_t value) {
    return pushBack(getDocument().makeElementInt(fieldName, value));
}

Status Element::appendTimestamp(std::string_view fieldName, Timestamp value) {
    return pushBack(getDocument().makeElementTimestamp(fieldName, value));
}

Status Element::appendDecimal(std::string_view fieldName, Decimal128 value) {
    return pushBack(getDocument().makeElementDecimal(fieldName, value));
}

Status Element::appendLong(std::string_view fieldName, int64_t value) {
    return pushBack(getDocument().makeElementLong(fieldName, value));
}

Status Element::appendMinKey(std::string_view fieldName) {
    return pushBack(getDocument().makeElementMinKey(fieldName));
}

Status Element::appendMaxKey(std::string_view fieldName) {
    return pushBack(getDocument().makeElementMaxKey(fieldName));
}

Status Element::appendElement(const BSONElement& value) {
    return pushBack(getDocument().makeElement(value));
}

Status Element::appendSafeNum(std::string_view fieldName, SafeNum value) {
    return pushBack(getDocument().makeElementSafeNum(fieldName, value));
}

std::string Element::toString() const {
    if (!ok())
        return "INVALID-MUTABLE-ELEMENT";

    if (hasValue())
        return getValue().toString();

    const BSONType type = getType();

    // The only types that sometimes don't have a value are Object and Array nodes.
    dassert((type == BSONType::object) || (type == BSONType::array));

    if (type == BSONType::object) {
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

}  // namespace mutablebson
}  // namespace mongo
