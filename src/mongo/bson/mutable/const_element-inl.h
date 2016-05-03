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

#pragma once

namespace mongo {
namespace mutablebson {

inline ConstElement::ConstElement(const Element& basis) : _basis(basis) {}

inline ConstElement ConstElement::leftChild() const {
    return _basis.leftChild();
}

inline ConstElement ConstElement::rightChild() const {
    return _basis.rightChild();
}

inline bool ConstElement::hasChildren() const {
    return _basis.hasChildren();
}

inline ConstElement ConstElement::leftSibling(size_t distance) const {
    return _basis.leftSibling(distance);
}

inline ConstElement ConstElement::rightSibling(size_t distance) const {
    return _basis.rightSibling(distance);
}

inline ConstElement ConstElement::parent() const {
    return _basis.parent();
}

inline ConstElement ConstElement::findNthChild(size_t n) const {
    return _basis.findNthChild(n);
}

inline ConstElement ConstElement::operator[](size_t n) const {
    return _basis[n];
}

inline ConstElement ConstElement::findFirstChildNamed(StringData name) const {
    return _basis.findFirstChildNamed(name);
}

inline ConstElement ConstElement::operator[](StringData name) const {
    return _basis[name];
}

inline ConstElement ConstElement::findElementNamed(StringData name) const {
    return _basis.findElementNamed(name);
}

inline size_t ConstElement::countSiblingsLeft() const {
    return _basis.countSiblingsLeft();
}

inline size_t ConstElement::countSiblingsRight() const {
    return _basis.countSiblingsRight();
}

inline size_t ConstElement::countChildren() const {
    return _basis.countChildren();
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

inline Timestamp ConstElement::getValueTimestamp() const {
    return _basis.getValueTimestamp();
}

inline int64_t ConstElement::getValueLong() const {
    return _basis.getValueLong();
}

inline Decimal128 ConstElement::getValueDecimal() const {
    return _basis.getValueDecimal();
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

inline int ConstElement::compareWithElement(
    const ConstElement& other,
    bool considerFieldName,
    const StringData::ComparatorInterface* comparator) const {
    return _basis.compareWithElement(other, considerFieldName, comparator);
}

inline int ConstElement::compareWithBSONElement(
    const BSONElement& other,
    bool considerFieldName,
    const StringData::ComparatorInterface* comparator) const {
    return _basis.compareWithBSONElement(other, considerFieldName, comparator);
}

inline int ConstElement::compareWithBSONObj(
    const BSONObj& other,
    bool considerFieldName,
    const StringData::ComparatorInterface* comparator) const {
    return _basis.compareWithBSONObj(other, considerFieldName, comparator);
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


}  // namespace mutablebson
}  // namespace mongo
