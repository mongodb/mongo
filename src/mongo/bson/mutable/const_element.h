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
class ConstElement {
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
    inline const BSONElement getValue() const;

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

    inline int compareWithElement(
        const ConstElement& other,
        bool considerFieldName = true,
        const StringData::ComparatorInterface* comparator = nullptr) const;

    inline int compareWithBSONElement(
        const BSONElement& other,
        bool considerFieldName = true,
        const StringData::ComparatorInterface* comparator = nullptr) const;

    inline int compareWithBSONObj(
        const BSONObj& other,
        bool considerFieldName = true,
        const StringData::ComparatorInterface* comparator = nullptr) const;

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
    inline void writeElement(Builder* builder, const StringData* fieldName = NULL) const;

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

}  // namespace mutablebson
}  // namespace mongo

#include "mongo/bson/mutable/const_element-inl.h"
