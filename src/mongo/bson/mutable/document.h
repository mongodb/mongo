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
#include <memory>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/mutable/api.h"
#include "mongo/bson/mutable/const_element.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/visibility.h"
#include "mongo/util/safe_num.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace mutablebson {

/** Mutable BSON Overview
 *
 *  Mutable BSON provides classes to facilitate the manipulation of existing BSON objects
 *  or the construction of new BSON objects from scratch in an incremental fashion. The
 *  operations (including additions, deletions, renamings, type changes and value
 *  modification) that are to be performed do not need to be known ahead of time, and do
 *  not need to occur in any particular order. This is in contrast to BSONObjBuilder and
 *  BSONArrayBuilder which offer only serialization and cannot revise already serialized
 *  data. If you need to build a BSONObj but you know upfront what you need to build then
 *  you should use BSONObjBuilder and BSONArrayBuilder directly as they will be faster and
 *  less resource intensive.
 *
 *  The classes in this library (Document, Element, and ConstElement) present a tree-like
 *  (or DOM like) interface. Elements are logically equivalent to BSONElements: they carry
 *  a type, a field name, and a value. Every Element belongs to a Document, which roots the
 *  tree, and Elements of proper type (mongo::Object or mongo::Array) may have child
 *  Elements of their own. Given an Element, you may navigate to the Element's parent, to
 *  its siblings to the left or right of the Element in the tree, and to the leftmost or
 *  rightmost children of the Element. Note that some Elements may not offer all of these
 *  relationships: An Element that represents a terminal BSON value (like an integer) will
 *  not have children (though it may well have siblings). Similarly, an Element that is an
 *  'only child' will not have any left or right siblings. Given a Document, you may begin
 *  navigating by obtaining the root Element of the tree by calling Document::root. See the
 *  documentation for the Element class for the specific navigation methods that will be
 *  available from the root Element.
 *
 *  Elements within the Document may be modified in various ways: the value of the Element
 *  may be changed, the Element may be removed, it may be renamed, and if it is eligible
 *  for children (i.e. it represents a mongo::Array or mongo::Object) it may have child
 *  Elements added to it. Once you have completed building or modifying the Document, you
 *  may write it back out to a BSONObjBuilder by calling Document::writeTo. You may also
 *  serialize individual Elements within the Document to BSONObjBuilder or BSONArrayBuilder
 *  objects by calling Element::writeTo or Element::writeArrayTo.
 *
 *  In addition to the above capabilities, there are algorithms provided in 'algorithm.h'
 *  to help with tasks like searching for Elements that match a predicate or for sorting
 *  the children of an Object Element.
 *
 *  Example 1: Building up a document from scratch, reworking it, and then serializing it:

     namespace mmb = mongo::mutablebson;
     // Create a new document
     mmb::Document doc;
     // doc contents: '{}'

     // Get the root of the document.
     mmb::Element root = doc.root();

     // Create a new mongo::NumberInt typed Element to represent life, the universe, and
     // everything, then push that Element into the root object, making it a child of root.
     mmb::Element e0 = doc.makeElementInt("ltuae", 42);
     root.pushBack(e0);
     // doc contents: '{ ltuae : 42 }'

     // Create a new empty mongo::Object-typed Element named 'magic', and push it back as a
     // child of the root, making it a sibling of e0.
     mmb::Element e1 = doc.makeElementObject("magic");
     root.pushBack(e1);
     // doc contents: '{ ltuae : 42, magic : {} }'

     // Create a new mongo::NumberDouble typed Element to represent Pi, and insert it as child
     // of the new object we just created.
     mmb::Element e3 = doc.makeElementDouble("pi", 3.14);
     e1.pushBack(e3);
     // doc contents: '{ ltuae : 42, magic : { pi : 3.14 } }'

     // Create a new mongo::NumberDouble to represent Plancks constant in electrovolt
     // micrometers, and add it as a child of the 'magic' object.
     mmb::Element e4 = doc.makeElementDouble("hbar", 1.239);
     e1.pushBack(e4);
     // doc contents: '{ ltuae : 42, magic : { pi : 3.14, hbar : 1.239 } }'

     // Rename the parent element of 'hbar' to be 'constants'.
     e4.parent().rename("constants");
     // doc contents: '{ ltuae : 42, constants : { pi : 3.14, hbar : 1.239 } }'

     // Rename 'ltuae' to 'answer' by accessing it as the root objects left child.
     doc.root().leftChild().rename("answer");
     // doc contents: '{ answer : 42, constants : { pi : 3.14, hbar : 1.239 } }'

     // Sort the constants by name.
     mmb::sortChildren(doc.root().rightChild(), mmb::FieldNameLessThan());
     // doc contents: '{ answer : 42, constants : { hbar : 1.239, pi : 3.14 } }'

     mongo::BSONObjBuilder builder;
     doc.writeTo(&builder);
     mongo::BSONObj result = builder.obj();
     // result contents: '{ answer : 42, constants : { hbar : 1.239, pi : 3.14 } }'

 *  While you can use this library to build Documents from scratch, its real purpose is to
 *  manipulate existing BSONObjs. A BSONObj may be passed to the Document constructor or to
 *  Document::make[Object|Array]Element, in which case the Document or Element will reflect
 *  the values contained within the provided BSONObj. Modifications will not alter the
 *  underlying BSONObj: they are held off to the side within the Document. However, when
 *  the Document is subsequently written back out to a BSONObjBuilder, the modifications
 *  applied to the Document will be reflected in the serialized version.
 *
 *  Example 2: Modifying an existing BSONObj (some error handling removed for length)

     namespace mmb = mongo::mutablebson;

     static const char inJson[] =
         "{"
         "  'whale': { 'alive': true, 'dv': -9.8, 'height': 50, attrs : [ 'big' ] },"
         "  'petunias': { 'alive': true, 'dv': -9.8, 'height': 50 } "
         "}";
     mongo::BSONObj obj = mongo::fromjson(inJson);

     // Create a new document representing the BSONObj with the above contents.
     mmb::Document doc(obj);

     // The whale hits the planet and dies.
     mmb::Element whale = mmb::findFirstChildNamed(doc.root(), "whale");
     // Find the 'dv' field in the whale.
     mmb::Element whale_deltav = mmb::findFirstChildNamed(whale, "dv");
     // Set the dv field to zero.
     whale_deltav.setValueDouble(0.0);
     // Find the 'height' field in the whale.
     mmb::Element whale_height = mmb::findFirstChildNamed(whale, "height");
     // Set the height field to zero.
     whale_deltav.setValueDouble(0);
     // Find the 'alive' field, and set it to false.
     mmb::Element whale_alive = mmb::findFirstChildNamed(whale, "alive");
     whale_alive.setValueBool(false);

     // The petunias survive, update its fields much like we did above.
     mmb::Element petunias = mmb::findFirstChildNamed(doc.root(), "petunias");
     mmb::Element petunias_deltav = mmb::findFirstChildNamed(petunias, "dv");
     petunias_deltav.setValueDouble(0.0);
     mmb::Element petunias_height = mmb::findFirstChildNamed(petunias, "height");
     petunias_deltav.setValueDouble(0);

     // Replace the whale by its wreckage, saving only its attributes:
     // Construct a new mongo::Object element for the ex-whale.
     mmb::Element ex_whale = doc.makeElementObject("ex-whale");
     doc.root().pushBack(ex_whale);
     // Find the attributes of the old 'whale' element.
     mmb::Element whale_attrs = mmb::findFirstChildNamed(whale, "attrs");
     // Remove the attributes from the whale (they remain valid, but detached).
     whale_attrs.remove();
     // Add the attributes into the ex-whale.
     ex_whale.pushBack(whale_attrs);
     // Remove the whale object.
     whale.remove();

     // Current state of document:
     "{"
     "    'petunias': { 'alive': true, 'dv': 0.0, 'height': 50 },"
     "    'ex-whale': { 'attrs': [ 'big' ] } })"
     "}";

 * Both of the above examples are derived from tests in mutable_bson_test.cpp, see the
 * tests Example1 and Example2 if you would like to play with the code.
 *
 * Additional details on Element and Document are available in their class and member
 * comments.
 */

