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

#include <cstddef>
#include <cstdint>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/mutable/api.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/visibility.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/safe_num.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace mutablebson {

/** For an overview of mutable BSON, please see the file document.h in this directory. */

class ConstElement;
class Document;

/** Element represents a BSON value or object in a mutable BSON Document. The lifetime of
 *  an Element is a subset of the Document to which it belongs. Much like a BSONElement, an
 *  Element has a type, a field name, and (usually) a value. An Element may be used to read
 *  or modify the value (including changing its type), to navigate to related Elements in
 *  the Document tree, or for a number of topological changes to the Document
 *  structure. Element also offers the ability to compare its value to that of other
 *  Elements, and to serialize its value to a BSONObjBuilder or BSONArrayBuilder.
 *
 *  Elements have reference or iterator like semantics, and are very lightweight. You
 *  should not worry about the cost of passing an Element by value, copying an Element, or
 *  similar operations. Such operations do not mean that the logical element in the
 *  underlying Document is duplicated. Only the reference is duplicated.
 *
 *  The API for Element is broken into several sections:
 *
 *  - Topology mutation: These methods are to either add other Elements to the Document
 *    tree as siblings or children (when applicable) of the current Element, to remove the
 *    Element from the tree, or to remove children of the Element (when applicable).
 *
 *  - Navigation: These methods are used to navigate the Document tree by returning other
 *    Elements in specified relationships to the current Element. In this regard, Elements
 *    act much like STL iterators that walk over the Document tree. One important
 *    difference is that Elements are never invalidated, even when 'remove' is called. If
 *    you have two Elements that alias the same element in the Document tree, modifications
 *    through one Element will be visible via the other.
 *
 *  - Value access: These methods provide access to the value in the Document tree that the
 *    current Element represents. All leaf (a.k.a. 'primitive', or non-Object and
 *    non-Array) like Elements will always be able to provide a value. However, there are
 *    cases where non-leaf Elements (representing Objects or Arrays) cannot provide a
 *    value. Therefore, you must always call 'hasValue' to determine if the value is
 *    available before calling 'getValue'. Similarly, you must determine the type of the
 *    Element by calling getType() and only call the matching typed getValue.
 *
 *  - Comparison: It is possible to compare one Element with another to determine ordering
 *    or equality as defined by woCompare. Similarly, it is possible to directly compare an
 *    Element with a BSONElement. It is legal to compare two Elements which belong to
 *    different Documents.
 *
 *  - Serialization: Elements may be serialized to BSONObjBuilder or to BSONArrayBuilder
 *    objects when appropriate. One detail to consider is that writeTo for the root Element
 *    behaves differently than the others: it does not start a new subobj scope in the
 *    builder, so all of its children will be added at the current level to the
 *    builder. The provided builder does not have its 'done' method called automatically.
 *
 *  - Value mutation: You may freely modify the value of an Element, including
 *    modifications that change the type of the Element and setting the value of the
 *    Element to the value of another BSONObj. You may also set the value from a SafeNum or
 *    from a BSONElement.
 *
 *  - Accessors: These provide access to various properties of the Element, like the
 *    Document to which the Element belongs, the BSON type and field name of the Element,
 *    etc. One critical accessor is 'ok'. When using the topology API to navigate a
 *    document, it is possible to request an Element which does not exist, like the parent
 *    of the root element, or the left child of an integer, or the right sibling of the
 *    last element in an array. In these cases, the topology API will return an Element for
 *    which the 'ok' accessor will return 'false', which is roughly analagous to an 'end'
 *    valued STL iterator. It is illegal to call any method (other than 'ok') on a non-OK
 *    Element.
 *
 *  - Streaming API: As a convenience for when you are building Documents from scratch, an
 *    API is provided that combines the effects of calling makeElement on the Document with
 *    calling pushBack on the current Element. The effect is to create the element and make
 *    it the new rightmost child of this Element. Use of this API is discouraged and it may
 *    be removed.
 */

class MONGO_MUTABLE_BSON_API Element {
public:
    typedef uint32_t RepIdx;

    // Some special RepIdx values. These are really implementation details, but they are
    // here so that we can inline Element::OK, which gets called very frequently, and they
    // need to be public so some free functions in document.cpp can use them. You must not
    // use these values explicitly.

    // Used to signal an invalid Element.
    static const RepIdx kInvalidRepIdx = RepIdx(-1);

    // A rep that points to an unexamined entity
    static const RepIdx kOpaqueRepIdx = RepIdx(-2);

