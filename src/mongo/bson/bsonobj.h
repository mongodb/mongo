// @file bsonobj.h

/*    Copyright 2009 10gen Inc.
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

#include <bitset>
#include <list>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/data_type.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/base/string_data_comparator_interface.h"
#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/shared_buffer.h"

namespace mongo {

typedef std::set<BSONElement, BSONElementCmpWithoutField> BSONElementSet;
typedef std::multiset<BSONElement, BSONElementCmpWithoutField> BSONElementMSet;

/**
   C++ representation of a "BSON" object -- that is, an extended JSON-style
   object in a binary representation.

   See bsonspec.org.

   Note that BSONObj's have a smart pointer capability built in -- so you can
   pass them around by value.  The reference counts used to implement this
   do not use locking, so copying and destroying BSONObj's are not thread-safe
   operations.

 BSON object format:

 code
 <unsigned totalSize> {<byte BSONType><cstring FieldName><Data>}* EOO

 totalSize includes itself.

 Data:
 Bool:      <byte>
 EOO:       nothing follows
 Undefined: nothing follows
 OID:       an OID object
 NumberDouble: <double>
 NumberInt: <int32>
 NumberDecimal: <dec128>
 String:    <unsigned32 strsizewithnull><cstring>
 Date:      <8bytes>
 Regex:     <cstring regex><cstring options>
 Object:    a nested object, leading with its entire size, which terminates with EOO.
 Array:     same as object
 DBRef:     <strlen> <cstring ns> <oid>
 DBRef:     a database reference: basically a collection name plus an Object ID
 BinData:   <int len> <byte subtype> <byte[len] data>
 Code:      a function (not a closure): same format as String.
 Symbol:    a language symbol (say a python symbol).  same format as String.
 Code With Scope: <total size><String><Object>
 \endcode
 */
class BSONObj {
public:
    // Declared in bsonobj_comparator_interface.h.
    class ComparatorInterface;

    /**
     * Operator overloads for relops return a DeferredComparison which can subsequently be evaluated
     * by a BSONObj::ComparatorInterface.
     */
    using DeferredComparison = BSONComparatorInterfaceBase<BSONObj>::DeferredComparison;

    static const char kMinBSONLength = 5;

    /** Construct an empty BSONObj -- that is, {}. */
    BSONObj() {
        // Little endian ordering here, but that is ok regardless as BSON is spec'd to be
        // little endian external to the system. (i.e. the rest of the implementation of
        // bson, not this part, fails to support big endian)
        static const char kEmptyObjectPrototype[] = {/*size*/ kMinBSONLength, 0, 0, 0, /*eoo*/ 0};

        _objdata = kEmptyObjectPrototype;
    }

    /** Construct a BSONObj from data in the proper format.
     *  Use this constructor when something else owns bsonData's buffer
    */
    explicit BSONObj(const char* bsonData) {
        init(bsonData);
    }

    explicit BSONObj(ConstSharedBuffer ownedBuffer)
        : _objdata(ownedBuffer.get() ? ownedBuffer.get() : BSONObj().objdata()),
          _ownedBuffer(std::move(ownedBuffer)) {}

    /** Move construct a BSONObj */
    BSONObj(BSONObj&& other) noexcept : _objdata(std::move(other._objdata)),
                                        _ownedBuffer(std::move(other._ownedBuffer)) {
        other._objdata = BSONObj()._objdata;  // To return to an empty state.
        dassert(!other.isOwned());
    }

    // The explicit move constructor above will inhibit generation of the copy ctor, so
    // explicitly request the default implementation.

    /** Copy construct a BSONObj. */
    BSONObj(const BSONObj&) = default;

    /** Provide assignment semantics. We use the value taking form so that we can use copy
     *  and swap, and consume both lvalue and rvalue references.
     */
    BSONObj& operator=(BSONObj otherCopy) noexcept {
        this->swap(otherCopy);
        return *this;
    }