/** Document is the entry point into the mutable BSON system. It has a fairly simple
 *  API. It acts as an owner for the Element resources of the document, provides a
 *  pre-constructed designated root Object Element, and acts as a factory for new Elements,
 *  which may then be attached to the root or to other Elements by calling the appropriate
 *  topology mutation methods in Element.
 *
 *  The default constructor builds an empty Document which you may then extend by creating
 *  new Elements and manipulating the tree topology. It is also possible to build a
 *  Document that derives its initial values from a BSONObj. The given BSONObj will not be
 *  modified, but it also must not be modified elsewhere while Document is using it. Unlike
 *  all other calls in this library where a BSONObj is passed in, the one argument Document
 *  constructor *does not copy* the BSONObj's contents, so they must remain valid for the
 *  duration of Documents lifetime. Document does hold a copy of the BSONObj itself, so it
 *  will up the refcount if the BSONObj internals are counted.
 *
 *  Newly constructed Elements formed by calls to 'makeElement[Type]' methods are not
 *  attached to the root of the document. You must explicitly attach them somewhere. If you
 *  lose the Element value that is returned to you from a 'makeElement' call before you
 *  attach it to the tree then the value will be unreachable. Elements in a document do not
 *  outlive the Document.
 *
 *  Document provides a convenience method to serialize all of the Elements in the tree
 *  that are reachable from the root element to a BSONObjBuilder. In general you should use
 *  this in preference to root().writeTo() if you mean to write the entire
 *  Document. Similarly, Document provides wrappers for comparisons that simply delegate to
 *  comparison operations on the root Element.
 *
 *  A 'const Document' is very limited: you may only write its contents out or obtain a
 *  ConstElement for the root. ConstElement is much like Element, but does not permit
 *  mutations. See the class comment for ConstElement for more information.
 */