    // This is the highest valid rep that does not overlap flag values.
    static const RepIdx kMaxRepIdx = RepIdx(-3);

    //
    // Topology mutation API. Element arguments must belong to the same Document.
    //

    /** Add the provided Element to the left of this Element. The added Element must be
     *  'ok', and this Element must have a parent.
     */
    Status addSiblingLeft(Element e);

    /** Add the provided Element to the right of this Element. The added Element must be
     *  'ok', and this Element must have a parent.
     */
    Status addSiblingRight(Element e);

    /** 'Remove' this Element by detaching it from its parent and siblings. The Element
     *  continues to exist and may be manipulated, but cannot be re-obtained by navigating
     *  from the root.
     */
    Status remove();

    /** If this Element is empty, add 'e' as the first child. Otherwise, add 'e' as the new
     *  left child.
     */
    Status pushFront(Element e);

    /** If this Element is empty, add 'e' as the first child. Otherwise, add 'e' as the new
     *  right child.
     */
    Status pushBack(Element e);

    /** Remove the leftmost child Element if it exists, otherwise return an error. */
    Status popFront();

    /** Remove the rightmost child Element if it exists, otherwise return an error. */
    Status popBack();

    /** Rename this Element to the provided name. */
    Status rename(StringData newName);


    //
    // Navigation API.
    //

    /** Returns either this Element's left child, or a non-ok Element if no left child
     *  exists.
     */
    Element leftChild() const;

    /** Returns either this Element's right child, or a non-ok Element if no right child
     *  exists. Note that obtaining the right child may require realizing all immediate
     *  child nodes of a document that is being consumed lazily.
     */
    Element rightChild() const;

    /** Returns true if this element has children. Always returns false if this Element is
     *  not an Object or Array.
     */
    bool hasChildren() const;

    /** Returns either this Element's sibling 'distance' elements to the left, or a non-ok
     *  Element if no such left sibling exists.
     */
    Element leftSibling(size_t distance = 1) const;

    /** Returns either this Element's sibling 'distance' Elements to the right, or a non-ok
     *  Element if no such right sibling exists.
     */
    Element rightSibling(size_t distance = 1) const;

    /** Returns this Element's parent, or a non-ok Element if this Element has no parent
     *  (is a root).
     */
    Element parent() const;

    /** Returns the nth child, if any, of this Element. If no such element exists, a non-ok
     *  Element is returned. This is not a constant time operation. This method is also
     *  available as operator[] taking a size_t for convenience.
     */
    Element findNthChild(size_t n) const;
    inline Element operator[](size_t n) const;

    /** Returns the first child, if any, of this Element named 'name'. If no such Element
     *  exists, a non-ok Element is returned. This is not a constant time operation. It is illegal
     *  to call this on an Array. This method is also available as operator[] taking a StringData
     *  for convenience.
     */
    Element findFirstChildNamed(StringData name) const;
    inline Element operator[](StringData name) const;

    /** Returns the first element found named 'name', starting the search at the current
     *  Element, and walking right. If no such Element exists, a non-ok Element is
     *  returned. This is not a constant time operation. This implementation is used in the
     *  specialized implementation of findElement<ElementType, FieldNameEquals>.
     */
    Element findElementNamed(StringData name) const;

    //
    // Counting API.
    //

    /** Returns the number of valid siblings to the left of this Element. */
    size_t countSiblingsLeft() const;

    /** Returns the number of valid siblings to the right of this Element. */
    size_t countSiblingsRight() const;

    /** Return the number of children of this Element. */
    size_t countChildren() const;

    //
    // Value access API.
    //
    // We only provide accessors for BSONElement and for simple types. For more complex
    // types like regex you should obtain the BSONElement and use that API to extract the
    // components.
    //
    // Note that the getValueX methods are *unchecked* in release builds: You are
    // responsible for calling hasValue() to ensure that this element has a value
    // representation, and for calling getType to ensure that the Element is of the proper
    // type.
    //
    // As usual, methods here are in bsonspec type order, please keep them that way.
    //

    /** Returns true if 'getValue' can return a valid BSONElement from which a value may be
     *  extracted. See the notes for 'getValue' to understand the conditions under which an
     *  Element can provide a BSONElement.
     */
    bool hasValue() const;

    /** Returns true if this element is a numeric type (e.g. NumberLong). Currently, the
     *  only numeric BSON types are NumberLong, NumberInt, NumberDouble, and NumberDecimal.
     */
    bool isNumeric() const;