    /** Swap this BSONObj with 'other' */
    void swap(BSONObj& other) noexcept {
        using std::swap;
        swap(_objdata, other._objdata);
        swap(_ownedBuffer, other._ownedBuffer);
    }

    /**
       A BSONObj can use a buffer it "owns" or one it does not.

       OWNED CASE
       If the BSONObj owns the buffer, the buffer can be shared among several BSONObj's (by
       assignment). In this case the buffer is basically implemented as a shared_ptr.
       Since BSONObj's are typically immutable, this works well.

       UNOWNED CASE
       A BSONObj can also point to BSON data in some other data structure it does not "own" or free
       later. For example, in a memory mapped file.  In this case, it is important the original data
       stays in scope for as long as the BSONObj is in use.  If you think the original data may go
       out of scope, call BSONObj::getOwned() to promote your BSONObj to having its own copy.

       On a BSONObj assignment, if the source is unowned, both the source and dest will have unowned
       pointers to the original buffer after the assignment.

       If you are not sure about ownership but need the buffer to last as long as the BSONObj, call
       getOwned().  getOwned() is a no-op if the buffer is already owned.  If not already owned, a
       malloc and memcpy will result.

       Most ways to create BSONObj's create 'owned' variants.  Unowned versions can be created with:
       (1) specifying true for the ifree parameter in the constructor
       (2) calling BSONObjBuilder::done().  Use BSONObjBuilder::obj() to get an owned copy
       (3) retrieving a subobject retrieves an unowned pointer into the parent BSON object

       @return true if this is in owned mode
    */
    bool isOwned() const {
        return bool(_ownedBuffer);
    }

    /**
     * Share ownership with another object.
     *
     * It is the callers responsibility to ensure that the other object is owned and contains the
     * data this BSONObj is viewing. This can happen if this is a subobject or sibling object
     * contained in a larger buffer.
     */
    BSONObj& shareOwnershipWith(ConstSharedBuffer buffer) & {
        invariant(buffer);
        _ownedBuffer = buffer;
        return *this;
    }
    BSONObj& shareOwnershipWith(const BSONObj& other) & {
        shareOwnershipWith(other.sharedBuffer());
        return *this;
    }
    BSONObj&& shareOwnershipWith(ConstSharedBuffer buffer) && {
        return std::move(shareOwnershipWith(buffer));
    }
    BSONObj&& shareOwnershipWith(const BSONObj& other) && {
        return std::move(shareOwnershipWith(other));
    }

    const ConstSharedBuffer& sharedBuffer() const {
        invariant(isOwned());
        return _ownedBuffer;
    }

    ConstSharedBuffer releaseSharedBuffer() {
        invariant(isOwned());
        BSONObj sink = std::move(*this);  // Leave *this in a valid moved-from state.
        return std::move(sink._ownedBuffer);
    }

    /** If the data buffer is under the control of this BSONObj, return it.
        Else return an owned copy.
    */
    BSONObj getOwned() const;

    /** @return a new full (and owned) copy of the object. */
    BSONObj copy() const;

    /** Readable representation of a BSON object in an extended JSON-style notation.
        This is an abbreviated representation which might be used for logging.
    */
    enum { maxToStringRecursionDepth = 100 };

    std::string toString(bool redactValues = false) const;
    void toString(StringBuilder& s,
                  bool isArray = false,
                  bool full = false,
                  bool redactValues = false,
                  int depth = 0) const;

    /** Properly formatted JSON string.
        @param pretty if true we try to add some lf's and indentation
    */
    std::string jsonString(JsonStringFormat format = Strict,
                           int pretty = 0,
                           bool isArray = false) const;

    /** note: addFields always adds _id even if not specified */
    int addFields(BSONObj& from, std::set<std::string>& fields); /* returns n added */

    /**
     * Add specific field to the end of the object if it did not exist, otherwise replace it
     * preserving original field order. Returns newly built object. Returns copy of this for empty
     * field.
     */
    BSONObj addField(const BSONElement& field) const;