class MONGO_MUTABLE_BSON_API Document {
    // TODO: In principle there is nothing that prevents implementing a deep copy for
    // Document, but for now it is not permitted.
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

public:
    //
    // Lifecycle
    //

    /** Construct a new empty document. */
    Document();

    enum InPlaceMode {
        kInPlaceDisabled = 0,
        kInPlaceEnabled = 1,
    };

    /** Construct new document for the given BSONObj. The data in 'value' is NOT copied. By
     *  default, queueing of in-place modifications against the underlying document is
     *  permitted. To disable this behavior, explicitly pass kInPlaceDisabled.
     */
    explicit Document(const BSONObj& value, InPlaceMode inPlaceMode = kInPlaceEnabled);

    /** Abandon all internal state associated with this Document, and return to a state
     *  semantically equivalent to that yielded by a call to the default constructor. All
     *  objects associated with the current document state are invalidated (e.g. Elements,
     *  BSONElements, BSONObj's values, field names, etc.). This method is useful because
     *  it may (though it is not required to) preserve the memory allocation of the
     *  internal data structures of Document. If you need to logically create and destroy
     *  many Documents in serial, it may be faster to reset.
     */
    void reset();

    /** As the no argument 'reset', but returns to a state semantically equivalent to that
     *  yielded by a call to the two argument constructor with the arguments provided
     *  here. As with the other 'reset' call, all associated objects are invalidated. */
    void reset(const BSONObj& value, InPlaceMode inPlaceMode = kInPlaceEnabled);

    /** Destroy this document permanently */
    ~Document();


    //
    // Comparison API
    //

    /** Compare this Document to 'other' with the semantics of BSONObj::woCompare. */
    inline int compareWith(const Document& other,
                           const StringDataComparator* comparator,
                           bool considerFieldName = true) const;

    /** Compare this Document to 'other' with the semantics of BSONObj::woCompare. */
    inline int compareWithBSONObj(const BSONObj& other,
                                  const StringDataComparator* comparator,
                                  bool considerFieldName = true) const;


    //
    // Serialization API
    //

    /** Serialize the Elements reachable from the root Element of this Document to the
     *  provided builder.
     */
    inline void writeTo(BSONObjBuilder* builder) const;

    /** Serialize the Elements reachable from the root Element of this Document and return
     *  the result as a BSONObj.
     */
    inline BSONObj getObject() const;


    //
    // Element creation API.
    //
    // Newly created elements are not attached to the tree (effectively, they are
    // 'roots'). You must call one of the topology management methods in 'Element' to
    // connect the newly created Element to another Element in the Document, possibly the
    // Element referenced by Document::root. Elements do not outlive the Document.
    //

    /** Create a new double Element with the given value and field name. */
    Element makeElementDouble(StringData fieldName, double value);

    /** Create a new std::string Element with the given value and field name. */
    Element makeElementString(StringData fieldName, StringData value);