    /** Returns true if this element is one of the integral numeric types (e.g. NumberLong
     *  or NumberInt).
     */
    bool isIntegral() const;

    /** Get the value of this element if available. Note that not all elements have a
     *  representation as a BSONElement. For elements that do have a representation, this
     *  will return it. For elements that do not this method returns an eoo
     *  BSONElement. All 'value-ish' Elements will have a BSONElement
     *  representation. 'Tree-ish' Elements may or may not have a BSONElement
     *  representation. Mutations may cause elements to change whether or not they have a
     *  value and may invalidate previously returned values.
     *
     *  Please note that a const BSONElement allows retrieval of a non-const
     *  BSONObj. However, the contents of the BSONElement returned here must be treated as
     *  const.
     */
    BSONElement getValue() const;

    /** Get the value from a double valued Element. */
    inline double getValueDouble() const;

    /** Get the value from a std::string valued Element. */
    inline StringData getValueString() const;

    /** Get the value from an object valued Element. Note that this may not always be
     *  possible!
     */
    inline BSONObj getValueObject() const;

    /** Get the value from an object valued Element. Note that this may not always be
     *  possible!
     */
    inline BSONArray getValueArray() const;

    /** Returns true if this Element is the undefined type. */
    inline bool isValueUndefined() const;

    /** Get the value from an OID valued Element. */
    inline OID getValueOID() const;

    /** Get the value from a bool valued Element. */
    inline bool getValueBool() const;

    /** Get the value from a date valued Element. */
    inline Date_t getValueDate() const;

    /** Returns true if this Element is the null type. */
    inline bool isValueNull() const;

    /** Get the value from a symbol valued Element. */
    inline StringData getValueSymbol() const;

    /** Get the value from an int valued Element. */
    inline int32_t getValueInt() const;

    /** Get the value from a timestamp valued Element. */
    inline Timestamp getValueTimestamp() const;

    /** Get the value from a long valued Element. */
    inline int64_t getValueLong() const;

    /** Get the value from a decimal valued Element. */
    inline Decimal128 getValueDecimal() const;

    /** Returns true if this Element is the min key type. */
    inline bool isValueMinKey() const;

    /** Returns true if this Element is the max key type. */
    inline bool isValueMaxKey() const;

    /** Returns the numeric value as a SafeNum */
    SafeNum getValueSafeNum() const;


    //
    // Comparision API.
    //

    /** Compare this Element with Element 'other'. The two Elements may belong to different
     *  Documents. You should not call this on the root Element of the Document because the
     *  root Element does not have a field name. Use compareWithBSONObj to handle that
     *  case.
     *
     *   Returns -1 if this < other according to BSONElement::woCompare
     *   Returns 0 if this == other either tautologically, or according to woCompare.
     *   Returns 1 if this > other according to BSONElement::woCompare
     */
    int compareWithElement(const ConstElement& other,
                           const StringDataComparator* comparator,
                           bool considerFieldName = true) const;

    /** Compare this Element with BSONElement 'other'. You should not call this on the root
     *  Element of the Document because the root Element does not have a field name. Use
     *  compareWithBSONObj to handle that case.
     *
     *   Returns -1 if this < other according to BSONElement::woCompare
     *   Returns 0 if this == other either tautologically, or according to woCompare.
     *   Returns 1 if this > other according to BSONElement::woCompare
     */
    int compareWithBSONElement(const BSONElement& other,
                               const StringDataComparator* comparator,
                               bool considerFieldName = true) const;

    /** Compare this Element, which must be an Object or an Array, with 'other'.
     *
     *   Returns -1 if this object < other according to BSONElement::woCompare
     *   Returns 0 if this object == other either tautologically, or according to woCompare.
     *   Returns 1 if this object > other according to BSONElement::woCompare
     */
    int compareWithBSONObj(const BSONObj& other,
                           const StringDataComparator* comparator,
                           bool considerFieldName = true) const;


    //
    // Serialization API.
    //

    /**
     * Writes this Element to the provided object builder. Note that if this element is not a root,
     * then it gets wrapped inside an object.
     */
    void writeTo(BSONObjBuilder* builder) const;

    /**
     * Writes all the children of the current Element to the provided object builder. This Element
     * must be of type mongo::Object.
     */
    void writeChildrenTo(BSONObjBuilder* builder) const;

    /**
     * Write this Element to the provided array builder. This Element must be of type
     * mongo::Array.
     */
    void writeArrayTo(BSONArrayBuilder* builder) const;


    //
    // Value mutation API. Please note that the types are ordered according to bsonspec.org
    // ordering. Please keep them that way.
    //