    /** remove specified field and return a new object with the remaining fields.
        slowish as builds a full new object
     */
    BSONObj removeField(StringData name) const;

    /** returns # of top level fields in the object
       note: iterates to count the fields
    */
    int nFields() const;

    /** adds the field names to the fields set.  does NOT clear it (appends). */
    int getFieldNames(std::set<std::string>& fields) const;

    /** Get the field of the specified name. eoo() is true on the returned
        element if not found.
    */
    BSONElement getField(StringData name) const;

    /** Get several fields at once. This is faster than separate getField() calls as the size of
        elements iterated can then be calculated only once each.
        @param n number of fieldNames, and number of elements in the fields array
        @param fields if a field is found its element is stored in its corresponding position in
                this array. if not found the array element is unchanged.
     */

    void getFields(unsigned n, const char** fieldNames, BSONElement* fields) const;

    /**
     * Get several fields at once. This is faster than separate getField() calls as the size of
     * elements iterated can then be calculated only once each.
     */
    template <size_t N>
    void getFields(const std::array<StringData, N>& fieldNames,
                   std::array<BSONElement, N>* fields) const;


    /** Get the field of the specified name. eoo() is true on the returned
        element if not found.
    */
    BSONElement operator[](StringData field) const {
        return getField(field);
    }

    BSONElement operator[](int field) const {
        StringBuilder ss;
        ss << field;
        std::string s = ss.str();
        return getField(s.c_str());
    }

    /** @return true if field exists */
    bool hasField(StringData name) const {
        return !getField(name).eoo();
    }
    /** @return true if field exists */
    bool hasElement(StringData name) const {
        return hasField(name);
    }

    /** @return "" if DNE or wrong type */
    const char* getStringField(StringData name) const;

    /** @return subobject of the given name */
    BSONObj getObjectField(StringData name) const;

    /** @return INT_MIN if not present - does some type conversions */
    int getIntField(StringData name) const;

    /** @return false if not present
        @see BSONElement::trueValue()
     */
    bool getBoolField(StringData name) const;

    /** @param pattern a BSON obj indicating a set of (un-dotted) field
     *  names.  Element values are ignored.
     *  @return a BSON obj constructed by taking the elements of this obj
     *  that correspond to the fields in pattern. Field names of the
     *  returned object are replaced with the empty string. If field in
     *  pattern is missing, it is omitted from the returned object.
     *
     *  Example: if this = {a : 4 , b : 5 , c : 6})
     *    this.extractFieldsUnDotted({a : 1 , c : 1}) -> {"" : 4 , "" : 6 }
     *    this.extractFieldsUnDotted({b : "blah"}) -> {"" : 5}
     *
    */
    BSONObj extractFieldsUnDotted(const BSONObj& pattern) const;

    BSONObj filterFieldsUndotted(const BSONObj& filter, bool inFilter) const;

    BSONElement getFieldUsingIndexNames(StringData fieldName, const BSONObj& indexKey) const;

    /** arrays are bson objects with numeric and increasing field names
        @return true if field names are numeric and increasing
     */
    bool couldBeArray() const;

    /** @return the raw data of the object */
    const char* objdata() const {
        return _objdata;
    }

    /** @return total size of the BSON object in bytes */
    int objsize() const {
        return ConstDataView(objdata()).read<LittleEndian<int>>();
    }

    /** performs a cursory check on the object's size only. */
    bool isValid() const {
        int x = objsize();
        return x > 0 && x <= BSONObjMaxInternalSize;
    }

    /**
     * Validates that the element is okay to be stored in a collection.
     * Recursively validates children.
     */
    Status storageValidEmbedded() const;

    /** @return true if object is empty -- i.e.,  {} */
    bool isEmpty() const {
        return objsize() <= kMinBSONLength;
    }

    void dump() const;

    /** Alternative output format */
    std::string hexDump() const;

