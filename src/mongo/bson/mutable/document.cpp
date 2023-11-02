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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/status.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/util/builder.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/debug_util.h"

namespace mongo {
namespace mutablebson {

/** Mutable BSON Implementation Overview
 *
 *  If you haven't read it already, please read the 'Mutable BSON Overview' comment in
 *  document.h before reading further.
 *
 *  In the following discussion, the capitalized terms 'Element' and 'Document' refer to
 *  the classes of the same name. At times, it is also necessary to refer to abstract
 *  'elements' or 'documents', in the sense of bsonspec.org. These latter uses are
 *  non-capitalized. In the BSON specification, there are two 'classes' of
 *  elements. 'Primitive' or 'leaf' elements are those elements which do not contain other
 *  elements. In practice, all BSON types except 'Array' and 'Object' are primitives. The
 *  CodeWScope type is an exception, but one that we sidestep by considering its BSONObj
 *  payload to be opaque.
 *
 *  A mutable BSON Document and its component Elements are implemented in terms of four
 *  data structures. These structures are owned by a Document::Impl object. Each Document
 *  owns a unique Document::Impl, which owns the relevant data structures and provides
 *  accessors, mutators, and helper methods related to those data structures. Understanding
 *  these data structures is critical for understanding how the system as a whole operates.
 *
 *  - The 'Elements Vector': This is a std::vector<ElementRep>, where 'ElementRep' is a
 *    structure type defined below that contains the detailed information about an entity
 *    in the Document (e.g. an Object, or an Array, or a NumberLong, etc.). The 'Element'
 *    and 'ConstElement' objects contain a pointer to a Document (which allows us to reach
 *    the Document::Impl for the Document), and an index into the Elements Vector in the
 *    Document::Impl. These two pieces of information make it possible for us to obtain the
 *    ElementRep associated with a given Element. Note that the Elements Vector is append
 *    only: ElementReps are never removed from it, even if the cooresponding Element is
 *    removed from the Document. By never removing ElementReps, and by using indexes into
 *    the Elements Vector, we can ensure that Elements are never invalidated. Note that
 *    every Document comes with an automatically provided 'root' element of mongo::Object
 *    type. The ElementRep for the root is always in the first slot (index zero) of the
 *    Elements Vector.
 *
 *  - The 'Leaf Builder': This is a standard BSONObjBuilder. When a request is made to the
 *    Document to add new data to the Document via one of the Document::makeElement[TYPE]
 *    calls, the element is constructed by invoking the appropriate method on the Leaf
 *    Builder, forwarding the arguments provided to the call on Document. This results in a
 *    contiguous region of memory which encodes this element, capturing its field name, its
 *    type, and the bytes that encode its value, in the same way it normally does when
 *    using BSONObjBuilder. We then build an ElementRep that indexes into the BufBuilder
 *    behind the BSONObjBuilder (more on how this happens below, in the section on the
 *    'Objects Vector'), then insert that new ElementRep into the ElementsVector, and
 *    finally return an Element that dereferences to the new ElementRep. Subsequently,
 *    requests for the type, fieldname or value bytes via the Element are satisfied by
 *    obtaining the contiguous memory region for the element, which may be used to
 *    construct a BSONElement over that memory region.
 *
 *  - The 'Objects Vector': This is a std::vector<BSONObj>. Any BSONObj object that
 *    provides values for parts of the Document is stored in the Objects Vector. For
 *    instance, in 'Example 2' from document.h, the Document we construct wraps an existing
 *    BSONObj, which is passed in to the Document constructor. That BSONObj would be stored
 *    in the Objects Vector. The data content of the BSONObj is not copied, but the BSONObj
 *    is copied, so the if the BSONObj is counted, we will up its refcount. In any event
 *    the lifetime of the BSONObj must exceed our lifetime by some mechanism. ElementReps
 *    that represent the component elements of the BSONObj store the index of their
 *    supporting BSONObj into the 'objIdx' field of ElementRep. Later, when Elements
 *    referring to those ElementReps are asked for properties like the field name or type
 *    of the Element, the underlying memory region in the appropriate BSONObj may be
 *    examined to provide the relevant data.
 *
 *  - The 'Field Name Heap': For some elements, particularly those in the Leaf Builder or
 *    those embedded in a BSONObj in the Objects Vector, we can easily obtain the field
 *    name by reading it from the encoded BSON. However, some elements are not so
 *    fortunate. Newly created elements of mongo::Array or mongo::Object type, for
 *    instance, don't have a memory region that provides values. In such cases, the field
 *    name is stored in the field name heap, which is simply std::vector<char>, where the
 *    field names are null-byte-delimited. ElementsReps for such elements store an offset
 *    into the Field Name Heap, and when asked for their field name simply return a pointer
 *    to the string data the offset identifies. This exploits the fact that in BSON, valid
 *    field names are null terinated and do not contain embedded null bytes.
 *
 *  - The 'root' Element. Each Document contains a well known Element, which always refers
 *    to a pre-constructed ElementRep at offset zero in the Elements Vector. This is an
 *    Object element, and it is considered as the root of the document tree. It is possible
 *    for ElementReps to exist in the Document data structures, but not be in a child
 *    relationship to the root Element. Newly created Elements, for instance, are in this
 *    sort of 'detached' state until they are attched to another element. Only Element's
 *    that are children of the root element are traversed when calling top level
 *    serialization or comparision operations on Document.
 *
 *  When you construct a Document that obtains its values from an underlying BSONObj, the
 *  entire BSONObj is not 'unpacked' into ElementReps at Document construction
 *  time. Instead, as you ask for Elements with the Element navigation API, the Elements
 *  for children and siblings are created on demand. Subobjects which are never visited
 *  will never have ElementReps constructed for them. Similarly, when writing a Document
 *  back out to a builder, regions of memory that provide values for the Document and which
 *  have not been modified will be block copied, instead of being recursively explored and
 *  written.
 *
 *  To see how these data structures interoperate, we will walk through an example. You may
 *  want to read the comments for ElementRep before tackling the example, since we will
 *  refer to the internal state of ElementRep here. The example code used here exists as a
 *  unit test in mutable_bson_test.cpp as (Documentation, Example3).
 *
 *
 *  Legend:
 *   oi   : objIdx
 *   +/-  : bitfield state (s: serialized, a: array)
 *   x    : invalid/empty rep idx
 *   ?    : opaque rep idx
 *   ls/rs: left/right sibling
 *   lc/rc: left/right child
 *   p    : parent

    static const char inJson[] =
        "{"
        "  'xs': { 'x' : 'x', 'X' : 'X' },"
        "  'ys': { 'y' : 'y' }"
        "}";
    mongo::BSONObj inObj = mongo::fromjson(inJson);
    mmb::Document doc(inObj);

 *    _elements
 *      oi      flags                offset                  ls  rs  lc  rc  p
 *    +-----------------------------------------------------------------------------+
 *  0 | 1 | s:- | ...       | off 0       into _fieldNames | x | x | ? | ? | x      |
 *    +-----------------------------------------------------------------------------+
 *
 *    _objects
 *    +-----------------------------------------------------------------------------+
 *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
 *    +-----------------------------------------------------------------------------+
 *
 *    _fieldNames
 *    +-----------------------------------------------------------------------------+
 *    | \0                                                                          |
 *    +-----------------------------------------------------------------------------+
 *
 *    _leafBuf
 *    +-----------------------------------------------------------------------------+
 *    | {}                                                                          |
 *    +-----------------------------------------------------------------------------+


    mmb::Element root = doc.root();
    mmb::Element xs = root.leftChild();

 *    _elements
 *      oi      flags                offset                  ls  rs  lc  rc  p
 *    +-----------------------------------------------------------------------------+
 *  0 | 1 | s:- | ...       | off 0       into _fieldNames | x | x | 1 | ? | x      | *
 *  1 | 1 | s:+ | ...       | off of 'xs' into _objects[1] | x | ? | ? | ? | 0      | *
 *    +-----------------------------------------------------------------------------+
 *
 *    _objects
 *    +-----------------------------------------------------------------------------+
 *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
 *    +-----------------------------------------------------------------------------+
 *
 *    _fieldNames
 *    +-----------------------------------------------------------------------------+
 *    | \0                                                                          |
 *    +-----------------------------------------------------------------------------+
 *
 *    _leafBuf
 *    +-----------------------------------------------------------------------------+
 *    | {}                                                                          |
 *    +-----------------------------------------------------------------------------+


    mmb::Element ys = xs.rightSibling();

 *    _elements
 *      oi      flags                offset                  ls  rs  lc  rc  p
 *    +-----------------------------------------------------------------------------+
 *  0 | 1 | s:- | ...       | off 0       into _fieldNames | x | x | 1 | ? | x      |
 *  1 | 1 | s:+ | ...       | off of 'xs' into _objects[1] | x | 2 | ? | ? | 0      | *
 *  2 | 1 | s:+ | ...       | off of 'ys' into _objects[1] | 1 | ? | ? | ? | 0      | *
 *    +-----------------------------------------------------------------------------+
 *
 *    _objects
 *    +-----------------------------------------------------------------------------+
 *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
 *    +-----------------------------------------------------------------------------+
 *
 *    _fieldNames
 *    +-----------------------------------------------------------------------------+
 *    | \0                                                                          |
 *    +-----------------------------------------------------------------------------+
 *
 *    _leafBuf
 *    +-----------------------------------------------------------------------------+
 *    | {}                                                                          |
 *    +-----------------------------------------------------------------------------+


    mmb::Element dne = ys.rightSibling();

 *    _elements
 *      oi      flags                offset                  ls  rs  lc  rc  p
 *    +-----------------------------------------------------------------------------+
 *  0 | 1 | s:- | ...       | off 0       into _fieldNames | x | x | 1 | 2 | x      | *
 *  1 | 1 | s:+ | ...       | off of 'xs' into _objects[1] | x | 2 | ? | ? | 0      |
 *  2 | 1 | s:+ | ...       | off of 'ys' into _objects[1] | 1 | x | ? | ? | 0      | *
 *    +-----------------------------------------------------------------------------+
 *
 *    _objects
 *    +-----------------------------------------------------------------------------+
 *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
 *    +-----------------------------------------------------------------------------+
 *
 *    _fieldNames
 *    +-----------------------------------------------------------------------------+
 *    | \0                                                                          |
 *    +-----------------------------------------------------------------------------+
 *
 *    _leafBuf
 *    +-----------------------------------------------------------------------------+
 *    | {}                                                                          |
 *    +-----------------------------------------------------------------------------+


    mmb::Element ycaps = doc.makeElementString("Y", "Y");

 *    _elements
 *      oi      flags                offset                  ls  rs  lc  rc  p
 *    +-----------------------------------------------------------------------------+
 *  0 | 1 | s:- | ...       | off 0       into _fieldNames | x | x | 1 | 2 | x      |
 *  1 | 1 | s:+ | ...       | off of 'xs' into _objects[1] | x | 2 | ? | ? | 0      |
 *  2 | 1 | s:+ | ...       | off of 'ys' into _objects[1] | 1 | x | ? | ? | 0      |
 *  3 | 0 | s:+ | ...       | off of 'Y'  into _objects[0] | x | x | x | x | x      | *
 *    +-----------------------------------------------------------------------------+
 *
 *    _objects
 *    +-----------------------------------------------------------------------------+
 *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
 *    +-----------------------------------------------------------------------------+
 *
 *    _fieldNames
 *    +-----------------------------------------------------------------------------+
 *    | \0                                                                          |
 *    +-----------------------------------------------------------------------------+
 *
 *    _leafBuf
 *    +-----------------------------------------------------------------------------+
 *    | { "Y" : "Y" }                                                               | *
 *    +-----------------------------------------------------------------------------+


    ys.pushBack(ycaps);

 *    _elements
 *      oi      flags                offset                    ls  rs  lc  rc  p
 *    +-----------------------------------------------------------------------------+
 *  0 | 1 | s:- | ...       | off 0         into _fieldNames | x | x | 1 | 2 | x    |
 *  1 | 1 | s:+ | ...       | off of 'xs'   into _objects[1] | x | 2 | ? | ? | 0    |
 *  2 | 1 | s:- | ...       | off of 'ys'   into _objects[1] | 1 | x | 4 | 3 | 0    | *
 *  3 | 0 | s:+ | ...       | off of 'Y'    into _objects[0] | 4 | x | x | x | 2    | *
 *  4 | 1 | s:+ | ...       | off of 'ys.y' into _objects[1] | x | 3 | x | x | 2    | *
 *    +-----------------------------------------------------------------------------+
 *
 *    _objects
 *    +-----------------------------------------------------------------------------+
 *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
 *    +-----------------------------------------------------------------------------+
 *
 *    _fieldNames
 *    +-----------------------------------------------------------------------------+
 *    | \0                                                                          |
 *    +-----------------------------------------------------------------------------+
 *
 *    _leafBuf
 *    +-----------------------------------------------------------------------------+
 *    | { "Y" : "Y" }                                                               |
 *    +-----------------------------------------------------------------------------+


    mmb::Element pun = doc.makeElementArray("why");

 *    _elements
 *      oi      flags                offset                     ls  rs  lc  rc  p
 *    +-----------------------------------------------------------------------------+
 *  0 | 1  | s:- | ...       | off 0         into _fieldNames | x | x | 1 | 2 | x   |
 *  1 | 1  | s:+ | ...       | off of 'xs'   into _objects[1] | x | 2 | ? | ? | 0   |
 *  2 | 1  | s:- | ...       | off of 'ys'   into _objects[1] | 1 | x | 4 | 3 | 0   |
 *  3 | 0  | s:+ | ...       | off of 'Y'    into _objects[0] | 4 | x | x | x | 2   |
 *  4 | 1  | s:+ | ...       | off of 'ys.y' into _objects[1] | x | 3 | x | x | 2   |
 *  5 | -1 | s:- | a:+ | ... | off of 'why'  into _fieldNames | x | x | x | x | x   | *
 *    +-----------------------------------------------------------------------------+
 *
 *    _objects
 *    +-----------------------------------------------------------------------------+
 *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
 *    +-----------------------------------------------------------------------------+
 *
 *    _fieldNames
 *    +-----------------------------------------------------------------------------+
 *    | \0why\0                                                                     | *
 *    +-----------------------------------------------------------------------------+
 *
 *    _leafBuf
 *    +-----------------------------------------------------------------------------+
 *    | { "Y" : "Y" }                                                               |
 *    +-----------------------------------------------------------------------------+


    ys.pushBack(pun);

 *    _elements
 *      oi      flags                offset                     ls  rs  lc  rc  p
 *    +-----------------------------------------------------------------------------+
 *  0 | 1  | s:- | ...       | off 0         into _fieldNames | x | x | 1 | 2 | x   |
 *  1 | 1  | s:+ | ...       | off of 'xs'   into _objects[1] | x | 2 | ? | ? | 0   |
 *  2 | 1  | s:- | ...       | off of 'ys'   into _objects[1] | 1 | x | 4 | 5 | 0   | *
 *  3 | 0  | s:+ | ...       | off of 'Y'    into _objects[0] | 4 | 5 | x | x | 2   | *
 *  4 | 1  | s:+ | ...       | off of 'ys.y' into _objects[1] | x | 3 | x | x | 2   |
 *  5 | -1 | s:- | a:+ | ... | off of 'why'  into _fieldNames | 3 | x | x | x | 2   | *
 *    +-----------------------------------------------------------------------------+
 *
 *    _objects
 *    +-----------------------------------------------------------------------------+
 *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
 *    +-----------------------------------------------------------------------------+
 *
 *    _fieldNames
 *    +-----------------------------------------------------------------------------+
 *    | \0why\0                                                                     |
 *    +-----------------------------------------------------------------------------+
 *
 *    _leafBuf
 *    +-----------------------------------------------------------------------------+
 *    | { "Y" : "Y" }                                                               |
 *    +-----------------------------------------------------------------------------+


    pun.appendString("na", "not");

 *    _elements
 *      oi      flags                offset                     ls  rs  lc  rc  p
 *    +-----------------------------------------------------------------------------+
 *  0 | 1  | s:- | ...       | off 0         into _fieldNames | x | x | 1 | 2 | x   |
 *  1 | 1  | s:+ | ...       | off of 'xs'   into _objects[1] | x | 2 | ? | ? | 0   |
 *  2 | 1  | s:- | ...       | off of 'ys'   into _objects[1] | 1 | x | 4 | 5 | 0   |
 *  3 | 0  | s:+ | ...       | off of 'Y'    into _objects[0] | 4 | 5 | x | x | 2   |
 *  4 | 1  | s:+ | ...       | off of 'ys.y' into _objects[1] | x | 3 | x | x | 2   |
 *  5 | -1 | s:- | a:+ | ... | off of 'why'  into _fieldNames | 3 | x | 6 | 6 | 2   | *
 *  6 | 0  | s:+ | ...       | off of 'na'   into _objects[0] | x | x | x | x | 5   | *
 *    +-----------------------------------------------------------------------------+
 *
 *    _objects
 *    +-----------------------------------------------------------------------------+
 *    | BSONObj for _leafBuilder | BSONObj for inObj |                              |
 *    +-----------------------------------------------------------------------------+
 *
 *    _fieldNames
 *    +-----------------------------------------------------------------------------+
 *    | \0why\0                                                                     |
 *    +-----------------------------------------------------------------------------+
 *
 *    _leafBuf
 *    +-----------------------------------------------------------------------------+
 *    | { "Y" : "Y", "na" : "not" }                                                 | *
 *    +-----------------------------------------------------------------------------+
 *
 */

// Work around http://gcc.gnu.org/bugzilla/show_bug.cgi?id=29365. Note that the selection of
// minor version 4 is somewhat arbitrary. It does appear that the fix for this was backported
// to earlier versions. This is a conservative choice that we can revisit later. We need the
// __clang__ here because Clang claims to be gcc of some version.
#if defined(__clang__) || !defined(__GNUC__) || (__GNUC__ > 4) || \
    (__GNUC__ == 4 && __GNUC_MINOR__ >= 4)
namespace {
#endif

// The designated field name for the root element.
constexpr auto kRootFieldName = ""_sd;

// How many reps do we cache before we spill to heap. Use a power of two. For debug
// builds we make this very small so it is less likely to mask vector invalidation
// logic errors. We don't make it zero so that we do execute the fastRep code paths.
const size_t kFastReps = kDebugBuild ? 2 : 128;

// An ElementRep contains the information necessary to locate the data for an Element,
// and the topology information for how the Element is related to other Elements in the
// document.
#pragma pack(push, 1)
struct ElementRep {
    // Builds an ElementRep in its correct default state. This is used instead of a default
    // constructor or NSDMIs to ensure that this type stays trivial so that vectors of it are cheap
    // to grow.
    static ElementRep makeDefaultRep();

