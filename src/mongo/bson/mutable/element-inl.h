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

#pragma once

namespace mongo {
namespace mutablebson {

    inline double Element::getValueDouble() const {
        dassert(hasValue() && isType(mongo::NumberDouble));
        return getValue()._numberDouble();
    }

    inline StringData Element::getValueString() const {
        dassert(hasValue() && isType(mongo::String));
        return getValueStringOrSymbol();
    }

    inline BSONObj Element::getValueObject() const {
        dassert(hasValue() && isType(mongo::Object));
        return getValue().Obj();
    }

    inline BSONArray Element::getValueArray() const {
        dassert(hasValue() && isType(mongo::Array));
        return BSONArray(getValue().Obj());
    }

    inline bool Element::isValueUndefined() const {
        return isType(mongo::Undefined);
    }

    inline OID Element::getValueOID() const {
        dassert(hasValue() && isType(mongo::jstOID));
        return getValue().__oid();
    }

    inline bool Element::getValueBool() const {
        dassert(hasValue() && isType(mongo::Bool));
        return getValue().boolean();
    }

    inline Date_t Element::getValueDate() const {
        dassert(hasValue() && isType(mongo::Date));
        return getValue().date();
    }

    inline bool Element::isValueNull() const {
        return isType(mongo::jstNULL);
    }

    inline StringData Element::getValueSymbol() const {
        dassert(hasValue() && isType(mongo::Symbol));
        return getValueStringOrSymbol();
    }

    inline int32_t Element::getValueInt() const {
        dassert(hasValue() && isType(mongo::NumberInt));
        return getValue()._numberInt();
    }

    inline OpTime Element::getValueTimestamp() const {
        dassert(hasValue() && isType(mongo::Timestamp));
        return getValue()._opTime();
    }

    inline int64_t Element::getValueLong() const {
        dassert(hasValue() && isType(mongo::NumberLong));
        return getValue()._numberLong();
    }

    inline bool Element::isValueMinKey() const {
        return isType(mongo::MinKey);
    }

    inline bool Element::isValueMaxKey() const {
        return isType(mongo::MaxKey);
    }

    inline bool Element::ok() const {
        dassert(_doc != NULL);
        return _repIdx <= kMaxRepIdx;
    }

    inline Document& Element::getDocument() {
        return *_doc;
    }

    inline const Document& Element::getDocument() const {
        return *_doc;
    }

    inline bool Element::isType(BSONType type) const {
        return (getType() == type);
    }

    inline Element::RepIdx Element::getIdx() const {
        return _repIdx;
    }

    inline Element::Element(Document* doc, RepIdx repIdx)
        : _doc(doc)
        , _repIdx(repIdx) {
        dassert(_doc != NULL);
    }

    inline StringData Element::getValueStringOrSymbol() const {
        const BSONElement value = getValue();
        const char* str = value.valuestr();
        const size_t size = value.valuestrsize() - 1;
        return StringData(str, size);
    }

    inline bool operator==(const Element& l, const Element& r) {
        return (l._doc == r._doc) && (l._repIdx == r._repIdx);
    }

    inline bool operator!=(const Element& l, const Element& r) {
        return !(l == r);
    }


} // namespace mutablebson
} // namespace mongo