    //
    // Comparison API.
    //
    // BSONObj instances can be compared either using woCompare() or via operator overloads. Most
    // callers should prefer operator overloads. Note that the operator overloads return a
    // DeferredComparison, which must be subsequently evaluated by a BSONObj::ComparatorInterface.
    // See bsonobj_comparator_interface.h for details.
    //

    /**wo='well ordered'.  fields must be in same order in each object.
       Ordering is with respect to the signs of the elements
       and allows ascending / descending key mixing.
       If comparator is non-null, it is used for all comparisons between two strings.
       @return  <0 if l<r. 0 if l==r. >0 if l>r
    */
    int woCompare(const BSONObj& r,
                  const Ordering& o,
                  bool considerFieldName = true,
                  const StringData::ComparatorInterface* comparator = nullptr) const;

    /**wo='well ordered'.  fields must be in same order in each object.
       Ordering is with respect to the signs of the elements
       and allows ascending / descending key mixing.
       If comparator is non-null, it is used for all comparisons between two strings.
       @return  <0 if l<r. 0 if l==r. >0 if l>r
    */
    int woCompare(const BSONObj& r,
                  const BSONObj& ordering = BSONObj(),
                  bool considerFieldName = true,
                  const StringData::ComparatorInterface* comparator = nullptr) const;

    DeferredComparison operator<(const BSONObj& other) const {
        return DeferredComparison(DeferredComparison::Type::kLT, *this, other);
    }

    DeferredComparison operator<=(const BSONObj& other) const {
        return DeferredComparison(DeferredComparison::Type::kLTE, *this, other);
    }

    DeferredComparison operator>(const BSONObj& other) const {
        return DeferredComparison(DeferredComparison::Type::kGT, *this, other);
    }

    DeferredComparison operator>=(const BSONObj& other) const {
        return DeferredComparison(DeferredComparison::Type::kGTE, *this, other);
    }

    DeferredComparison operator==(const BSONObj& other) const {
        return DeferredComparison(DeferredComparison::Type::kEQ, *this, other);
    }

    DeferredComparison operator!=(const BSONObj& other) const {
        return DeferredComparison(DeferredComparison::Type::kNE, *this, other);
    }

    /**
     * Returns true if 'this' is a prefix of otherObj- in other words if otherObj contains the same
     * field names and field vals in the same order as 'this', plus optionally some additional
     * elements.
     *
     * All comparisons between elements are made using 'eltCmp'.
     */
    bool isPrefixOf(const BSONObj& otherObj, const BSONElement::ComparatorInterface& eltCmp) const;

    /**
     * @param otherObj
     * @return returns true if the list of field names in 'this' is a prefix
     * of the list of field names in otherObj.  Similar to 'isPrefixOf',
     * but ignores the field values and only looks at field names.
     */
    bool isFieldNamePrefixOf(const BSONObj& otherObj) const;

    /** This is "shallow equality" -- ints and doubles won't match.  for a
       deep equality test use woCompare (which is slower).
    */
    bool binaryEqual(const BSONObj& r) const {
        int os = objsize();
        if (os == r.objsize()) {
            return (os == 0 || memcmp(objdata(), r.objdata(), os) == 0);
        }
        return false;
    }

    /** @return first field of the object */
    BSONElement firstElement() const {
        return BSONElement(objdata() + 4);
    }

    /** faster than firstElement().fieldName() - for the first element we can easily find the
     * fieldname without computing the element size.
     */
    const char* firstElementFieldName() const {
        const char* p = objdata() + 4;
        return *p == EOO ? "" : p + 1;
    }

    BSONType firstElementType() const {
        const char* p = objdata() + 4;
        return (BSONType)*p;
    }

    /** Get the _id field from the object.  For good performance drivers should
        assure that _id is the first element of the object; however, correct operation
        is assured regardless.
        @return true if found
    */
    bool getObjectID(BSONElement& e) const;