    // The index of the BSONObj that provides the value for this Element. For nodes
    // where serialized is 'false', this value may be kInvalidObjIdx to indicate that
    // the Element does not have a supporting BSONObj.
    typedef uint16_t ObjIdx;
    ObjIdx objIdx;

    // This bit is true if this ElementRep identifies a completely serialized
    // BSONElement (i.e. a region of memory with a bson type byte, a fieldname, and an
    // encoded value). Changes to children of a serialized element will cause it to be
    // marked as unserialized.
    uint16_t serialized : 1;

    // For object like Elements where we cannot determine the type of the object by
    // looking a region of memory, the 'array' bit allows us to determine whether we
    // are an object or an array.
    uint16_t array : 1;

    // Reserved for future use.
    uint16_t reserved : 14;

    // This word either gives the offset into the BSONObj associated with this
    // ElementRep where this serialized BSON element may be located, or the offset into
    // the _fieldNames member of the Document where the field name for this BSON
    // element may be located.
    uint32_t offset;

    // The indexes of our left and right siblings in the Document.
    struct {
        Element::RepIdx left;
        Element::RepIdx right;
    } sibling;

    // The indexes of our left and right chidren in the Document.
    struct {
        Element::RepIdx left;
        Element::RepIdx right;
    } child;

    // The index of our parent in the Document.
    Element::RepIdx parent;

    // The size of the field name and the total element size are cached to allow quickly
    // constructing a BSONElement object. These fields are private (even though the rest of this
    // struct is public) because of the somewhat complex requirements to update and use them
    // correctly.
    void setFieldNameSizeAndTotalSize(int fieldNameSize, int totalSize) {
        _fieldNameSize = fieldNameSize <= std::numeric_limits<int16_t>::max() ? fieldNameSize : -1;
        _totalSize = totalSize <= std::numeric_limits<int16_t>::max() ? totalSize : -1;
    }

