/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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

    Element Element::operator[](size_t n) const {
        return getNthChild(*this, n);
    }

    Element Element::operator[](const StringData& name) const {
        return findFirstChildNamed(*this, name);
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