    // Return a version of this object where top level elements of types
    // that are not part of the bson wire protocol are replaced with
    // std::string identifier equivalents.
    // TODO Support conversion of element types other than min and max.
    BSONObj clientReadable() const;

    /** Return new object with the field names replaced by those in the
        passed object. */
    BSONObj replaceFieldNames(const BSONObj& obj) const;

    /**
     * Returns true if this object is valid according to the specified BSON version, and returns
     * false otherwise.
     */
    bool valid(BSONVersion version) const;

    /** add all elements of the object to the specified vector */
    void elems(std::vector<BSONElement>&) const;
    /** add all elements of the object to the specified list */
    void elems(std::list<BSONElement>&) const;

    friend class BSONObjIterator;
    typedef BSONObjIterator iterator;

    /**
     * These enable range-based for loops over BSONObjs:
     *
     *      for (BSONElement elem : BSON("a" << 1 << "b" << 2)) {
     *          ... // Do something with elem
     *      }
     */
    BSONObjIterator begin() const;
    BSONObjIterator end() const;

    void appendSelfToBufBuilder(BufBuilder& b) const {
        verify(objsize());
        b.appendBuf(objdata(), objsize());
    }

    template <typename T>
    bool coerceVector(std::vector<T>* out) const;

    /// members for Sorter
    struct SorterDeserializeSettings {};  // unused
    void serializeForSorter(BufBuilder& buf) const {
        buf.appendBuf(objdata(), objsize());
    }
    static BSONObj deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
        const int size = buf.peek<LittleEndian<int>>();
        const void* ptr = buf.skip(size);
        return BSONObj(static_cast<const char*>(ptr));
    }
    int memUsageForSorter() const {
        // TODO consider ownedness?
        return sizeof(BSONObj) + objsize();
    }

private:
    void _assertInvalid() const;

    void init(const char* data) {
        _objdata = data;
        if (!isValid())
            _assertInvalid();
    }

    const char* _objdata;
    ConstSharedBuffer _ownedBuffer;
};

std::ostream& operator<<(std::ostream& s, const BSONObj& o);
std::ostream& operator<<(std::ostream& s, const BSONElement& e);

StringBuilder& operator<<(StringBuilder& s, const BSONObj& o);
StringBuilder& operator<<(StringBuilder& s, const BSONElement& e);

inline void swap(BSONObj& l, BSONObj& r) noexcept {
    l.swap(r);
}

struct BSONArray : BSONObj {
    // Don't add anything other than forwarding constructors!!!
    BSONArray() : BSONObj() {}
    explicit BSONArray(const BSONObj& obj) : BSONObj(obj) {}
};

/** iterator for a BSONObj

   Note each BSONObj ends with an EOO element: so you will get more() on an empty
   object, although next().eoo() will be true.

   The BSONObj must stay in scope for the duration of the iterator's execution.

   todo: Finish making this an STL-compatible iterator.
            Need iterator_catagory et al (maybe inherit from std::iterator).
            Need operator->
            operator* should return a const reference not a value.
*/
class BSONObjIterator {
public:
    /** Create an iterator for a BSON object.
    */
    explicit BSONObjIterator(const BSONObj& jso) {
        int sz = jso.objsize();
        if (MONGO_unlikely(sz == 0)) {
            _pos = _theend = 0;
            return;
        }
        _pos = jso.objdata() + 4;
        _theend = jso.objdata() + sz - 1;
    }

    BSONObjIterator(const char* start, const char* end) {
        _pos = start + 4;
        _theend = end - 1;
    }

    static BSONObjIterator endOf(const BSONObj& obj) {
        BSONObjIterator end(obj);
        end._pos = end._theend;
        return end;
    }

    /** @return true if more elements exist to be enumerated. */
    bool more() {
        return _pos < _theend;
    }

    /** @return true if more elements exist to be enumerated INCLUDING the EOO element which is
     * always at the end. */
    bool moreWithEOO() {
        return _pos <= _theend;
    }