    BSONElement toSerializedElement(const BSONObj& holder) const {
        return BSONElement(holder.objdata() + offset,  //
                           _fieldNameSize,
                           _totalSize);
    }

private:
    // The cached sizes for this element, or -1 if unknown or too big to fit.
    // TODO consider putting _fieldNameSize in the reserved bit field above to allow larger total
    // sizes to be cached. Alternatively, could use an 8/24 split uint32_t. Since BSONObj is limited
    // to just over 16MB, that will cover all practical element sizes. For now, this is fine since
    // computing the size is a trivial cost when working with elements larger 32KB.
    int16_t _fieldNameSize;
    int16_t _totalSize;
};
#pragma pack(pop)

MONGO_STATIC_ASSERT(sizeof(ElementRep) == 32);

// We want ElementRep to be a POD so Document::Impl can grow the std::vector with
// memmove.
//
MONGO_STATIC_ASSERT(std::is_trivial<ElementRep>::value);

// The ElementRep for the root element is always zero.
const Element::RepIdx kRootRepIdx = Element::RepIdx(0);

// This is the object index for elements in the leaf heap.
const ElementRep::ObjIdx kLeafObjIdx = ElementRep::ObjIdx(0);

// This is the sentinel value to indicate that we have no supporting BSONObj.
const ElementRep::ObjIdx kInvalidObjIdx = ElementRep::ObjIdx(-1);

// This is the highest valid object index that does not overlap sentinel values.
const ElementRep::ObjIdx kMaxObjIdx = ElementRep::ObjIdx(-2);

ElementRep ElementRep::makeDefaultRep() {
    ElementRep out;
    out.objIdx = kInvalidObjIdx;
    out.serialized = false;
    out.array = false;
    out.reserved = 0;
    out.offset = 0;
    out.sibling = {Element::kInvalidRepIdx, Element::kInvalidRepIdx};
    out.child = {Element::kInvalidRepIdx, Element::kInvalidRepIdx};
    out.parent = Element::kInvalidRepIdx;
    out._fieldNameSize = -1;
    out._totalSize = -1;
    return out;
}

// Returns the offset of 'elt' within 'object' as a uint32_t. The element must be part
// of the object or the behavior is undefined.
uint32_t getElementOffset(const BSONObj& object, const BSONElement& elt) {
    dassert(!elt.eoo());
    const char* const objRaw = object.objdata();
    const char* const eltRaw = elt.rawdata();
    dassert(objRaw < eltRaw);
    dassert(eltRaw < objRaw + object.objsize());
    dassert(eltRaw + elt.size() <= objRaw + object.objsize());
    const ptrdiff_t offset = eltRaw - objRaw;
    // BSON documents express their size as an int32_t so we should always be able to
    // express the offset as a uint32_t.
    invariant(offset > 0);
    invariant(offset <= std::numeric_limits<int32_t>::max());
    return offset;
}

// Returns true if this ElementRep is 'detached' from all other elements and can be
// added as a child, which helps ensure that we maintain a tree rather than a graph
// when adding new elements to the tree. The root element is never considered to be
// attachable.
bool canAttach(const Element::RepIdx id, const ElementRep& rep) {
    return (id != kRootRepIdx) && (rep.sibling.left == Element::kInvalidRepIdx) &&
        (rep.sibling.right == Element::kInvalidRepIdx) && (rep.parent == Element::kInvalidRepIdx);
}

// Returns a Status describing why 'canAttach' returned false. This function should not
// be inlined since it just makes the callers larger for no real gain.
MONGO_COMPILER_NOINLINE Status getAttachmentError(const ElementRep& rep);
Status getAttachmentError(const ElementRep& rep) {
    if (rep.sibling.left != Element::kInvalidRepIdx)
        return Status(ErrorCodes::IllegalOperation, "dangling left sibling");
    if (rep.sibling.right != Element::kInvalidRepIdx)
        return Status(ErrorCodes::IllegalOperation, "dangling right sibling");
    if (rep.parent != Element::kInvalidRepIdx)
        return Status(ErrorCodes::IllegalOperation, "dangling parent");
    return Status(ErrorCodes::IllegalOperation, "cannot add the root as a child");
}


// Enable paranoid mode to force a reallocation on mutation of the princple data
// structures in Document::Impl. This is really slow, but can be very helpful if you
// suspect an invalidation logic error and want to find it with valgrind. Paranoid mode
// only works in debug mode; it is ignored in release builds.
const bool paranoid = false;

#if defined(__clang__) || !defined(__GNUC__) || (__GNUC__ > 4) || \
    (__GNUC__ == 4 && __GNUC_MINOR__ >= 4)
}  // namespace
#endif

/** Document::Impl holds the Document state. Please see the file comment above for details
 *  on the fields of Impl and how they are used to realize the implementation of mutable
 *  BSON. Impl provides various utility methods to insert, lookup, and interrogate the
 *  Elements, BSONObj objects, field names, and builders associated with the Document.
 *
 *  TODO: At some point, we could remove the firewall and inline the members of Impl into
 *  Document.
 */
class Document::Impl {
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

public:
    Impl(Document::InPlaceMode inPlaceMode)
        : _numElements(0),
          _slowElements(),
          _objects(),
          _fieldNames(),
          _leafBuf(),
          _leafBuilder(_leafBuf),
          _fieldNameScratch(),
          _damages(),
          _inPlaceMode(inPlaceMode) {
        // We always have a BSONObj for the leaves, and we often have
        // one for our base document, so reserve 2.
        _objects.reserve(2);

        // We always have at least one byte for the root field name, and we would like
        // to be able to hold a few short field names without reallocation.
        _fieldNames.reserve(8);

        // We need an object at _objects[0] so that we can access leaf elements we
        // construct with the leaf builder in the same way we access elements serialized in
        // other BSONObjs. So we call asTempObj on the builder and store the result in slot
        // 0.
        dassert(_objects.size() == kLeafObjIdx);
        _objects.push_back(_leafBuilder.asTempObj());
        dassert(_leafBuf.len() != 0);
    }

    ~Impl() {
        _leafBuilder.abandon();
    }

    void reset(Document::InPlaceMode inPlaceMode) {
        // Clear out the state in the vectors.
        _slowElements.clear();
        _numElements = 0;

        _objects.clear();
        _fieldNames.clear();

        // There is no way to reset the state of a BSONObjBuilder, so we need to call its
        // dtor, reset the underlying buf, and re-invoke the constructor in-place.
        _leafBuilder.abandon();
        _leafBuilder.~BSONObjBuilder();
        _leafBuf.reset();
        new (&_leafBuilder) BSONObjBuilder(_leafBuf);

        _fieldNameScratch.clear();
        _damages.clear();
        _inPlaceMode = inPlaceMode;

        // Ensure that we start in the same state as the ctor would leave us in.
        _objects.push_back(_leafBuilder.asTempObj());
    }

    // Obtain the ElementRep for the given rep id.
    ElementRep& getElementRep(Element::RepIdx id) {
        return const_cast<ElementRep&>(const_cast<const Impl*>(this)->getElementRep(id));
    }

    // Obtain the ElementRep for the given rep id.
    const ElementRep& getElementRep(Element::RepIdx id) const {
        dassert(id < _numElements);
        if (id < kFastReps)
            return _fastElements[id];
        else
            return _slowElements[id - kFastReps];
    }

    // Construct and return a new default initialized ElementRep. The RepIdx identifying
    // the new rep is returned in the out parameter.
    ElementRep& makeNewRep(Element::RepIdx* newIdx) {
        const Element::RepIdx id = *newIdx = _numElements++;

        if (id < kFastReps) {
            return _fastElements[id] = ElementRep::makeDefaultRep();
        } else {
            invariant(id <= Element::kMaxRepIdx);

            if (kDebugBuild && paranoid) {
                // Force all reps to new addresses to help catch invalid rep usage.
                std::vector<ElementRep> newSlowElements(_slowElements);
                _slowElements.swap(newSlowElements);
            }

            return *_slowElements.insert(_slowElements.end(), ElementRep::makeDefaultRep());
        }
    }

    // Insert a new ElementRep for a leaf element at the given offset and return its ID.
    Element::RepIdx insertLeafElement(int offset, int fieldNameSize = -1, int totalSize = -1) {
        // BufBuilder hands back sizes in 'int's.
        Element::RepIdx inserted;
        ElementRep& rep = makeNewRep(&inserted);

        rep.setFieldNameSizeAndTotalSize(fieldNameSize, totalSize);
        rep.objIdx = kLeafObjIdx;
        rep.serialized = true;
        dassert(offset >= 0);
        // TODO: Is this a legitimate possibility?
        dassert(static_cast<unsigned int>(offset) < std::numeric_limits<uint32_t>::max());
        rep.offset = offset;
        _objects[kLeafObjIdx] = _leafBuilder.asTempObj();
        return inserted;
    }

    // Obtain the object builder for the leaves.
    BSONObjBuilder& leafBuilder() {
        return _leafBuilder;
    }

    // Obtain the BSONObj for the given object id.
    BSONObj& getObject(ElementRep::ObjIdx objIdx) {
        dassert(objIdx < _objects.size());
        return _objects[objIdx];
    }

    // Obtain the BSONObj for the given object id.
    const BSONObj& getObject(ElementRep::ObjIdx objIdx) const {
        dassert(objIdx < _objects.size());
        return _objects[objIdx];
    }

    // Insert the given BSONObj and return an ID for it.
    ElementRep::ObjIdx insertObject(const BSONObj& newObj) {
        const size_t objIdx = _objects.size();
        invariant(objIdx <= kMaxObjIdx);
        _objects.push_back(newObj);
        if (kDebugBuild && paranoid) {
            // Force reallocation to catch use after invalidation.
            std::vector<BSONObj> new_objects(_objects);
            _objects.swap(new_objects);
        }
        return objIdx;
    }

    // Given a RepIdx, return the BSONElement that it represents.
    BSONElement getSerializedElement(const ElementRep& rep) const {
        return rep.toSerializedElement(getObject(rep.objIdx));
    }

    // A helper method that either inserts the field name into the field name heap and
    // updates element.
    void insertFieldName(ElementRep& rep, StringData fieldName) {
        dassert(!rep.serialized);
        rep.offset = insertFieldName(fieldName);
    }

    // Retrieve the fieldName, given a rep.
    StringData getFieldName(const ElementRep& rep) const {
        // The root element has no field name.
        if (&rep == &getElementRep(kRootRepIdx))
            return StringData();

        if (rep.serialized || (rep.objIdx != kInvalidObjIdx))
            return getSerializedElement(rep).fieldNameStringData();

        return getFieldName(rep.offset);
    }

    StringData getFieldNameForNewElement(const ElementRep& rep) {
        StringData result = getFieldName(rep);
        if (rep.objIdx == kLeafObjIdx) {
            _fieldNameScratch.assign(result.rawData(), result.size());
            result = StringData(_fieldNameScratch);
        }
        return result;
    }

    // Retrieve the type, given a rep.
    BSONType getType(const ElementRep& rep) const {
        // The root element is always an Object.
        if (&rep == &getElementRep(kRootRepIdx))
            return mongo::Object;

        if (rep.serialized || (rep.objIdx != kInvalidObjIdx))
            return getSerializedElement(rep).type();

        return rep.array ? mongo::Array : mongo::Object;
    }

    static bool isLeafType(BSONType type) {
        return ((type != mongo::Object) && (type != mongo::Array));
    }

    // Returns true if rep is not an object or array.
    bool isLeaf(const ElementRep& rep) const {
        return isLeafType(getType(rep));
    }

    bool isLeaf(const BSONElement& elt) const {
        return isLeafType(elt.type());
    }

    // Returns true if rep's value can be provided as a BSONElement.
    bool hasValue(const ElementRep& rep) const {
        // The root element may be marked serialized, but it doesn't have a BSONElement
        // representation.
        if (&rep == &getElementRep(kRootRepIdx))
            return false;

        return rep.serialized;
    }

    // Return the index of the left child of the Element with index 'index', resolving the
    // left child to a realized Element if it is currently opaque. This may also cause the
    // parent elements child.right entry to be updated.
    Element::RepIdx resolveLeftChild(Element::RepIdx index) {
        dassert(index != Element::kInvalidRepIdx);
        dassert(index != Element::kOpaqueRepIdx);

        // If the left child is anything other than opaque, then we are done here.
        ElementRep* rep = &getElementRep(index);
        if (rep->child.left != Element::kOpaqueRepIdx)
            return rep->child.left;

        // It should be impossible to have an opaque left child and be non-serialized,
        dassert(rep->serialized);
        BSONElement childElt =
            (hasValue(*rep) ? getSerializedElement(*rep).embeddedObject() : getObject(rep->objIdx))
                .firstElement();

        if (!childElt.eoo()) {
            // Do this now before other writes so compiler can exploit knowing
            // that we are not eoo.
            const int32_t fieldNameSize = childElt.fieldNameSize();
            const int32_t totalSize = childElt.size();

            Element::RepIdx inserted;
            ElementRep& newRep = makeNewRep(&inserted);
            // Calling makeNewRep invalidates rep since it may cause a reallocation of
            // the element vector. After calling insertElement, we reacquire rep.
            rep = &getElementRep(index);

            newRep.serialized = true;
            newRep.objIdx = rep->objIdx;
            newRep.offset = getElementOffset(getObject(rep->objIdx), childElt);
            newRep.parent = index;
            newRep.sibling.right = Element::kOpaqueRepIdx;
            // If this new object has possible substructure, mark its children as opaque.
            if (!isLeaf(childElt)) {
                newRep.child.left = Element::kOpaqueRepIdx;
                newRep.child.right = Element::kOpaqueRepIdx;
            }
            newRep.setFieldNameSizeAndTotalSize(fieldNameSize, totalSize);
            rep->child.left = inserted;
        } else {
            rep->child.left = Element::kInvalidRepIdx;
            rep->child.right = Element::kInvalidRepIdx;
        }

        dassert(rep->child.left != Element::kOpaqueRepIdx);
        return rep->child.left;
    }

