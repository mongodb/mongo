// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