    /**
     * @return the next element in the object. For the final element, element.eoo() will be true.
     */
    BSONElement next(bool checkEnd) {
        verify(_pos <= _theend);

        int maxLen = -1;
        if (checkEnd) {
            maxLen = _theend + 1 - _pos;
            verify(maxLen > 0);
        }

        BSONElement e(_pos, maxLen);
        int esize = e.size(maxLen);
        massert(16446, "BSONElement has bad size", esize > 0);
        _pos += esize;

        return e;
    }

    BSONElement next() {
        verify(_pos <= _theend);
        BSONElement e(_pos);
        _pos += e.size();
        return e;
    }

    /** pre-increment */
    BSONObjIterator& operator++() {
        next();
        return *this;
    }

    /** post-increment */
    BSONObjIterator operator++(int) {
        BSONObjIterator oldPos = *this;
        next();
        return oldPos;
    }

    BSONElement operator*() {
        verify(_pos <= _theend);
        return BSONElement(_pos);
    }

    bool operator==(const BSONObjIterator& other) {
        dassert(_theend == other._theend);
        return _pos == other._pos;
    }

    bool operator!=(const BSONObjIterator& other) {
        return !(*this == other);
    }

private:
    const char* _pos;
    const char* _theend;
};

/** Base class implementing ordered iteration through BSONElements. */
class BSONIteratorSorted {
    MONGO_DISALLOW_COPYING(BSONIteratorSorted);

public:
    ~BSONIteratorSorted() {
        verify(_fields);
    }

    bool more() {
        return _cur < _nfields;
    }

    BSONElement next() {
        verify(_fields);
        if (_cur < _nfields)
            return BSONElement(_fields[_cur++]);
        return BSONElement();
    }

protected:
    class ElementFieldCmp;
    BSONIteratorSorted(const BSONObj& o, const ElementFieldCmp& cmp);

private:
    const int _nfields;
    const std::unique_ptr<const char* []> _fields;
    int _cur;
};

/** Provides iteration of a BSONObj's BSONElements in lexical field order. */
class BSONObjIteratorSorted : public BSONIteratorSorted {
public:
    BSONObjIteratorSorted(const BSONObj& object);
};

/**
 * Provides iteration of a BSONArray's BSONElements in numeric field order.
 * The elements of a bson array should always be numerically ordered by field name, but this
 * implementation re-sorts them anyway.
 */
class BSONArrayIteratorSorted : public BSONIteratorSorted {
public:
    BSONArrayIteratorSorted(const BSONArray& array);
};

inline BSONObjIterator BSONObj::begin() const {
    return BSONObjIterator(*this);
}
inline BSONObjIterator BSONObj::end() const {
    return BSONObjIterator::endOf(*this);
}

/**
 * Similar to BOOST_FOREACH
 *
 * DEPRECATED: Use range-based for loops now.
 */
#define BSONForEach(elemName, obj) for (BSONElement elemName : (obj))

template <>
struct DataType::Handler<BSONObj> {
    static Status load(BSONObj* bson,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset) {
        auto temp = BSONObj(ptr);
        auto len = temp.objsize();
        if (bson) {
            *bson = std::move(temp);
        }
        if (advanced) {
            *advanced = len;
        }
        return Status::OK();
    }

    static Status store(const BSONObj& bson,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset);

    static BSONObj defaultConstruct() {
        return BSONObj();
    }
};

template <size_t N>
inline void BSONObj::getFields(const std::array<StringData, N>& fieldNames,
                               std::array<BSONElement, N>* fields) const {
    std::bitset<N> foundFields;
    auto iter = this->begin();
    while (iter.more() && !foundFields.all()) {
        auto el = iter.next();
        auto fieldName = el.fieldNameStringData();
        for (std::size_t i = 0; i < N; ++i) {
            if (!foundFields.test(i) && (fieldNames[i] == fieldName)) {
                (*fields)[i] = std::move(el);
                foundFields.set(i);
                break;
            }
        }
    }
}
}  // namespace mongo