    // Return the index of the right child of the Element with index 'index', resolving any
    // opaque nodes. Note that this may require resolving all of the right siblings of the
    // left child.
    Element::RepIdx resolveRightChild(Element::RepIdx index) {
        dassert(index != Element::kInvalidRepIdx);
        dassert(index != Element::kOpaqueRepIdx);

        Element::RepIdx current = getElementRep(index).child.right;
        if (current == Element::kOpaqueRepIdx) {
            current = resolveLeftChild(index);
            while (current != Element::kInvalidRepIdx) {
                Element::RepIdx next = resolveRightSibling(current);
                if (next == Element::kInvalidRepIdx)
                    break;
                current = next;
            }

            // The resolveRightSibling calls should have eventually updated this nodes right
            // child pointer to point to the node we are about to return.
            dassert(getElementRep(index).child.right == current);
        }

        return current;
    }

    // Return the index of the right sibling of the Element with index 'index', resolving
    // the right sibling to a realized Element if it is currently opaque.
    Element::RepIdx resolveRightSibling(Element::RepIdx index) {
        dassert(index != Element::kInvalidRepIdx);
        dassert(index != Element::kOpaqueRepIdx);

        // If the right sibling is anything other than opaque, then we are done here.
        ElementRep* rep = &getElementRep(index);
        if (rep->sibling.right != Element::kOpaqueRepIdx)
            return rep->sibling.right;

        BSONElement elt = getSerializedElement(*rep);
        BSONElement rightElt(elt.rawdata() + elt.size());

        if (!rightElt.eoo()) {
            // Do this now before other writes so compiler can exploit knowing
            // that we are not eoo.
            const int32_t fieldNameSize = rightElt.fieldNameSize();
            const int32_t totalSize = rightElt.size();

            Element::RepIdx inserted;
            ElementRep& newRep = makeNewRep(&inserted);
            // Calling makeNewRep invalidates rep since it may cause a reallocation of
            // the element vector. After calling insertElement, we reacquire rep.
            rep = &getElementRep(index);

            newRep.serialized = true;
            newRep.objIdx = rep->objIdx;
            newRep.offset = getElementOffset(getObject(rep->objIdx), rightElt);
            newRep.parent = rep->parent;
            newRep.sibling.left = index;
            newRep.sibling.right = Element::kOpaqueRepIdx;
            // If this new object has possible substructure, mark its children as opaque.
            if (!isLeaf(rightElt)) {
                newRep.child.left = Element::kOpaqueRepIdx;
                newRep.child.right = Element::kOpaqueRepIdx;
            }
            newRep.setFieldNameSizeAndTotalSize(fieldNameSize, totalSize);
            rep->sibling.right = inserted;
        } else {
            rep->sibling.right = Element::kInvalidRepIdx;
            // If we have found the end of this object, then our (necessarily existing)
            // parent's necessarily opaque right child is now determined to be us.
            dassert(rep->parent <= Element::kMaxRepIdx);
            ElementRep& parentRep = getElementRep(rep->parent);
            dassert(parentRep.child.right == Element::kOpaqueRepIdx);
            parentRep.child.right = index;
        }

        dassert(rep->sibling.right != Element::kOpaqueRepIdx);
        return rep->sibling.right;
    }

    // Find the ElementRep at index 'index', and mark it and all of its currently
    // serialized parents as non-serialized.
    void deserialize(Element::RepIdx index) {
        while (index != Element::kInvalidRepIdx) {
            ElementRep& rep = getElementRep(index);
            // It does not make sense for leaf Elements to become deserialized, and
            // requests to do so indicate a bug in the implementation of the library.
            dassert(!isLeaf(rep));
            if (!rep.serialized)
                break;
            rep.serialized = false;
            index = rep.parent;
        }
    }

    inline bool doesNotAlias(StringData s) const {
        // StringData may come from either the field name heap or the leaf builder.
        return doesNotAliasLeafBuilder(s) && doesNotAliasFieldNameHeap(s);
    }

    inline bool doesNotAliasFieldNameHeap(StringData s) const {
        return !inFieldNameHeap(s.rawData());
    }

    inline bool doesNotAliasLeafBuilder(StringData s) const {
        return !inLeafBuilder(s.rawData());
    }

    inline bool doesNotAlias(const BSONElement& e) const {
        // A BSONElement could alias the leaf builder.
        return !inLeafBuilder(e.rawdata());
    }

    inline bool doesNotAlias(const BSONObj& o) const {
        // A BSONObj could alias the leaf buildr.
        return !inLeafBuilder(o.objdata());
    }

    // Returns true if 'data' points within the leaf BufBuilder.
    inline bool inLeafBuilder(const char* data) const {
        // TODO: Write up something documenting that the following is technically UB due
        // to illegality of comparing pointers to different aggregates for ordering. Also,
        // do we need to do anything to prevent the optimizer from compiling this out on
        // that basis? I've seen clang do that. We may need to declare these volatile. On
        // the other hand, these should only be being called under a dassert, so the
        // optimizer is maybe not in play, and the UB is unlikely to be a problem in
        // practice.
        const char* const start = _leafBuf.buf();
        const char* const end = start + _leafBuf.len();
        return (data >= start) && (data < end);
    }

    // Returns true if 'data' points within the field name heap.
    inline bool inFieldNameHeap(const char* data) const {
        if (_fieldNames.empty())
            return false;
        const char* const start = &_fieldNames.front();
        const char* const end = &_fieldNames.back();
        return (data >= start) && (data < end);
    }

    void reserveDamageEvents(size_t expectedEvents) {
        _damages.reserve(expectedEvents);
    }

    bool getInPlaceUpdates(DamageVector* damages, const char** source, size_t* size) {
        // If some operations were not in-place, set source to NULL and return false to
        // inform upstream that we are not returning in-place result data.
        if (_inPlaceMode == Document::kInPlaceDisabled) {
            damages->clear();
            *source = nullptr;
            if (size)
                *size = 0;
            return false;
        }

        // Set up the source and source size out parameters.
        *source = _objects[0].objdata();
        if (size)
            *size = _objects[0].objsize();

        // Swap our damage event queue with upstream, and reset ours to an empty vector. In
        // princple, we can do another round of in-place updates.
        damages->swap(_damages);
        _damages.clear();

        return true;
    }

    void disableInPlaceUpdates() {
        _inPlaceMode = Document::kInPlaceDisabled;
    }

    Document::InPlaceMode getCurrentInPlaceMode() const {
        return _inPlaceMode;
    }

    bool isInPlaceModeEnabled() const {
        return getCurrentInPlaceMode() == Document::kInPlaceEnabled;
    }

    void recordDamageEvent(DamageEvent::OffsetSizeType targetOffset,
                           DamageEvent::OffsetSizeType sourceOffset,
                           size_t size) {
        _damages.push_back(DamageEvent());
        _damages.back().targetOffset = targetOffset;
        _damages.back().targetSize = size;
        _damages.back().sourceOffset = sourceOffset;
        _damages.back().sourceSize = size;
        if (kDebugBuild && paranoid) {
            // Force damage events to new addresses to catch invalidation errors.
            DamageVector new_damages(_damages);
            _damages.swap(new_damages);
        }
    }

    // Check all preconditions on doing an in-place update, except for size match.
    bool canUpdateInPlace(const ElementRep& sourceRep, const ElementRep& targetRep) {
        // NOTE: CodeWScope might arguably be excluded since it has substructure, but
        // mutable doesn't permit navigation into its document, so we can handle it.

        // We can only do an in-place update to an element that is serialized and is not in
        // the leaf heap.
        //
        // TODO: In the future, we can replace values in the leaf heap if they are of the
        // same size as the origin was. For now, we don't support that.
        if (!hasValue(targetRep) || (targetRep.objIdx == kLeafObjIdx))
            return false;

        // sourceRep should be newly created, so it must have a value representation.
        dassert(hasValue(sourceRep));

        // For a target that has substructure, we only permit in-place updates if there
        // cannot be ElementReps that reference data within the target. We don't need to
        // worry about ElementReps for source, since it is newly created. The only way
        // there can be ElementReps referring into substructure is if the Element has
        // non-empty non-opaque child references.
        if (!isLeaf(targetRep)) {
            if (((targetRep.child.left != Element::kOpaqueRepIdx) &&
                 (targetRep.child.left != Element::kInvalidRepIdx)) ||
                ((targetRep.child.right != Element::kOpaqueRepIdx) &&
                 (targetRep.child.right != Element::kInvalidRepIdx)))
                return false;
        }

        return true;
    }

    template <typename Builder>
    void writeElement(Element::RepIdx repIdx,
                      Builder* builder,
                      const StringData* fieldName = nullptr) const;

    template <typename Builder>
    void writeChildren(Element::RepIdx repIdx, Builder* builder) const;

private:
    // Insert the given field name into the field name heap, and return an ID for this
    // field name.
    int32_t insertFieldName(StringData fieldName) {
        const uint32_t id = _fieldNames.size();
        if (!fieldName.empty())
            _fieldNames.insert(
                _fieldNames.end(), fieldName.rawData(), fieldName.rawData() + fieldName.size());
        _fieldNames.push_back('\0');
        if (kDebugBuild && paranoid) {
            // Force names to new addresses to catch invalidation errors.
            std::vector<char> new_fieldNames(_fieldNames);
            _fieldNames.swap(new_fieldNames);
        }
        return id;
    }

    // Retrieve the field name with the given id.
    StringData getFieldName(uint32_t fieldNameId) const {
        dassert(fieldNameId < _fieldNames.size());
        return &_fieldNames[fieldNameId];
    }

    size_t _numElements;
    ElementRep _fastElements[kFastReps];
    std::vector<ElementRep> _slowElements;

    std::vector<BSONObj> _objects;
    std::vector<char> _fieldNames;

    // We own a BufBuilder to avoid BSONObjBuilder's ref-count mechanism which would throw
    // off our offset calculations.
    BufBuilder _leafBuf;
    BSONObjBuilder _leafBuilder;

    // Sometimes, we need a temporary storage area for a fieldName, because the source of
    // the fieldName is in the same buffer that we want to write to, potentially
    // reallocating it. In such cases, we temporarily store the value here, rather than
    // creating and destroying a string and its buffer each time.
    std::string _fieldNameScratch;