    /** Set the value of this Element to the given double. */
    Status setValueDouble(double value);

    /** Set the value of this Element to the given string. */
    Status setValueString(StringData value);

    /** Set the value of this Element to the given object. The data in 'value' is
     *  copied.
     */
    Status setValueObject(const BSONObj& value);

    /** Set the value of this Element to the given object. The data in 'value' is
     *  copied.
     */
    Status setValueArray(const BSONObj& value);

    /** Set the value of this Element to the given binary data. */
    Status setValueBinary(uint32_t len, mongo::BinDataType binType, const void* data);

    /** Set the value of this Element to Undefined. */
    Status setValueUndefined();

    /** Set the value of this Element to the given OID. */
    Status setValueOID(OID value);

    /** Set the value of this Element to the given boolean. */
    Status setValueBool(bool value);

    /** Set the value of this Element to the given date. */
    Status setValueDate(Date_t value);

    /** Set the value of this Element to Null. */
    Status setValueNull();

    /** Set the value of this Element to the given regex parameters. */
    Status setValueRegex(StringData re, StringData flags);

    /** Set the value of this Element to the given db ref parameters. */
    Status setValueDBRef(StringData ns, OID oid);

    /** Set the value of this Element to the given code data. */
    Status setValueCode(StringData value);

    /** Set the value of this Element to the given symbol. */
    Status setValueSymbol(StringData value);

    /** Set the value of this Element to the given code and scope data. */
    Status setValueCodeWithScope(StringData code, const BSONObj& scope);

    /** Set the value of this Element to the given integer. */
    Status setValueInt(int32_t value);

    /** Set the value of this Element to the given timestamp. */
    Status setValueTimestamp(Timestamp value);

    /** Set the value of this Element to the given long integer */
    Status setValueLong(int64_t value);

    /** Set the value of this Element to the given decimal. */
    Status setValueDecimal(Decimal128 value);

    /** Set the value of this Element to MinKey. */
    Status setValueMinKey();

    /** Set the value of this Element to MaxKey. */
    Status setValueMaxKey();


    //
    // Value mutation API from variant types.
    //

    /** Set the value of this element to equal the value of the provided BSONElement
     *  'value'. The name of this Element is not modified.
     *
     *  The contents of value are copied.
     */
    Status setValueBSONElement(const BSONElement& value);

    /** Set the value of this Element to a numeric type appropriate to hold the given
     *  SafeNum value.
     */
    Status setValueSafeNum(SafeNum value);

    /** Set the value of this Element to the value from another Element.
     *
     * The name of this Element is not modified.
     */
    Status setValueElement(ConstElement setFrom);


    //
    // Accessors
    //

    /** Returns true if this Element represents a valid part of the Document. */
    inline bool ok() const;

    /** Returns the Document to which this Element belongs. */
    inline Document& getDocument();

    /** Returns the Document to which this Element belongs. */
    inline const Document& getDocument() const;

    /** Returns the BSONType of this Element. */
    BSONType getType() const;

    /** Returns true if this Element is of the specified type */
    inline bool isType(BSONType type) const;

    /** Returns the field name of this Element. Note that the value returned here is not
     *  stable across mutations, since the storage for fieldNames may be reallocated. If
     *  you need a stable version of the fieldName, you must call toString on the returned
     *  StringData.
     */
    StringData getFieldName() const;

    /** Returns the opaque ID for this element. This is unlikely to be useful to a caller
     *  and is mostly for testing.
     */
    inline RepIdx getIdx() const;


    //
    // Stream API - BSONObjBuilder like API, but methods return a Status.  These are
    // strictly a convenience API. You don't need to use them if you would rather be more
    // explicit.
    //

    /** Append the provided double value as a new field with the provided name. */
    Status appendDouble(StringData fieldName, double value);

    /** Append the provided std::string value as a new field with the provided name. */
    Status appendString(StringData fieldName, StringData value);

    /** Append the provided object as a new field with the provided name. The data in
     *  'value' is copied.
     */
    Status appendObject(StringData fieldName, const BSONObj& value);

    /** Append the provided array object as a new field with the provided name. The data in
     *  value is copied.
     */
    Status appendArray(StringData fieldName, const BSONObj& value);

    /** Append the provided binary data as a new field with the provided name. */
    Status appendBinary(StringData fieldName,
                        uint32_t len,
                        mongo::BinDataType binType,
                        const void* data);

    /** Append an undefined value as a new field with the provided name. */
    Status appendUndefined(StringData fieldName);

