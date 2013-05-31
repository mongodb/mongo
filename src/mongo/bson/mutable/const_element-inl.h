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

    inline ConstElement::ConstElement(const Element& basis)
        : _basis(basis) {}

    inline ConstElement ConstElement::leftChild() const {
        return _basis.leftChild();
    }

    inline ConstElement ConstElement::rightChild() const {
        return _basis.rightChild();
    }

    inline bool ConstElement::hasChildren() const {
        return _basis.hasChildren();
    }

    inline ConstElement ConstElement::leftSibling() const {
        return _basis.leftSibling();
    }

    inline ConstElement ConstElement::rightSibling() const {
        return _basis.rightSibling();
    }

    inline ConstElement ConstElement::parent() const {
        return _basis.parent();
    }

    inline ConstElement ConstElement::operator[](size_t n) const {
        return _basis[n];
    }

    inline ConstElement ConstElement::operator[](const StringData& name) const {
        return _basis[name];
    }

    inline bool ConstElement::hasValue() const {
        return _basis.hasValue();
    }

    inline const BSONElement ConstElement::getValue() const {
        return _basis.getValue();
    }

    inline double ConstElement::getValueDouble() const {
        return _basis.getValueDouble();
    }

    inline StringData ConstElement::getValueString() const {
        return _basis.getValueString();
    }

    inline BSONObj ConstElement::getValueObject() const {
        return _basis.getValueObject();
    }

    inline BSONArray ConstElement::getValueArray() const {
        return _basis.getValueArray();
    }

    inline bool ConstElement::isValueUndefined() const {
        return _basis.isValueUndefined();
    }

    inline OID ConstElement::getValueOID() const {
        return _basis.getValueOID();
    }

    inline bool ConstElement::getValueBool() const {
        return _basis.getValueBool();
    }

    inline Date_t ConstElement::getValueDate() const {
        return _basis.getValueDate();
    }

    inline bool ConstElement::isValueNull() const {
        return _basis.isValueNull();
    }

    inline StringData ConstElement::getValueSymbol() const {
        return _basis.getValueSymbol();
    }

    inline int32_t ConstElement::getValueInt() const {
        return _basis.getValueInt();
    }

    inline OpTime ConstElement::getValueTimestamp() const {
        return _basis.getValueTimestamp();
    }

    inline int64_t ConstElement::getValueLong() const {
        return _basis.getValueLong();
    }

    inline bool ConstElement::isValueMinKey() const {
        return _basis.isValueMinKey();
    }

    inline bool ConstElement::isValueMaxKey() const {
        return _basis.isValueMaxKey();
    }

    inline SafeNum ConstElement::getValueSafeNum() const {
        return _basis.getValueSafeNum();
    }

    inline int ConstElement::compareWithElement(const ConstElement& other,
                                                bool considerFieldName) const {
        return _basis.compareWithElement(other, considerFieldName);
    }

    inline int ConstElement::compareWithBSONElement(const BSONElement& other,
                                                    bool considerFieldName) const {
        return _basis.compareWithBSONElement(other, considerFieldName);
    }

    inline int ConstElement::compareWithBSONObj(const BSONObj& other,
                                                bool considerFieldName) const {
        return _basis.compareWithBSONObj(other, considerFieldName);
    }

    inline void ConstElement::writeTo(BSONObjBuilder* builder) const {
        return _basis.writeTo(builder);
    }

    inline void ConstElement::writeArrayTo(BSONArrayBuilder* builder) const {
        return _basis.writeArrayTo(builder);
    }

    inline bool ConstElement::ok() const {
        return _basis.ok();
    }

    inline const Document& ConstElement::getDocument() const {
        return _basis.getDocument();
    }

    inline BSONType ConstElement::getType() const {
        return _basis.getType();
    }

    inline bool ConstElement::isType(BSONType type) const {
        return _basis.isType(type);
    }

    inline StringData ConstElement::getFieldName() const {
        return _basis.getFieldName();
    }

    inline Element::RepIdx ConstElement::getIdx() const {
        return _basis.getIdx();
    }

    inline std::string ConstElement::toString() const {
        return _basis.toString();
    }

    template<typename Builder>
    inline void ConstElement::writeElement(Builder* builder, const StringData* fieldName) const {
        return _basis.writeElement(builder, fieldName);
    }

    inline bool operator==(const ConstElement& l, const ConstElement& r) {
        return l._basis == r._basis;
    }

    inline bool operator!=(const ConstElement& l, const ConstElement& r) {
        return !(l == r);
    }

    inline bool operator==(const Element& l, const ConstElement& r) {
        return ConstElement(l) == r;
    }

    inline bool operator!=(const Element& l, const ConstElement& r) {
        return !(l == r);
    }

    inline bool operator==(const ConstElement& l, const Element& r) {
        return l == ConstElement(r);
    }

    inline bool operator!=(const ConstElement& l, const Element& r) {
        return !(l == r);
    }


} // namespace mutablebson
} // namespace mongo