    // Queue of damage events and status bit for whether in-place updates are possible.
    DamageVector _damages;
    Document::InPlaceMode _inPlaceMode;
};

Status Element::addSiblingLeft(Element e) {
    invariant(ok());
    invariant(e.ok());
    invariant(_doc == e._doc);

    Document::Impl& impl = getDocument().getImpl();
    ElementRep& newRep = impl.getElementRep(e._repIdx);

    // check that new element roots a clean subtree.
    if (!canAttach(e._repIdx, newRep))
        return getAttachmentError(newRep);

    ElementRep& thisRep = impl.getElementRep(_repIdx);

    dassert(thisRep.parent != kOpaqueRepIdx);
    if (thisRep.parent == kInvalidRepIdx)
        return Status(ErrorCodes::IllegalOperation,
                      "Attempt to add a sibling to an element without a parent");

    ElementRep& parentRep = impl.getElementRep(thisRep.parent);
    dassert(!impl.isLeaf(parentRep));

    impl.disableInPlaceUpdates();

    // The new element shares our parent.
    newRep.parent = thisRep.parent;

    // We are the new element's right sibling.
    newRep.sibling.right = _repIdx;

    // The new element's left sibling is our left sibling.
    newRep.sibling.left = thisRep.sibling.left;

    // If the new element has a left sibling after the adjustments above, then that left
    // sibling must be updated to have the new element as its right sibling.
    if (newRep.sibling.left != kInvalidRepIdx)
        impl.getElementRep(thisRep.sibling.left).sibling.right = e._repIdx;

    // The new element becomes our left sibling.
    thisRep.sibling.left = e._repIdx;

    // If we were our parent's left child, then we no longer are. Make the new right
    // sibling the right child.
    if (parentRep.child.left == _repIdx)
        parentRep.child.left = e._repIdx;

    impl.deserialize(thisRep.parent);

    return Status::OK();
}

Status Element::addSiblingRight(Element e) {
    invariant(ok());
    invariant(e.ok());
    invariant(_doc == e._doc);

    Document::Impl& impl = getDocument().getImpl();
    ElementRep* newRep = &impl.getElementRep(e._repIdx);

    // check that new element roots a clean subtree.
    if (!canAttach(e._repIdx, *newRep))
        return getAttachmentError(*newRep);

    ElementRep* thisRep = &impl.getElementRep(_repIdx);

    dassert(thisRep->parent != kOpaqueRepIdx);
    if (thisRep->parent == kInvalidRepIdx)
        return Status(ErrorCodes::IllegalOperation,
                      "Attempt to add a sibling to an element without a parent");

    ElementRep* parentRep = &impl.getElementRep(thisRep->parent);
    dassert(!impl.isLeaf(*parentRep));

    impl.disableInPlaceUpdates();

    // If our current right sibling is opaque it needs to be resolved. This will invalidate
    // our reps so we need to reacquire them.
    Element::RepIdx rightSiblingIdx = thisRep->sibling.right;
    if (rightSiblingIdx == kOpaqueRepIdx) {
        rightSiblingIdx = impl.resolveRightSibling(_repIdx);
        dassert(rightSiblingIdx != kOpaqueRepIdx);
        newRep = &impl.getElementRep(e._repIdx);
        thisRep = &impl.getElementRep(_repIdx);
        parentRep = &impl.getElementRep(thisRep->parent);
    }

    // The new element shares our parent.
    newRep->parent = thisRep->parent;

    // We are the new element's left sibling.
    newRep->sibling.left = _repIdx;

    // The new element right sibling is our right sibling.
    newRep->sibling.right = rightSiblingIdx;

    // The new element becomes our right sibling.
    thisRep->sibling.right = e._repIdx;

    // If the new element has a right sibling after the adjustments above, then that right
    // sibling must be updated to have the new element as its left sibling.
    if (newRep->sibling.right != kInvalidRepIdx)
        impl.getElementRep(rightSiblingIdx).sibling.left = e._repIdx;

    // If we were our parent's right child, then we no longer are. Make the new right
    // sibling the right child.
    if (parentRep->child.right == _repIdx)
        parentRep->child.right = e._repIdx;

    impl.deserialize(thisRep->parent);

    return Status::OK();
}

Status Element::remove() {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();

    // We need to realize any opaque right sibling, because we are going to need to set its
    // left sibling. Do this before acquiring thisRep since otherwise we would potentially
    // invalidate it.
    impl.resolveRightSibling(_repIdx);

    ElementRep& thisRep = impl.getElementRep(_repIdx);

    if (thisRep.parent == kInvalidRepIdx)
        return Status(ErrorCodes::IllegalOperation, "trying to remove a parentless element");
    impl.disableInPlaceUpdates();

    // If our right sibling is not the end of the object, then set its left sibling to be
    // our left sibling.
    if (thisRep.sibling.right != kInvalidRepIdx)
        impl.getElementRep(thisRep.sibling.right).sibling.left = thisRep.sibling.left;

    // Similarly, if our left sibling is not the beginning of the obejct, then set its
    // right sibling to be our right sibling.
    if (thisRep.sibling.left != kInvalidRepIdx) {
        ElementRep& leftRep = impl.getElementRep(thisRep.sibling.left);
        leftRep.sibling.right = thisRep.sibling.right;
    }

    // If this element was our parent's right child, then our left sibling is the new right
    // child.
    ElementRep& parentRep = impl.getElementRep(thisRep.parent);
    if (parentRep.child.right == _repIdx)
        parentRep.child.right = thisRep.sibling.left;

    // Similarly, if this element was our parent's left child, then our right sibling is
    // the new left child.
    if (parentRep.child.left == _repIdx)
        parentRep.child.left = thisRep.sibling.right;

    impl.deserialize(thisRep.parent);

    // The Element becomes detached.
    thisRep.parent = kInvalidRepIdx;
    thisRep.sibling.left = kInvalidRepIdx;
    thisRep.sibling.right = kInvalidRepIdx;

    return Status::OK();
}

Status Element::rename(StringData newName) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();

    if (_repIdx == kRootRepIdx)
        return Status(ErrorCodes::IllegalOperation,
                      "Invalid attempt to rename the root element of a document");

    dassert(impl.doesNotAlias(newName));

    // TODO: Some rename operations may be possible to do in-place.
    impl.disableInPlaceUpdates();

    // Operations below may invalidate thisRep, so we may need to reacquire it.
    ElementRep* thisRep = &impl.getElementRep(_repIdx);

    // For non-leaf serialized elements, we can realize any opaque relatives and then
    // convert ourselves to deserialized.
    if (thisRep->objIdx != kInvalidObjIdx && !impl.isLeaf(*thisRep)) {
        const bool array = (impl.getType(*thisRep) == mongo::Array);

        // Realize any opaque right sibling or left child now, since otherwise we will lose
        // the ability to do so.
        impl.resolveLeftChild(_repIdx);
        impl.resolveRightSibling(_repIdx);

        // The resolve calls above may have invalidated thisRep, we need to reacquire it.
        thisRep = &impl.getElementRep(_repIdx);

        // Set this up as a non-supported deserialized element. We will set the fieldName
        // in the else clause in the block below.
        impl.deserialize(_repIdx);

        thisRep->array = array;

        // TODO: If we ever want to be able to add to the left or right of an opaque object
        // without expanding, this may need to change.
        thisRep->objIdx = kInvalidObjIdx;
    }

    if (impl.hasValue(*thisRep)) {
        // For leaf elements we just create a new Element with the current value and
        // replace. Note that the 'setValue' call below will invalidate thisRep.
        Element replacement = _doc->makeElementWithNewFieldName(newName, *this);
        setValue(replacement._repIdx).transitional_ignore();
    } else {
        // The easy case: just update what our field name offset refers to.
        impl.insertFieldName(*thisRep, newName);
    }

    return Status::OK();
}

Element Element::leftChild() const {
    invariant(ok());

    // Capturing Document::Impl by non-const ref exploits the constness loophole
    // created by our Impl so that we can let leftChild be lazily evaluated, even for a
    // const Element.
    Document::Impl& impl = _doc->getImpl();
    const Element::RepIdx leftChildIdx = impl.resolveLeftChild(_repIdx);
    dassert(leftChildIdx != kOpaqueRepIdx);
    return Element(_doc, leftChildIdx);
}

Element Element::rightChild() const {
    invariant(ok());

    // Capturing Document::Impl by non-const ref exploits the constness loophole
    // created by our Impl so that we can let leftChild be lazily evaluated, even for a
    // const Element.
    Document::Impl& impl = _doc->getImpl();
    const Element::RepIdx rightChildIdx = impl.resolveRightChild(_repIdx);
    dassert(rightChildIdx != kOpaqueRepIdx);
    return Element(_doc, rightChildIdx);
}

bool Element::hasChildren() const {
    invariant(ok());
    // Capturing Document::Impl by non-const ref exploits the constness loophole
    // created by our Impl so that we can let leftChild be lazily evaluated, even for a
    // const Element.
    Document::Impl& impl = _doc->getImpl();
    return impl.resolveLeftChild(_repIdx) != kInvalidRepIdx;
}

Element Element::leftSibling(size_t distance) const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    Element::RepIdx current = _repIdx;
    while ((current != kInvalidRepIdx) && (distance-- != 0)) {
        // We are (currently) never left opaque, so don't need to resolve.
        current = impl.getElementRep(current).sibling.left;
    }
    return Element(_doc, current);
}

Element Element::rightSibling(size_t distance) const {
    invariant(ok());

    // Capturing Document::Impl by non-const ref exploits the constness loophole
    // created by our Impl so that we can let rightSibling be lazily evaluated, even for a
    // const Element.
    Document::Impl& impl = _doc->getImpl();
    Element::RepIdx current = _repIdx;
    while ((current != kInvalidRepIdx) && (distance-- != 0))
        current = impl.resolveRightSibling(current);
    return Element(_doc, current);
}

Element Element::parent() const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    const Element::RepIdx parentIdx = impl.getElementRep(_repIdx).parent;
    dassert(parentIdx != kOpaqueRepIdx);
    return Element(_doc, parentIdx);
}

Element Element::findNthChild(size_t n) const {
    invariant(ok());
    Document::Impl& impl = _doc->getImpl();
    Element::RepIdx current = _repIdx;
    current = impl.resolveLeftChild(current);
    while ((current != kInvalidRepIdx) && (n-- != 0))
        current = impl.resolveRightSibling(current);
    return Element(_doc, current);
}

Element Element::findFirstChildNamed(StringData name) const {
    invariant(ok());
    Document::Impl& impl = _doc->getImpl();
    invariant(getType() != BSONType::Array);
    Element::RepIdx current = _repIdx;
    current = impl.resolveLeftChild(current);
    // TODO: Could DRY this loop with the identical logic in findElementNamed.
    while ((current != kInvalidRepIdx) && (impl.getFieldName(impl.getElementRep(current)) != name))
        current = impl.resolveRightSibling(current);
    return Element(_doc, current);
}

Element Element::findElementNamed(StringData name) const {
    invariant(ok());
    Document::Impl& impl = _doc->getImpl();
    Element::RepIdx current = _repIdx;
    while ((current != kInvalidRepIdx) && (impl.getFieldName(impl.getElementRep(current)) != name))
        current = impl.resolveRightSibling(current);
    return Element(_doc, current);
}