    /** Create a new empty object Element with the given field name. */
    Element makeElementObject(StringData fieldName);

    /** Create a new object Element with the given field name. The data in 'value' is
     *  copied.
     */
    Element makeElementObject(StringData fieldName, const BSONObj& value);

    /** Create a new empty array Element with the given field name. */
    Element makeElementArray(StringData fieldName);

    /** Create a new array Element with the given field name. The data in 'value' is
     *  copied.
     */
    Element makeElementArray(StringData fieldName, const BSONObj& value);

    /** Create a new binary Element with the given data and field name. */
    Element makeElementBinary(StringData fieldName,
                              uint32_t len,
                              BinDataType binType,
                              const void* data);

    /** Create a new undefined Element with the given field name. */
    Element makeElementUndefined(StringData fieldName);

    /** Create a new OID + Element with the given field name. */
    Element makeElementNewOID(StringData fieldName);

    /** Create a new OID Element with the given value and field name. */
    Element makeElementOID(StringData fieldName, mongo::OID value);

    /** Create a new bool Element with the given value and field name. */
    Element makeElementBool(StringData fieldName, bool value);

    /** Create a new date Element with the given value and field name. */
    Element makeElementDate(StringData fieldName, Date_t value);

    /** Create a new null Element with the given field name. */
    Element makeElementNull(StringData fieldName);

    /** Create a new regex Element with the given data and field name. */
    Element makeElementRegex(StringData fieldName, StringData regex, StringData flags);

    /** Create a new DBRef Element with the given data and field name. */
    Element makeElementDBRef(StringData fieldName, StringData ns, mongo::OID oid);

    /** Create a new code Element with the given value and field name. */
    Element makeElementCode(StringData fieldName, StringData value);

    /** Create a new symbol Element with the given value and field name. */
    Element makeElementSymbol(StringData fieldName, StringData value);

    /** Create a new scoped code Element with the given data and field name. */
    Element makeElementCodeWithScope(StringData fieldName, StringData code, const BSONObj& scope);

    /** Create a new integer Element with the given value and field name. */
    Element makeElementInt(StringData fieldName, int32_t value);

    /** Create a new timestamp Element with the given value and field name. */
    Element makeElementTimestamp(StringData fieldName, Timestamp value);

    /** Create a new long integer Element with the given value and field name. */
    Element makeElementLong(StringData fieldName, int64_t value);

    /** Create a new dec128 Element with the given value and field name. */
    Element makeElementDecimal(StringData fieldName, Decimal128 value);

    /** Create a new min key Element with the given field name. */
    Element makeElementMinKey(StringData fieldName);

    /** Create a new max key Element with the given field name. */
    Element makeElementMaxKey(StringData fieldName);


    //
    // Element creation methods from variant types
    //

    /** Construct a new Element with the same name, type, and value as the provided
     *  BSONElement. The value is copied.
     */
    Element makeElement(const BSONElement& elt);

    /** Construct a new Element with the same type and value as the provided BSONElement,
     *  but with a new name. The value is copied.
     */
    Element makeElementWithNewFieldName(StringData fieldName, const BSONElement& elt);

    /** Create a new element of the appopriate type to hold the given value, with the given
     *  field name.
     */
    Element makeElementSafeNum(StringData fieldName, SafeNum value);

    /** Construct a new element with the same name, type, and value as the provided mutable
     *  Element. The data is copied from the given Element. Unlike most methods in this
     *  class the provided Element may be from a different Document.
     */
    Element makeElement(ConstElement elt);

    /** Construct a new Element with the same type and value as the provided mutable
     *  Element, but with a new field name. The data is copied from the given
     *  Element. Unlike most methods in this class the provided Element may be from a
     *  different Document.
     */
    Element makeElementWithNewFieldName(StringData fieldName, ConstElement elt);

    //
    // Accessors
    //

    /** Returns the root element for this document. */
    inline Element root();

    /** Returns the root element for this document. */
    inline ConstElement root() const;

    /** Returns an element that will compare equal to a non-ok element. */
    inline Element end();

    /** Returns an element that will compare equal to a non-ok element. */
    inline ConstElement end() const;

    inline std::string toString() const;

    //
    // In-place API.
    //

    /** Ensure that at least 'expectedEvents' damage events can be recorded for in-place
     *  mutations without reallocation. This call is ignored if damage events are disabled.
     */
    void reserveDamageEvents(size_t expectedEvents);