    /** Append the provided OID as a new field with the provided name. */
    Status appendOID(StringData fieldName, mongo::OID value);

    /** Append the provided bool as a new field with the provided name. */
    Status appendBool(StringData fieldName, bool value);

    /** Append the provided date as a new field with the provided name. */
    Status appendDate(StringData fieldName, Date_t value);

    /** Append a null as a new field with the provided name. */
    Status appendNull(StringData fieldName);

    /** Append the provided regex data as a new field with the provided name. */
    Status appendRegex(StringData fieldName, StringData re, StringData flags);

    /** Append the provided DBRef data as a new field with the provided name. */
    Status appendDBRef(StringData fieldName, StringData ns, mongo::OID oid);

    /** Append the provided code data as a new field with the iven name. */
    Status appendCode(StringData fieldName, StringData value);

    /** Append the provided symbol data as a new field with the provided name. */
    Status appendSymbol(StringData fieldName, StringData value);

    /** Append the provided code and scope data as a new field with the provided name. */
    Status appendCodeWithScope(StringData fieldName, StringData code, const BSONObj& scope);

    /** Append the provided integer as a new field with the provided name. */
    Status appendInt(StringData fieldName, int32_t value);

    /** Append the provided timestamp as a new field with the provided name. */
    Status appendTimestamp(StringData fieldName, Timestamp value);

    /** Append the provided long integer as a new field with the provided name. */
    Status appendLong(StringData fieldName, int64_t value);

    /** Append the provided decimal as a new field with the provided name. */
    Status appendDecimal(StringData fieldName, Decimal128 value);

    /** Append a max key as a new field with the provided name. */
    Status appendMinKey(StringData fieldName);

    /** Append a min key as a new field with the provided name. */
    Status appendMaxKey(StringData fieldName);

    /** Append the given BSONElement. The data in 'value' is copied. */
    Status appendElement(const BSONElement& value);

    /** Append the provided number as field of the appropriate numeric type with the
     *  provided name.
     */
    Status appendSafeNum(StringData fieldName, SafeNum value);

    /** Convert this element to its JSON representation if ok(),
     *  otherwise return !ok() message */
    std::string toString() const;

private:
    friend class Document;
    friend class ConstElement;

    friend bool operator==(const Element&, const Element&);

    inline Element(Document* doc, RepIdx repIdx);

    MONGO_PRIVATE Status addChild(Element e, bool front);

    MONGO_PRIVATE StringData getValueStringOrSymbol() const;

    MONGO_PRIVATE Status setValue(Element::RepIdx newValueIdx);

    Document* _doc;
    RepIdx _repIdx;
};

/** Element comparison support. Comparison is like STL iterator comparision: equal Elements
 *  refer to the same underlying data. The equality does *not* mean that the underlying
 *  values are equivalent. Use the Element::compareWith methods to compare the represented
 *  data.
 */

/** Returns true if l and r refer to the same data, false otherwise. */
inline bool operator==(const Element& l, const Element& r);

/** Returns false if l and r refer to the same data, true otherwise. */
inline bool operator!=(const Element& l, const Element& r);

inline Element Element::operator[](size_t n) const {
    return findNthChild(n);
}

inline Element Element::operator[](StringData name) const {
    return findFirstChildNamed(name);
}

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

inline Timestamp Element::getValueTimestamp() const {
    dassert(hasValue() && isType(mongo::bsonTimestamp));
    return getValue().timestamp();
}

inline int64_t Element::getValueLong() const {
    dassert(hasValue() && isType(mongo::NumberLong));
    return getValue()._numberLong();
}

inline Decimal128 Element::getValueDecimal() const {
    dassert(hasValue() && isType(mongo::NumberDecimal));
    return getValue()._numberDecimal();
}

inline bool Element::isValueMinKey() const {
    return isType(mongo::MinKey);
}

inline bool Element::isValueMaxKey() const {
    return isType(mongo::MaxKey);
}

inline bool Element::ok() const {
    dassert(_doc != nullptr);
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

inline Element::Element(Document* doc, RepIdx repIdx) : _doc(doc), _repIdx(repIdx) {
    dassert(_doc != nullptr);
}

inline StringData Element::getValueStringOrSymbol() const {
    return getValue().valueStringData();
}

inline bool operator==(const Element& l, const Element& r) {
    return (l._doc == r._doc) && (l._repIdx == r._repIdx);
}

inline bool operator!=(const Element& l, const Element& r) {
    return !(l == r);
}


}  // namespace mutablebson
}  // namespace mongo