size_t Element::countSiblingsLeft() const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    Element::RepIdx current = _repIdx;
    size_t result = 0;
    while (true) {
        // We are (currently) never left opaque, so don't need to resolve.
        current = impl.getElementRep(current).sibling.left;
        if (current == kInvalidRepIdx)
            break;
        ++result;
    }
    return result;
}

size_t Element::countSiblingsRight() const {
    invariant(ok());
    Document::Impl& impl = _doc->getImpl();
    Element::RepIdx current = _repIdx;
    size_t result = 0;
    while (true) {
        current = impl.resolveRightSibling(current);
        if (current == kInvalidRepIdx)
            break;
        ++result;
    }
    return result;
}

size_t Element::countChildren() const {
    invariant(ok());
    Document::Impl& impl = _doc->getImpl();
    Element::RepIdx current = _repIdx;
    current = impl.resolveLeftChild(current);
    size_t result = 0;
    while (current != kInvalidRepIdx) {
        ++result;
        current = impl.resolveRightSibling(current);
    }
    return result;
}

bool Element::hasValue() const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    return impl.hasValue(thisRep);
}

bool Element::isNumeric() const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const BSONType type = impl.getType(thisRep);
    return ((type == mongo::NumberLong) || (type == mongo::NumberInt) ||
            (type == mongo::NumberDouble) || (type == mongo::NumberDecimal));
}

bool Element::isIntegral() const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const BSONType type = impl.getType(thisRep);
    return ((type == mongo::NumberLong) || (type == mongo::NumberInt));
}

BSONElement Element::getValue() const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    if (impl.hasValue(thisRep))
        return impl.getSerializedElement(thisRep);
    return BSONElement();
}

SafeNum Element::getValueSafeNum() const {
    switch (getType()) {
        case mongo::NumberInt:
            return static_cast<int32_t>(getValueInt());
        case mongo::NumberLong:
            return static_cast<int64_t>(getValueLong());
        case mongo::NumberDouble:
            return getValueDouble();
        case mongo::NumberDecimal:
            return getValueDecimal();
        default:
            return SafeNum();
    }
}

int Element::compareWithElement(const ConstElement& other,
                                const StringDataComparator* comparator,
                                bool considerFieldName) const {
    invariant(ok());
    invariant(other.ok());

    // Short circuit a tautological compare.
    if ((_repIdx == other.getIdx()) && (_doc == &other.getDocument()))
        return 0;

    // If either Element can represent its current value as a BSONElement, then we can
    // obtain its value and use compareWithBSONElement. If both Elements have a
    // representation as a BSONElement, compareWithBSONElement will notice that the first
    // argument has a value and delegate to BSONElement::woCompare.

    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);

    // Subtle: we must negate the comparison result here because we are reversing the
    // argument order in this call.
    //
    // TODO: Andy has suggested that this may not be legal since woCompare is not reflexive
    // in all cases.
    if (impl.hasValue(thisRep))
        return -other.compareWithBSONElement(
            impl.getSerializedElement(thisRep), comparator, considerFieldName);

    const Document::Impl& oimpl = other.getDocument().getImpl();
    const ElementRep& otherRep = oimpl.getElementRep(other.getIdx());

    if (oimpl.hasValue(otherRep))
        return compareWithBSONElement(
            oimpl.getSerializedElement(otherRep), comparator, considerFieldName);

    // Leaf elements should always have a value, so we should only be dealing with Objects
    // or Arrays here.
    dassert(!impl.isLeaf(thisRep));
    dassert(!oimpl.isLeaf(otherRep));

    // Obtain the canonical types for this Element and the BSONElement, if they are
    // different use the difference as the result. Please see BSONElement::woCompare for
    // details. We know that thisRep is not a number, so we don't need to check that
    // particular case.
    const int leftCanonType = canonicalizeBSONType(impl.getType(thisRep));
    const int rightCanonType = canonicalizeBSONType(oimpl.getType(otherRep));
    const int diffCanon = leftCanonType - rightCanonType;
    if (diffCanon != 0)
        return diffCanon;

    // If we are considering field names, and the field names do not compare as equal,
    // return the field name ordering as the element ordering.
    if (considerFieldName) {
        const int fnamesComp = impl.getFieldName(thisRep).compare(oimpl.getFieldName(otherRep));
        if (fnamesComp != 0)
            return fnamesComp;
    }

    const bool considerChildFieldNames =
        (impl.getType(thisRep) != mongo::Array) && (oimpl.getType(otherRep) != mongo::Array);

    // We are dealing with either two objects, or two arrays. We need to consider the child
    // elements individually. We walk two iterators forward over the children and compare
    // them. Length mismatches are handled by checking early for reaching the end of the
    // children.
    ConstElement thisIter = leftChild();
    ConstElement otherIter = other.leftChild();

    while (true) {
        if (!thisIter.ok())
            return !otherIter.ok() ? 0 : -1;
        if (!otherIter.ok())
            return 1;

        const int result =
            thisIter.compareWithElement(otherIter, comparator, considerChildFieldNames);
        if (result != 0)
            return result;

        thisIter = thisIter.rightSibling();
        otherIter = otherIter.rightSibling();
    }
}

int Element::compareWithBSONElement(const BSONElement& other,
                                    const StringDataComparator* comparator,
                                    bool considerFieldName) const {
    invariant(ok());

    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);

    // If we have a representation as a BSONElement, we can just use BSONElement::woCompare
    // to do the entire comparison.
    if (impl.hasValue(thisRep))
        return impl.getSerializedElement(thisRep).woCompare(other, considerFieldName, comparator);

    // Leaf elements should always have a value, so we should only be dealing with Objects
    // or Arrays here.
    dassert(!impl.isLeaf(thisRep));

    // Obtain the canonical types for this Element and the BSONElement, if they are
    // different use the difference as the result. Please see BSONElement::woCompare for
    // details. We know that thisRep is not a number, so we don't need to check that
    // particular case.
    const int leftCanonType = canonicalizeBSONType(impl.getType(thisRep));
    const int rightCanonType = canonicalizeBSONType(other.type());
    const int diffCanon = leftCanonType - rightCanonType;
    if (diffCanon != 0)
        return diffCanon;

    // If we are considering field names, and the field names do not compare as equal,
    // return the field name ordering as the element ordering.
    if (considerFieldName) {
        const int fnamesComp = impl.getFieldName(thisRep).compare(other.fieldNameStringData());
        if (fnamesComp != 0)
            return fnamesComp;
    }

    const bool considerChildFieldNames =
        (impl.getType(thisRep) != mongo::Array) && (other.type() != mongo::Array);

    return compareWithBSONObj(other.Obj(), comparator, considerChildFieldNames);
}

int Element::compareWithBSONObj(const BSONObj& other,
                                const StringDataComparator* comparator,
                                bool considerFieldName) const {
    invariant(ok());

    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    invariant(!impl.isLeaf(thisRep));

    // We are dealing with either two objects, or two arrays. We need to consider the child
    // elements individually. We walk two iterators forward over the children and compare
    // them. Length mismatches are handled by checking early for reaching the end of the
    // children.
    ConstElement thisIter = leftChild();
    BSONObjIterator otherIter(other);

    while (true) {
        const BSONElement otherVal = otherIter.next();

        if (!thisIter.ok())
            return otherVal.eoo() ? 0 : -1;
        if (otherVal.eoo())
            return 1;

        const int result = thisIter.compareWithBSONElement(otherVal, comparator, considerFieldName);
        if (result != 0)
            return result;

        thisIter = thisIter.rightSibling();
    }
}

void Element::writeTo(BSONObjBuilder* const builder) const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    invariant(impl.getType(thisRep) == mongo::Object);
    if (thisRep.parent == kInvalidRepIdx && _repIdx == kRootRepIdx) {
        // If this is the root element, then we need to handle it differently, since it
        // doesn't have a field name and should embed directly, rather than as an object.
        impl.writeChildren(_repIdx, builder);
    } else {
        impl.writeElement(_repIdx, builder);
    }
}

void Element::writeChildrenTo(BSONObjBuilder* const builder) const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    invariant(impl.getType(thisRep) == mongo::Object);
    impl.writeChildren(_repIdx, builder);
}

void Element::writeArrayTo(BSONArrayBuilder* const builder) const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    invariant(impl.getType(thisRep) == mongo::Array);
    return impl.writeChildren(_repIdx, builder);
}

