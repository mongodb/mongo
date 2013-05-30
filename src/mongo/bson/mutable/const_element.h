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
        inline ConstElement leftSibling() const;
        inline ConstElement rightSibling() const;
        inline ConstElement parent() const;
        inline ConstElement operator[](size_t n) const;
        inline ConstElement operator[](const StringData& n) const;

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
        inline OpTime getValueTimestamp() const;
        inline int64_t getValueLong() const;
        inline bool isValueMinKey() const;
        inline bool isValueMaxKey() const;
        inline SafeNum getValueSafeNum() const;

        inline int compareWithElement(const ConstElement& other,
                                      bool considerFieldName = true) const;

        inline int compareWithBSONElement(const BSONElement& other,
                                          bool considerFieldName = true) const;

        inline int compareWithBSONObj(const BSONObj& other,
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

        template<typename Builder>
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

} // namespace mutablebson
} // namespace mongo

#include "mongo/bson/mutable/const_element-inl.h"