    /** Request a vector of damage events describing in-place updates to this Document. If
     *  the modifications to this Document were not all able to be achieved in-place, then
     *  a non-OK Status is returned, and the provided damage vector will be made empty and
     *  *source set equal to NULL. Otherwise, the provided damage vector is populated, and
     *  the 'source' argument is set to point to a region from which bytes can be read. The
     *  'source' offsets in the damage vector are to be interpreted as offsets within this
     *  region. If the 'size' parameter is non-null and 'source' is set to a non-NULL
     *  value, then size will be filled in with the size of the 'source' region to
     *  facilitate making an owned copy of the source data, in the event that that is
     *  needed.
     *
     *  The lifetime of the source region should be considered to extend only from the
     *  return from this call to before the next API call on this Document or any of its
     *  member Elements. That is almost certainly overly conservative: some read only calls
     *  are undoubtedly fine. But it is very easy to invalidate 'source' by calling any
     *  mutating operation, so proceed with due caution.
     *
     *  It is expected, though, that in normal modes of operation obtainin the damage
     *  vector is one of the last operations performed on a Document before its
     *  destruction, so this is not so great a restriction.
     *
     *  The destination offsets in the damage events are implicitly offsets into the
     *  BSONObj used to construct this Document.
     */
    bool getInPlaceUpdates(DamageVector* damages, const char** source, size_t* size = nullptr);

    /** Drop the queue of in-place update damage events, and do not queue new operations
     *  that would otherwise have been in-place. Use this if you know that in-place updates
     *  will not continue to be possible and do not want to pay the overhead of
     *  speculatively queueing them. After calling this method, getInPlaceUpdates will
     *  return a non-OK Status. It is not possible to re-enable in-place updates once
     *  disabled.
     */
    void disableInPlaceUpdates();

    /** Returns the current in-place mode for the document. Note that for some documents,
     *  like those created without any backing BSONObj, this will always return kForbidden,
     *  since in-place updates make no sense for such an object. In other cases, an object
     *  which started in kInPlacePermitted mode may transition to kInPlaceForbidden if a
     *  topology mutating operation is applied.
     */
    InPlaceMode getCurrentInPlaceMode() const;

    /** A convenience routine, this returns true if the current in-place mode is
     *  kInPlaceEnabled, and false otherwise.
     */
    inline bool isInPlaceModeEnabled() const;

private:
    friend class Element;

    // For now, the implementation of Document is firewalled.
    class MONGO_PRIVATE Impl;
    inline Impl& getImpl();
    inline const Impl& getImpl() const;

    MONGO_PRIVATE Element makeRootElement();
    MONGO_PRIVATE Element makeRootElement(const BSONObj& value);
    MONGO_PRIVATE Element makeElement(ConstElement element, const StringData* fieldName);

    const std::unique_ptr<Impl> _impl;

    // The root element of this document.
    const Element _root;
};

inline int Document::compareWith(const Document& other,
                                 const StringDataComparator* comparator,
                                 bool considerFieldName) const {
    // We cheat and use Element::compareWithElement since we know that 'other' is a
    // Document and has a 'hidden' fieldname that is always indentical across all Document
    // instances.
    return root().compareWithElement(other.root(), comparator, considerFieldName);
}

inline int Document::compareWithBSONObj(const BSONObj& other,
                                        const StringDataComparator* comparator,
                                        bool considerFieldName) const {
    return root().compareWithBSONObj(other, comparator, considerFieldName);
}

inline void Document::writeTo(BSONObjBuilder* builder) const {
    return root().writeTo(builder);
}

inline BSONObj Document::getObject() const {
    BSONObjBuilder builder;
    writeTo(&builder);
    return builder.obj();
}

inline Element Document::root() {
    return _root;
}

inline ConstElement Document::root() const {
    return _root;
}

inline Element Document::end() {
    return Element(this, Element::kInvalidRepIdx);
}

inline ConstElement Document::end() const {
    return const_cast<Document*>(this)->end();
}

inline std::string Document::toString() const {
    return getObject().toString();
}

inline bool Document::isInPlaceModeEnabled() const {
    return getCurrentInPlaceMode() == kInPlaceEnabled;
}

}  // namespace mutablebson
}  // namespace mongo