Status Element::setValueDouble(const double value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    ElementRep thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementDouble(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueString(StringData value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();

    dassert(impl.doesNotAlias(value));

    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementString(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueObject(const BSONObj& value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();

    dassert(impl.doesNotAlias(value));

    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementObject(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueArray(const BSONObj& value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();

    dassert(impl.doesNotAlias(value));

    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementArray(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueBinary(const uint32_t len,
                               mongo::BinDataType binType,
                               const void* const data) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();

    // TODO: Alias check for binary data?

    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementBinary(fieldName, len, binType, data);
    return setValue(newValue._repIdx);
}

Status Element::setValueUndefined() {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementUndefined(fieldName);
    return setValue(newValue._repIdx);
}

Status Element::setValueOID(const OID value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementOID(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueBool(const bool value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    ElementRep thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementBool(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueDate(const Date_t value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementDate(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueNull() {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementNull(fieldName);
    return setValue(newValue._repIdx);
}

Status Element::setValueRegex(StringData re, StringData flags) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();

    dassert(impl.doesNotAlias(re));
    dassert(impl.doesNotAlias(flags));

    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementRegex(fieldName, re, flags);
    return setValue(newValue._repIdx);
}

Status Element::setValueDBRef(StringData ns, const OID oid) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();

    dassert(impl.doesNotAlias(ns));

    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementDBRef(fieldName, ns, oid);
    return setValue(newValue._repIdx);
}

Status Element::setValueCode(StringData value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();

    dassert(impl.doesNotAlias(value));

    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementCode(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueSymbol(StringData value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();

    dassert(impl.doesNotAlias(value));

    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementSymbol(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueCodeWithScope(StringData code, const BSONObj& scope) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();

    dassert(impl.doesNotAlias(code));
    dassert(impl.doesNotAlias(scope));

    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementCodeWithScope(fieldName, code, scope);
    return setValue(newValue._repIdx);
}

Status Element::setValueInt(const int32_t value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    ElementRep thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementInt(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueTimestamp(const Timestamp value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementTimestamp(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueLong(const int64_t value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    ElementRep thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementLong(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueDecimal(const Decimal128 value) {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    ElementRep thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementDecimal(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueMinKey() {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementMinKey(fieldName);
    return setValue(newValue._repIdx);
}

Status Element::setValueMaxKey() {
    invariant(ok());
    Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementMaxKey(fieldName);
    return setValue(newValue._repIdx);
}

Status Element::setValueBSONElement(const BSONElement& value) {
    invariant(ok());

    if (value.type() == mongo::EOO)
        return Status(ErrorCodes::IllegalOperation, "Can't set Element value to EOO");

    Document::Impl& impl = getDocument().getImpl();

    dassert(impl.doesNotAlias(value));

    ElementRep thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementWithNewFieldName(fieldName, value);
    return setValue(newValue._repIdx);
}

Status Element::setValueSafeNum(const SafeNum value) {
    invariant(ok());
    switch (value.type()) {
        case mongo::NumberInt:
            return setValueInt(value._value.int32Val);
        case mongo::NumberLong:
            return setValueLong(value._value.int64Val);
        case mongo::NumberDouble:
            return setValueDouble(value._value.doubleVal);
        case mongo::NumberDecimal:
            return setValueDecimal(Decimal128(value._value.decimalVal));
        default:
            return Status(ErrorCodes::UnsupportedFormat,
                          "Don't know how to handle unexpected SafeNum type");
    }
}

Status Element::setValueElement(ConstElement setFrom) {
    invariant(ok());

    // Can't set to your own root element, since this would create a circular document.
    if (_doc->root() == setFrom) {
        return Status(ErrorCodes::IllegalOperation,
                      "Attempt to set an element to its own document's root");
    }

    // Setting to self is a no-op.
    //
    // Setting the root is always an error so we want to fall through to the error handling in this
    // case.
    if (*this == setFrom && _repIdx != kRootRepIdx) {
        return Status::OK();
    }

    Document::Impl& impl = getDocument().getImpl();
    ElementRep thisRep = impl.getElementRep(_repIdx);
    const StringData fieldName = impl.getFieldNameForNewElement(thisRep);
    Element newValue = getDocument().makeElementWithNewFieldName(fieldName, setFrom);
    return setValue(newValue._repIdx);
}

BSONType Element::getType() const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    return impl.getType(thisRep);
}

StringData Element::getFieldName() const {
    invariant(ok());
    const Document::Impl& impl = getDocument().getImpl();
    const ElementRep& thisRep = impl.getElementRep(_repIdx);
    return impl.getFieldName(thisRep);
}

Status Element::addChild(Element e, bool front) {
    // No need to invariant(ok()) since we are only called from methods that have done so.
    dassert(ok());

    invariant(e.ok());
    invariant(_doc == e._doc);

    Document::Impl& impl = getDocument().getImpl();
    ElementRep& newRep = impl.getElementRep(e._repIdx);

    // check that new element roots a clean subtree.
    if (!canAttach(e._repIdx, newRep))
        return getAttachmentError(newRep);

    // Check that this element is eligible for children.
    ElementRep& thisRep = impl.getElementRep(_repIdx);
    if (impl.isLeaf(thisRep))
        return Status(ErrorCodes::IllegalOperation,
                      "Attempt to add a child element to a non-object element");

    impl.disableInPlaceUpdates();

    // TODO: In both of the following cases, we call two public API methods each. We can
    // probably do better by writing this explicitly here and drying it with the public
    // addSiblingLeft and addSiblingRight implementations.
    if (front) {
        // TODO: It is cheap to get the left child. However, it still means creating a rep
        // for it. Can we do better?
        Element lc = leftChild();
        if (lc.ok())
            return lc.addSiblingLeft(e);
    } else {
        // TODO: It is expensive to get the right child, since we have to build reps for
        // all of the opaque children. But in principle, we don't really need them. Could
        // we potentially add this element as a right child, leaving its left sibling
        // opaque? We would at minimum need to update leftSibling, which currently assumes
        // that your left sibling is never opaque. But adding new Elements to the end is a
        // quite common operation, so it would be nice if we could do this efficiently.
        Element rc = rightChild();
        if (rc.ok())
            return rc.addSiblingRight(e);
    }

    // It must be the case that we have no children, so the new element becomes both the
    // right and left child of this node.
    dassert((thisRep.child.left == kInvalidRepIdx) && (thisRep.child.right == kInvalidRepIdx));
    thisRep.child.left = thisRep.child.right = e._repIdx;
    newRep.parent = _repIdx;
    impl.deserialize(_repIdx);
    return Status::OK();
}

Status Element::setValue(const Element::RepIdx newValueIdx) {
    // No need to invariant(ok()) since we are only called from methods that have done so.
    dassert(ok());

    if (_repIdx == kRootRepIdx)
        return Status(ErrorCodes::IllegalOperation, "Cannot call setValue on the root object");

    Document::Impl& impl = getDocument().getImpl();

    // Establish our right sibling in case it is opaque. Otherwise, we would lose the
    // ability to do so after the modifications below. It is important that this occur
    // before we acquire thisRep and valueRep since otherwise we would potentially
    // invalidate them.
    impl.resolveRightSibling(_repIdx);

    ElementRep& thisRep = impl.getElementRep(_repIdx);
    ElementRep& valueRep = impl.getElementRep(newValueIdx);

    if (impl.isInPlaceModeEnabled() && impl.canUpdateInPlace(valueRep, thisRep)) {
        // Get the BSONElement representations of the existing and new value, so we can
        // check if they are size compatible.
        BSONElement thisElt = impl.getSerializedElement(thisRep);
        BSONElement valueElt = impl.getSerializedElement(valueRep);

        if (thisElt.size() == valueElt.size()) {
            // The old and new elements are size compatible. Compute the base offsets
            // of each BSONElement in the object in which it resides. We use these to
            // calculate the source and target offsets in the damage entries we are
            // going to write.

            const DamageEvent::OffsetSizeType targetBaseOffset =
                getElementOffset(impl.getObject(thisRep.objIdx), thisElt);

            const DamageEvent::OffsetSizeType sourceBaseOffset =
                getElementOffset(impl.getObject(valueRep.objIdx), valueElt);

            // If this is a type change, record a damage event for the new type.
            if (thisElt.type() != valueElt.type()) {
                impl.recordDamageEvent(targetBaseOffset, sourceBaseOffset, 1);
            }

            dassert(thisElt.fieldNameSize() == valueElt.fieldNameSize());
            dassert(thisElt.valuesize() == valueElt.valuesize());

            // Record a damage event for the new value data.
            impl.recordDamageEvent(targetBaseOffset + thisElt.fieldNameSize() + 1,
                                   sourceBaseOffset + thisElt.fieldNameSize() + 1,
                                   thisElt.valuesize());
        } else {
            // We couldn't do it in place, so disable future in-place updates.
            impl.disableInPlaceUpdates();
        }
    }

    // If we are not rootish, then wire in the new value among our relations.
    if (thisRep.parent != kInvalidRepIdx) {
        valueRep.parent = thisRep.parent;
        valueRep.sibling.left = thisRep.sibling.left;
        valueRep.sibling.right = thisRep.sibling.right;
    }

    // Copy the rep for value to our slot so that our repIdx is unmodified.
    thisRep = valueRep;

    // Be nice and clear out the source rep to make debugging easier.
    valueRep = ElementRep();

    impl.deserialize(thisRep.parent);
    return Status::OK();
}


namespace {

// A helper for Element::writeElement below. For cases where we are building inside an
// array, we want to ignore field names. So the specialization for BSONArrayBuilder ignores
// the third parameter.
template <typename Builder>
struct SubBuilder;

template <>
struct SubBuilder<BSONObjBuilder> {
    SubBuilder(BSONObjBuilder* builder, BSONType type, StringData fieldName)
        : buffer((type == mongo::Array) ? builder->subarrayStart(fieldName)
                                        : builder->subobjStart(fieldName)) {}
    BufBuilder& buffer;
};

template <>
struct SubBuilder<BSONArrayBuilder> {
    SubBuilder(BSONArrayBuilder* builder, BSONType type, StringData)
        : buffer((type == mongo::Array) ? builder->subarrayStart() : builder->subobjStart()) {}
    BufBuilder& buffer;
};

static void appendElement(BSONObjBuilder* builder,
                          const BSONElement& element,
                          const StringData* fieldName) {
    if (fieldName)
        builder->appendAs(element, *fieldName);
    else
        builder->append(element);
}

// BSONArrayBuilder should not be appending elements with a fieldName
static void appendElement(BSONArrayBuilder* builder,
                          const BSONElement& element,
                          const StringData* fieldName) {
    invariant(!fieldName);
    builder->append(element);
}

}  // namespace

template <typename Builder>
void Document::Impl::writeElement(Element::RepIdx repIdx,
                                  Builder* builder,
                                  const StringData* fieldName) const {
    const ElementRep& rep = getElementRep(repIdx);

    if (hasValue(rep)) {
        appendElement(builder, getSerializedElement(rep), fieldName);
    } else {
        const BSONType type = getType(rep);
        const StringData subName = fieldName ? *fieldName : getFieldName(rep);
        SubBuilder<Builder> subBuilder(builder, type, subName);

        // Otherwise, this is a 'dirty leaf', which is impossible.
        dassert((type == mongo::Array) || (type == mongo::Object));

        if (type == mongo::Array) {
            BSONArrayBuilder child_builder(subBuilder.buffer);
            writeChildren(repIdx, &child_builder);
            child_builder.doneFast();
        } else {
            BSONObjBuilder child_builder(subBuilder.buffer);
            writeChildren(repIdx, &child_builder);
            child_builder.doneFast();
        }
    }
}

template <typename Builder>
void Document::Impl::writeChildren(Element::RepIdx repIdx, Builder* builder) const {
    // TODO: In theory, I think we can walk rightwards building a write region from all
    // serialized embedded children that share an obj id and form a contiguous memory
    // region. For arrays we would need to know something about how many elements we wrote
    // that way so that the indexes would come out right.
    //
    // However, that involves walking the memory twice: once to build the copy region, and
    // another time to actually copy it. It is unclear if this is better than just walking
    // it once with the recursive solution.

    const ElementRep& rep = getElementRep(repIdx);

    // OK, need to resolve left if we haven't done that yet.
    Element::RepIdx current = rep.child.left;
    if (current == Element::kOpaqueRepIdx)
        current = const_cast<Impl*>(this)->resolveLeftChild(repIdx);

    // We need to write the element, and then walk rightwards.
    while (current != Element::kInvalidRepIdx) {
        writeElement(current, builder);

        // If we have an opaque region to the right, and we are not in an array, then we
        // can bulk copy from the end of the element we just wrote to the end of our
        // parent.
        const ElementRep& currentRep = getElementRep(current);

        if (currentRep.sibling.right == Element::kOpaqueRepIdx) {
            // Obtain the current parent, so we can see if we can bulk copy the right
            // siblings.
            const ElementRep& parentRep = getElementRep(currentRep.parent);

            // Bulk copying right only works on objects
            if ((getType(parentRep) == mongo::Object) && (currentRep.objIdx != kInvalidObjIdx) &&
                (currentRep.objIdx == parentRep.objIdx)) {
                BSONElement currentElt = getSerializedElement(currentRep);
                const uint32_t currentSize = currentElt.size();

                const BSONObj parentObj = (currentRep.parent == kRootRepIdx)
                    ? getObject(parentRep.objIdx)
                    : getSerializedElement(parentRep).Obj();
                const uint32_t parentSize = parentObj.objsize();

                const uint32_t currentEltOffset = getElementOffset(parentObj, currentElt);
                const uint32_t nextEltOffset = currentEltOffset + currentSize;

                const char* copyBegin = parentObj.objdata() + nextEltOffset;
                const uint32_t copyBytes = parentSize - nextEltOffset;

                // The -1 is because we don't want to copy in the terminal EOO.
                builder->bb().appendBuf(copyBegin, copyBytes - 1);

                // We are done with all children.
                break;
            }

            // We couldn't bulk copy, and our right sibling is opaque. We need to
            // resolve. Note that the call to resolve may invalidate 'currentRep', so
            // rather than falling through and acquiring the index by examining currentRep,
            // update it with the return value of resolveRightSibling and restart the loop.
            current = const_cast<Impl*>(this)->resolveRightSibling(current);
            continue;
        }

        current = currentRep.sibling.right;
    }
}

Document::Document() : _impl(new Impl(Document::kInPlaceDisabled)), _root(makeRootElement()) {
    dassert(_root._repIdx == kRootRepIdx);
}

Document::Document(const BSONObj& value, InPlaceMode inPlaceMode)
    : _impl(new Impl(inPlaceMode)), _root(makeRootElement(value)) {
    dassert(_root._repIdx == kRootRepIdx);
}

void Document::reset() {
    _impl->reset(Document::kInPlaceDisabled);
    [[maybe_unused]] const Element newRoot = makeRootElement();
    dassert(newRoot._repIdx == _root._repIdx);
    dassert(_root._repIdx == kRootRepIdx);
}

void Document::reset(const BSONObj& value, InPlaceMode inPlaceMode) {
    _impl->reset(inPlaceMode);
    [[maybe_unused]] const Element newRoot = makeRootElement(value);
    dassert(newRoot._repIdx == _root._repIdx);
    dassert(_root._repIdx == kRootRepIdx);
}

Document::~Document() {}

void Document::reserveDamageEvents(size_t expectedEvents) {
    return getImpl().reserveDamageEvents(expectedEvents);
}

bool Document::getInPlaceUpdates(DamageVector* damages, const char** source, size_t* size) {
    return getImpl().getInPlaceUpdates(damages, source, size);
}

void Document::disableInPlaceUpdates() {
    return getImpl().disableInPlaceUpdates();
}

Document::InPlaceMode Document::getCurrentInPlaceMode() const {
    return getImpl().getCurrentInPlaceMode();
}

Element Document::makeElementDouble(StringData fieldName, const double value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.append(fieldName, value);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementString(StringData fieldName, StringData value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));
    dassert(impl.doesNotAliasLeafBuilder(value));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.append(fieldName, value);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementObject(StringData fieldName) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasFieldNameHeap(fieldName));

    Element::RepIdx newEltIdx;
    ElementRep& newElt = impl.makeNewRep(&newEltIdx);
    impl.insertFieldName(newElt, fieldName);
    return Element(this, newEltIdx);
}

Element Document::makeElementObject(StringData fieldName, const BSONObj& value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));
    dassert(impl.doesNotAlias(value));

    // Copy the provided values into the leaf builder.
    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.append(fieldName, value);
    Element::RepIdx newEltIdx =
        impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef);
    ElementRep& newElt = impl.getElementRep(newEltIdx);

    newElt.child.left = Element::kOpaqueRepIdx;
    newElt.child.right = Element::kOpaqueRepIdx;

    return Element(this, newEltIdx);
}

Element Document::makeElementArray(StringData fieldName) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasFieldNameHeap(fieldName));

    Element::RepIdx newEltIdx;
    ElementRep& newElt = impl.makeNewRep(&newEltIdx);
    newElt.array = true;
    impl.insertFieldName(newElt, fieldName);
    return Element(this, newEltIdx);
}

Element Document::makeElementArray(StringData fieldName, const BSONObj& value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));
    dassert(impl.doesNotAlias(value));

    // Copy the provided array values into the leaf builder.
    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendArray(fieldName, value);
    Element::RepIdx newEltIdx =
        impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef);
    ElementRep& newElt = impl.getElementRep(newEltIdx);
    newElt.child.left = Element::kOpaqueRepIdx;
    newElt.child.right = Element::kOpaqueRepIdx;
    return Element(this, newEltIdx);
}

Element Document::makeElementBinary(StringData fieldName,
                                    const uint32_t len,
                                    const mongo::BinDataType binType,
                                    const void* const data) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));
    // TODO: Alias check 'data'?

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendBinData(fieldName, len, binType, data);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementUndefined(StringData fieldName) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendUndefined(fieldName);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementNewOID(StringData fieldName) {
    OID newOID;
    newOID.init();
    return makeElementOID(fieldName, newOID);
}

Element Document::makeElementOID(StringData fieldName, const OID value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.append(fieldName, value);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementBool(StringData fieldName, const bool value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendBool(fieldName, value);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementDate(StringData fieldName, const Date_t value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendDate(fieldName, value);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementNull(StringData fieldName) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendNull(fieldName);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementRegex(StringData fieldName, StringData re, StringData flags) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));
    dassert(impl.doesNotAliasLeafBuilder(re));
    dassert(impl.doesNotAliasLeafBuilder(flags));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendRegex(fieldName, re, flags);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementDBRef(StringData fieldName, StringData ns, const OID value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));
    dassert(impl.doesNotAliasLeafBuilder(ns));
    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendDBRef(fieldName, ns, value);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementCode(StringData fieldName, StringData value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));
    dassert(impl.doesNotAliasLeafBuilder(value));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendCode(fieldName, value);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementSymbol(StringData fieldName, StringData value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));
    dassert(impl.doesNotAliasLeafBuilder(value));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendSymbol(fieldName, value);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementCodeWithScope(StringData fieldName,
                                           StringData code,
                                           const BSONObj& scope) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));
    dassert(impl.doesNotAliasLeafBuilder(code));
    dassert(impl.doesNotAlias(scope));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendCodeWScope(fieldName, code, scope);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementInt(StringData fieldName, const int32_t value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.append(fieldName, value);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementTimestamp(StringData fieldName, const Timestamp value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.append(fieldName, value);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementLong(StringData fieldName, const int64_t value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.append(fieldName, static_cast<long long int>(value));
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementDecimal(StringData fieldName, const Decimal128 value) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.append(fieldName, value);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementMinKey(StringData fieldName) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendMinKey(fieldName);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElementMaxKey(StringData fieldName) {
    Impl& impl = getImpl();
    dassert(impl.doesNotAliasLeafBuilder(fieldName));

    BSONObjBuilder& builder = impl.leafBuilder();
    const int leafRef = builder.len();
    builder.appendMaxKey(fieldName);
    return Element(this,
                   impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
}

Element Document::makeElement(const BSONElement& value) {
    Impl& impl = getImpl();

    // Attempts to create an EOO element are translated to returning an invalid
    // Element. For array and object nodes, we flow through the custom
    // makeElement{Object|Array} methods, since they have special logic to deal with
    // opaqueness. Otherwise, we can just insert via appendAs.
    if (value.type() == mongo::EOO)
        return end();
    else if (value.type() == mongo::Object)
        return makeElementObject(value.fieldNameStringData(), value.Obj());
    else if (value.type() == mongo::Array)
        return makeElementArray(value.fieldNameStringData(), value.Obj());
    else {
        dassert(impl.doesNotAlias(value));
        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.append(value);
        return Element(this, impl.insertLeafElement(leafRef, value.fieldNameSize(), value.size()));
    }
}

Element Document::makeElementWithNewFieldName(StringData fieldName, const BSONElement& value) {
    Impl& impl = getImpl();

    // See the above makeElement for notes on these cases.
    if (value.type() == mongo::EOO)
        return end();
    else if (value.type() == mongo::Object)
        return makeElementObject(fieldName, value.Obj());
    else if (value.type() == mongo::Array)
        return makeElementArray(fieldName, value.Obj());
    else {
        dassert(getImpl().doesNotAliasLeafBuilder(fieldName));
        dassert(getImpl().doesNotAlias(value));
        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();
        builder.appendAs(value, fieldName);
        return Element(
            this, impl.insertLeafElement(leafRef, fieldName.size() + 1, builder.len() - leafRef));
    }
}

Element Document::makeElementSafeNum(StringData fieldName, SafeNum value) {
    dassert(getImpl().doesNotAliasLeafBuilder(fieldName));

    switch (value.type()) {
        case mongo::NumberInt:
            return makeElementInt(fieldName, value._value.int32Val);
        case mongo::NumberLong:
            return makeElementLong(fieldName, value._value.int64Val);
        case mongo::NumberDouble:
            return makeElementDouble(fieldName, value._value.doubleVal);
        case mongo::NumberDecimal:
            return makeElementDecimal(fieldName, Decimal128(value._value.decimalVal));
        default:
            // Return an invalid element to indicate that we failed.
            return end();
    }
}

Element Document::makeElement(ConstElement element) {
    return makeElement(element, nullptr);
}

Element Document::makeElementWithNewFieldName(StringData fieldName, ConstElement element) {
    return makeElement(element, &fieldName);
}

Element Document::makeRootElement() {
    return makeElementObject(kRootFieldName);
}

Element Document::makeRootElement(const BSONObj& value) {
    Impl& impl = getImpl();
    Element::RepIdx newEltIdx = Element::kInvalidRepIdx;
    ElementRep* newElt = &impl.makeNewRep(&newEltIdx);

    // A BSONObj provided for the root Element is stored in _objects rather than being
    // copied like all other BSONObjs.
    newElt->objIdx = impl.insertObject(value);
    impl.insertFieldName(*newElt, kRootFieldName);

    // Strictly, the following is a lie: the root isn't serialized, because it doesn't
    // have a contiguous fieldname. However, it is a useful fiction to pretend that it
    // is, so we can easily check if we have a 'pristine' document state by checking if
    // the root is marked as serialized.
    newElt->serialized = true;

    // If the provided value is empty, mark it as having no children, otherwise mark the
    // children as opaque.
    if (value.isEmpty())
        newElt->child.left = Element::kInvalidRepIdx;
    else
        newElt->child.left = Element::kOpaqueRepIdx;
    newElt->child.right = newElt->child.left;

    return Element(this, newEltIdx);
}

Element Document::makeElement(ConstElement element, const StringData* fieldName) {
    Impl& impl = getImpl();

    if (this == &element.getDocument()) {
        // If the Element that we want to build from belongs to this Document, then we have
        // to first copy it to the side, and then back in, since otherwise we might be
        // attempting both read to and write from the underlying BufBuilder simultaneously,
        // which will not work.
        BSONObjBuilder builder;
        impl.writeElement(element.getIdx(), &builder, fieldName);
        BSONObj built = builder.done();
        BSONElement newElement = built.firstElement();
        return makeElement(newElement);

    } else {
        // If the Element belongs to another document, then we can just stream it into our
        // builder. We still do need to dassert that the field name doesn't alias us
        // somehow.
        if (fieldName) {
            dassert(impl.doesNotAlias(*fieldName));
        }
        BSONObjBuilder& builder = impl.leafBuilder();
        const int leafRef = builder.len();

        const Impl& oImpl = element.getDocument().getImpl();
        oImpl.writeElement(element.getIdx(), &builder, fieldName);
        return Element(this,
                       impl.insertLeafElement(leafRef,
                                              fieldName ? fieldName->size() + 1 : -1,
                                              builder.len() - leafRef));
    }
}

inline Document::Impl& Document::getImpl() {
    // Don't use unique_ptr<Impl>::operator* since it may generate assertions that the
    // pointer is non-null, but we already know that to be always and forever true, and
    // otherwise the assertion code gets spammed into every method that inlines the call to
    // this function. We just dereference the pointer returned from 'get' ourselves.
    return *_impl.get();
}

inline const Document::Impl& Document::getImpl() const {
    return *_impl.get();
}

}  // namespace mutablebson
}  // namespace mongo
