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

#pragma once

#include "mongo/bson/mutable/api.h"
#include "mongo/bson/mutable/element.h"

namespace mongo {
namespace mutablebson {

/** For an overview of mutable BSON, please see the file document.h in this directory. */

/** ConstElement recapitulates all of the const methods of Element, but cannot be converted
 *  to an Element. This makes it safe to return as a value from a constant Document, since
 *  none of Element's non-const methods may be called on ConstElement or any values it
 *  yields. If you think of Element like an STL 'iterator', then ConstElement is a
 *  'const_iterator'.
 *
 *  For details on the API methods of ConstElement, please see the method comments for the
 *  analagous Element methods in the file element.h (in this same directory).
 *
 *  All calls on ConstElement are simply forwarded to the underlying Element.
 */
class MONGO_MUTABLE_BSON_API ConstElement {
public:
    // This one argument constructor is intentionally not explicit, since we want to be
    // able to pass Elements to functions taking ConstElements without complaint.
    inline ConstElement(const Element& basis);

    inline ConstElement leftChild() const;
    inline ConstElement rightChild() const;
    inline bool hasChildren() const;
    inline ConstElement leftSibling(size_t distance = 1) const;
    inline ConstElement rightSibling(size_t distance = 1) const;
    inline ConstElement parent() const;
    inline ConstElement findNthChild(size_t n) const;
    inline ConstElement operator[](size_t n) const;
    inline ConstElement findFirstChildNamed(StringData name) const;
    inline ConstElement operator[](StringData n) const;
    inline ConstElement findElementNamed(StringData name) const;

    inline size_t countSiblingsLeft() const;
    inline size_t countSiblingsRight() const;
    inline size_t countChildren() const;

    inline bool hasValue() const;
    inline BSONElement getValue() const;

    inline double getValueDouble() const;
    inline StringData getValueString() const;
    inline BSONObj getValueObject() const;
    inline BSONArray getValueArray() const;
    inline bool isValueUndefined() const;
    inline OID getValueOID() const;
    inline bool getValueBool() const;
    inline Date_t getValueDate() const;
    inline bool isValueNull() const;
    inline StringData getValueSymbol() const;
    inline int32_t getValueInt() const;
    inline Timestamp getValueTimestamp() const;
    inline int64_t getValueLong() const;
    inline Decimal128 getValueDecimal() const;
    inline bool isValueMinKey() const;
    inline bool isValueMaxKey() const;
    inline SafeNum getValueSafeNum() const;

    inline int compareWithElement(const ConstElement& other,
                                  const StringDataComparator* comparator,
                                  bool considerFieldName = true) const;

    inline int compareWithBSONElement(const BSONElement& other,
                                      const StringDataComparator* comparator,
                                      bool considerFieldName = true) const;

    inline int compareWithBSONObj(const BSONObj& other,
                                  const StringDataComparator* comparator,
                                  bool considerFieldName = true) const;

    inline void writeTo(BSONObjBuilder* builder) const;
    inline void writeArrayTo(BSONArrayBuilder* builder) const;

    inline bool ok() const;
    inline const Document& getDocument() const;
    inline BSONType getType() const;
    inline bool isType(BSONType type) const;
    inline StringData getFieldName() const;
    inline Element::RepIdx getIdx() const;

    inline std::string toString() const;

    friend bool operator==(const ConstElement&, const ConstElement&);

private:
    friend class Document;

    template <typename Builder>
    inline void writeElement(Builder* builder, const StringData* fieldName = nullptr) const;

    Element _basis;
};

/** See notes for operator==(const Element&, const Element&). The multiple variants listed
 *  here enable cross type comparisons between Elements and ConstElements.
 */
inline bool operator==(const ConstElement& l, const ConstElement& r);
inline bool operator!=(const ConstElement& l, const ConstElement& r);
inline bool operator==(const Element& l, const ConstElement& r);
inline bool operator!=(const Element& l, const ConstElement& r);
inline bool operator==(const ConstElement& l, const Element& r);
inline bool operator!=(const ConstElement& l, const Element& r);

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

inline BSONElement ConstElement::getValue() const {
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

inline int ConstElement::compareWithElement(const ConstElement& other,
                                            const StringDataComparator* comparator,
                                            bool considerFieldName) const {
    return _basis.compareWithElement(other, comparator, considerFieldName);
}

inline int ConstElement::compareWithBSONElement(const BSONElement& other,
                                                const StringDataComparator* comparator,
                                                bool considerFieldName) const {
    return _basis.compareWithBSONElement(other, comparator, considerFieldName);
}

inline int ConstElement::compareWithBSONObj(const BSONObj& other,
                                            const StringDataComparator* comparator,
                                            bool considerFieldName) const {
    return _basis.compareWithBSONObj(other, comparator, considerFieldName);
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
