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

#include <array>
#include <bitset>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <iosfwd>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/base/data_type.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bson_comparator_interface_base.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BSONObjBuilder;

class BSONObjStlIterator;
class ExtendedCanonicalV200Generator;
class ExtendedRelaxedV200Generator;
class LegacyStrictGenerator;

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
    struct DefaultSizeTrait {
        constexpr static int MaxSize = BSONObjMaxInternalSize;
    };
    struct LargeSizeTrait {
        constexpr static int MaxSize = BufferMaxSize;
    };

    // Declared in bsonobj_comparator_interface.h.
    class ComparatorInterface;

    /**
     * Operator overloads for relops return a DeferredComparison which can subsequently be evaluated
     * by a BSONObj::ComparatorInterface.
     */
    using DeferredComparison = BSONComparatorInterfaceBase<BSONObj>::DeferredComparison;

    /**
     * Set of rules that dictate the behavior of the comparison APIs.
     */
    using ComparisonRules = BSONComparatorInterfaceBase<BSONObj>::ComparisonRules;
    using ComparisonRulesSet = BSONComparatorInterfaceBase<BSONObj>::ComparisonRulesSet;

    static constexpr char kMinBSONLength = 5;
    static const BSONObj kEmptyObject;

    /**
     * Construct an empty BSONObj -- that is, {}.
     */
    BSONObj() {
        // Little endian ordering here, but that is ok regardless as BSON is spec'd to be
        // little endian external to the system. (i.e. the rest of the implementation of
        // bson, not this part, fails to support big endian)
        _objdata = kEmptyObjectPrototype;
    }

    /**
     * Construct a BSONObj from data in the proper format.
     *  Use this constructor when something else owns bsonData's buffer
     */
    template <typename Traits = DefaultSizeTrait>
    explicit BSONObj(const char* bsonData, Traits t = Traits{}) {
        init<Traits>(bsonData);
    }

    explicit BSONObj(ConstSharedBuffer ownedBuffer)
        : _objdata(ownedBuffer.get() ? ownedBuffer.get() : BSONObj().objdata()),
          _ownedBuffer(std::move(ownedBuffer)) {}

    /**
     * Move construct a BSONObj
     */
    BSONObj(BSONObj&& other) noexcept
        : _objdata(std::move(other._objdata)), _ownedBuffer(std::move(other._ownedBuffer)) {
        other._objdata = BSONObj()._objdata;  // To return to an empty state.
        dassert(!other.isOwned());
    }

    // The explicit move constructor above will inhibit generation of the copy ctor, so
    // explicitly request the default implementation.

    /**
     * Copy construct a BSONObj.
     */
    BSONObj(const BSONObj&) = default;

    /**
     * Provide assignment semantics. We use the value taking form so that we can use copy
     * and swap, and consume both lvalue and rvalue references.
     */
    BSONObj& operator=(BSONObj otherCopy) noexcept {
        this->swap(otherCopy);
        return *this;
    }

    /**
     * Swap this BSONObj with 'other'
     */
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

    /**
     * If the data buffer is under the control of this BSONObj, return it.
     * Else return an owned copy.
     */
    BSONObj getOwned() const;

    /**
     * Returns an owned copy of the given BSON object.
     */
    static BSONObj getOwned(const BSONObj& obj);

    /**
     * @return a new full (and owned) copy of the object.
     */
    BSONObj copy() const;

    /**
     * If the data buffer is not under the control of this BSONObj, allocate
     * a separate copy and make this object a fully owned one.
     */
    void makeOwned();

    enum class RedactLevel : int8_t { all, encryptedAndSensitive, sensitiveOnly };

    /**
     * @return a new full (and owned) redacted copy of the object.
     */
    BSONObj redact(
        RedactLevel level = RedactLevel::all,
        std::function<std::string(const BSONElement&)> fieldNameRedactor = nullptr) const;

    /**
     * Readable representation of a BSON object in an extended JSON-style notation.
     * This is an abbreviated representation which might be used for logging.
     */
    enum { maxToStringRecursionDepth = 100 };

    std::string toString(bool redactValues = false) const;
    void toString(StringBuilder& s,
                  bool isArray = false,
                  bool full = false,
                  bool redactValues = false,
                  int depth = 0) const;

    /**
     * Properly formatted JSON string.
     * @param pretty if true we try to add some lf's and indentation
     */
    std::string jsonString(JsonStringFormat format = ExtendedCanonicalV2_0_0,
                           int pretty = 0,
                           bool isArray = false,
                           size_t writeLimit = 0,
                           BSONObj* outTruncationResult = nullptr) const;

    BSONObj jsonStringBuffer(JsonStringFormat format,
                             int pretty,
                             bool isArray,
                             fmt::memory_buffer& buffer,
                             size_t writeLimit = 0) const;

    BSONObj jsonStringGenerator(ExtendedCanonicalV200Generator const& generator,
                                int pretty,
                                bool isArray,
                                fmt::memory_buffer& buffer,
                                size_t writeLimit = 0) const;
    BSONObj jsonStringGenerator(ExtendedRelaxedV200Generator const& generator,
                                int pretty,
                                bool isArray,
                                fmt::memory_buffer& buffer,
                                size_t writeLimit = 0) const;
    BSONObj jsonStringGenerator(LegacyStrictGenerator const& generator,
                                int pretty,
                                bool isArray,
                                fmt::memory_buffer& buffer,
                                size_t writeLimit = 0) const;

    /**
     * Add specific field to the end of the object if it did not exist, otherwise replace it
     * preserving original field order. Returns newly built object. Returns copy of this for empty
     * field.
     */
    BSONObj addField(const BSONElement& field) const;

    /**
     * Merges the specified 'fields' from the 'from' object into the current BSON and returns the
     * merged object. If the 'fields' is not specified, all the fields from the 'from' object are
     * merged.
     *
     * Note that if the original object already has a particular field, then the field will be
     * replaced.
     */
    BSONObj addFields(const BSONObj& from,
                      const boost::optional<StringDataSet>& fields = boost::none) const;

    /**
     * Remove specified field and return a new object with the remaining fields.
     * slowish as builds a full new object
     */
    BSONObj removeField(StringData name) const;

    /**
     * Remove specified fields and return a new object with the remaining fields.
     */
    BSONObj removeFields(const std::set<std::string>& fields) const;
    BSONObj removeFields(const StringDataSet& fields) const;

    /**
     * Returns # of top level fields in the object
     * note: iterates to count the fields
     */
    int nFields() const;

    /**
     * Returns a 'Container' populated with the field names of the object.
     */
    template <class Container>
    Container getFieldNames() const;

    /**
     * Get the field of the specified name. eoo() is true on the returned
     * element if not found.
     */
    BSONElement getField(StringData name) const;

    /**
     * Get several fields at once. This is faster than separate getField() calls as the size of
     * elements iterated can then be calculated only once each.
     * @param n number of fieldNames, and number of elements in the fields array
     * @param fields if a field is found its element is stored in its corresponding position in
     *          this array. if not found the array element is unchanged.
     */
    void getFields(unsigned n, const char** fieldNames, BSONElement* fields) const;

    /**
     * Get several fields at once. This is faster than separate getField() calls as the size of
     * elements iterated can then be calculated only once each.
     */
    template <size_t N>
    void getFields(const std::array<StringData, N>& fieldNames,
                   std::array<BSONElement, N>* fields) const;


    /**
     * Get the field of the specified name. eoo() is true on the returned
     * element if not found.
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

    /**
     * @return true if field exists
     */
    bool hasField(StringData name) const {
        return !getField(name).eoo();
    }
    /**
     * @return true if field exists
     */
    bool hasElement(StringData name) const {
        return hasField(name);
    }

    /**
     * Looks up the element with the given 'name'. If the element is a string,
     * returns it as a StringData. Otherwise returns an empty StringData.
     */
    StringData getStringField(StringData name) const;

    /**
     * @return subobject of the given name
     */
    BSONObj getObjectField(StringData name) const;

    /**
     * @return INT_MIN if not present - does some type conversions
     */
    int getIntField(StringData name) const;

    /**
     * @return false if not present
     * @see BSONElement::trueValue()
     */
    bool getBoolField(StringData name) const;

    /**
     * @param pattern a BSON obj indicating a set of (un-dotted) field
     * names.  Element values are ignored.
     * @return a BSON obj constructed by taking the elements of this obj
     * that correspond to the fields in pattern. Field names of the
     * returned object are replaced with the empty string. If field in
     * pattern is missing, it is omitted from the returned object.
     *
     * Example: if this = {a : 4 , b : 5 , c : 6})
     *   this.extractFieldsUnDotted({a : 1 , c : 1}) -> {"" : 4 , "" : 6 }
     *   this.extractFieldsUnDotted({b : "blah"}) -> {"" : 5}
     */
    BSONObj extractFieldsUndotted(const BSONObj& pattern) const;
    void extractFieldsUndotted(BSONObjBuilder* b, const BSONObj& pattern) const;

    BSONObj filterFieldsUndotted(const BSONObj& filter, bool inFilter) const;
    void filterFieldsUndotted(BSONObjBuilder* b, const BSONObj& filter, bool inFilter) const;

    BSONElement getFieldUsingIndexNames(StringData fieldName, const BSONObj& indexKey) const;

    /**
     * arrays are bson objects with numeric and increasing field names
     * @return true if field names are numeric and increasing
     */
    bool couldBeArray() const;

    /**
     * @return the raw data of the object
     */
    const char* objdata() const {
        return _objdata;
    }

    /**
     * @return total size of the BSON object in bytes
     */
    int objsize() const {
        return ConstDataView(objdata()).read<LittleEndian<int>>();
    }

    /**
     * performs a cursory check on the object's size only.
     */
    template <typename Traits = DefaultSizeTrait>
    bool isValid() const {
        static_assert(Traits::MaxSize > 0 && Traits::MaxSize <= std::numeric_limits<int>::max(),
                      "BSONObj maximum size must be within possible limits");
        int x = objsize();
        return x > 0 && x <= Traits::MaxSize;
    }

    /**
     * Validates that the element is okay to be stored in a collection.
     * Recursively validates children.
     */
    Status storageValidEmbedded() const;

    /**
     * @return true if object is empty -- i.e.,  {}
     */
    bool isEmpty() const {
        return objsize() <= kMinBSONLength;
    }

    /**
     * Whether this BSONObj is the "empty prototype" special case.
     */
    bool isEmptyPrototype() const {
        return _objdata == kEmptyObjectPrototype;
    }

    /**
     * Alternative output format
     */
    std::string hexDump() const;

    //
    // Comparison API.
    //
    // BSONObj instances can be compared either using woCompare() or via operator overloads. Most
    // callers should prefer operator overloads. Note that the operator overloads return a
    // DeferredComparison, which must be subsequently evaluated by a BSONObj::ComparatorInterface.
    // See bsonobj_comparator_interface.h for details.
    //

    /**
     * Compares two BSON Objects using the rules specified by 'rules', 'comparator' for
     * string comparisons, and 'o' for ascending vs. descending ordering.
     *
     * Returns <0 if 'this' is less than 'obj'.
     *         >0 if 'this' is greater than 'obj'.
     *          0 if 'this' is equal to 'obj'.
     */
    int woCompare(const BSONObj& r,
                  const Ordering& o,
                  ComparisonRulesSet rules = ComparisonRules::kConsiderFieldName,
                  const StringDataComparator* comparator = nullptr) const;

    int woCompare(const BSONObj& r,
                  const BSONObj& ordering = BSONObj(),
                  ComparisonRulesSet rules = ComparisonRules::kConsiderFieldName,
                  const StringDataComparator* comparator = nullptr) const;

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

    /**
     * This is "shallow equality" -- ints and doubles won't match.  for a
     * deep equality test use woCompare (which is slower).
     */
    bool binaryEqual(const BSONObj& r) const {
        int os = objsize();
        if (os == r.objsize()) {
            return (os == 0 || memcmp(objdata(), r.objdata(), os) == 0);
        }
        return false;
    }

    /**
     * @return first field of the object
     */
    BSONElement firstElement() const {
        return BSONElement(objdata() + 4);
    }

    /**
     * faster than firstElement().fieldName() - for the first element we can easily find the
     * fieldname without computing the element size.
     */
    const char* firstElementFieldName() const {
        const char* p = objdata() + 4;
        return *p == EOO ? "" : p + 1;
    }

    StringData firstElementFieldNameStringData() const {
        return StringData(firstElementFieldName());
    }

    BSONType firstElementType() const {
        const char* p = objdata() + 4;
        return (BSONType)*p;
    }

    /**
     * Get the _id field from the object.  For good performance drivers should
     * assure that _id is the first element of the object; however, correct operation
     * is assured regardless.
     * @return true if found
     */
    bool getObjectID(BSONElement& e) const;

    /**
     * Return a version of this object where top level elements of types
     * that are not part of the bson wire protocol are replaced with
     * std::string identifier equivalents.
     * TODO Support conversion of element types other than min and max.
     */
    BSONObj clientReadable() const;

    /**
     * Return new object with the field names replaced by those in the
     * passed object.
     */
    BSONObj replaceFieldNames(const BSONObj& obj) const;

    static BSONObj stripFieldNames(const BSONObj& obj);

    bool hasFieldNames() const;

    /**
     * add all elements of the object to the specified vector
     */
    void elems(std::vector<BSONElement>&) const;
    /**
     * add all elements of the object to the specified list
     */
    void elems(std::list<BSONElement>&) const;

    friend class BSONObjIterator;
    friend class BSONObjStlIterator;
    typedef BSONObjStlIterator iterator;
    typedef BSONObjStlIterator const_iterator;

    /**
     * These enable range-based for loops over BSONObjs:
     *
     *      for (BSONElement elem : BSON("a" << 1 << "b" << 2)) {
     *          ... // Do something with elem
     *      }
     *
     * You can also loop over a bson object as-if it were a map<StringData, BSONElement>:
     *
     *      for (auto [fieldName, elem] : BSON("a" << 1 << "b" << 2)) {
     *          ... // Do something with fieldName and elem
     *      }
     */
    iterator begin() const;
    iterator end() const;

    void appendSelfToBufBuilder(BufBuilder& b) const {
        MONGO_verify(objsize());
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
    static constexpr char kEmptyObjectPrototype[] = {/*size*/ kMinBSONLength, 0, 0, 0, /*eoo*/ 0};

    template <typename Generator>
    BSONObj _jsonStringGenerator(const Generator& g,
                                 int pretty,
                                 bool isArray,
                                 fmt::memory_buffer& buffer,
                                 size_t writeLimit) const;

    void _assertInvalid(int maxSize) const;

    template <typename Traits = DefaultSizeTrait>
    void init(const char* data) {
        _objdata = data;
        if (!isValid<Traits>())
            _assertInvalid(Traits::MaxSize);
    }

    void _validateUnownedSize(int size) const;

    const char* _objdata;
    ConstSharedBuffer _ownedBuffer;
};

MONGO_STATIC_ASSERT(std::is_nothrow_move_constructible_v<BSONObj>);
MONGO_STATIC_ASSERT(std::is_nothrow_move_assignable_v<BSONObj>);

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

/**
 * An stl-compatible forward iterator over the elements of a BSONObj.
 *
 * The BSONObj must stay in scope for the duration of the iterator's execution.
 */
class BSONObjStlIterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using difference_type = ptrdiff_t;
    using value_type = BSONElement;
    using pointer = const BSONElement*;
    using reference = const BSONElement&;

    /**
     * All default constructed iterators are equal to each other.
     * They are in a dereferencable state, and return an EOO BSONElement.
     * They must not be incremented.
     */
    BSONObjStlIterator() = default;

    /**
     * Constructs an iterator pointing to the first element in obj or EOO if it is empty.
     */
    explicit BSONObjStlIterator(const BSONObj& obj) : BSONObjStlIterator(obj.firstElement()) {}

    /**
     * Returns an iterator pointing to the EOO element in obj.
     */
    static BSONObjStlIterator endOf(const BSONObj& obj) {
        auto eooElem = BSONElement();
        eooElem.data = (obj.objdata() + obj.objsize() - 1);
        dassert(eooElem.eoo());  // This is checked in the BSONObj constructor.
        return BSONObjStlIterator(eooElem);
    }

    /**
     * pre-increment
     */
    BSONObjStlIterator& operator++() {
        dassert(!_cur.eoo());
        *this = BSONObjStlIterator(BSONElement(_cur.rawdata() + _cur.size()));
        return *this;
    }

    /**
     * post-increment
     */
    BSONObjStlIterator operator++(int) {
        BSONObjStlIterator oldPos = *this;
        ++*this;
        return oldPos;
    }

    const BSONElement& operator*() const {
        return _cur;
    }
    const BSONElement* operator->() const {
        return &_cur;
    }

    friend bool operator==(const BSONObjStlIterator& lhs, const BSONObjStlIterator& rhs) {
        return lhs._cur.rawdata() == rhs._cur.rawdata();
    }
    friend bool operator!=(const BSONObjStlIterator& lhs, const BSONObjStlIterator& rhs) {
        return !(lhs == rhs);
    }

private:
    explicit BSONObjStlIterator(BSONElement elem) : _cur(elem) {}

    BSONElement _cur;
};

/**
 * Non-STL iterator for a BSONObj
 *
 * For simple loops over BSONObj, do this instead: for (auto&& elem : obj) { ... }
 *
 * Note each BSONObj ends with an EOO element: so you will get moreWithEOO() on an empty
 * object, although more() will be false and next().eoo() will be true.
 *
 * The BSONObj must stay in scope for the duration of the iterator's execution.
 */
class BSONObjIterator {
public:
    /**
     * Create an iterator for a BSON object.
     */
    explicit BSONObjIterator(const BSONObj& jso) {
        int sz = jso.objsize();
        if (MONGO_unlikely(sz == 0)) {
            _pos = _theend = nullptr;
            return;
        }
        _pos = jso.objdata() + 4;
        _theend = jso.objdata() + sz - 1;
    }

    BSONObjIterator(const char* start, const char* end) {
        _pos = start + 4;
        _theend = end - 1;
    }

    /**
     * Advance '_pos' by currentElement.size(). The element passed in must be equivalent to the
     * current element '_pos' is at.
     */
    void advance(const BSONElement& currentElement) {
        dassert(BSONElement(_pos).size() == currentElement.size());
        _pos += currentElement.size();
    }

    /**
     * Return true if the current element is equal to 'otherElement'.
     * Do *not* use with moreWithEOO() as the function will return false if the current element and
     * 'otherElement' are EOO.
     */
    bool currentElementBinaryEqual(const BSONElement& otherElement) {
        auto sz = otherElement.size();
        return sz <= (_theend - _pos) && memcmp(otherElement.rawdata(), _pos, sz) == 0;
    }

    /**
     * @return true if more elements exist to be enumerated.
     */
    bool more() const {
        return _pos < _theend;
    }

    /**
     * @return true if more elements exist to be enumerated INCLUDING the EOO element which is
     * always at the end.
     */
    bool moreWithEOO() const {
        return _pos <= _theend;
    }

    BSONElement next() {
        MONGO_verify(_pos <= _theend);
        BSONElement e(_pos);
        _pos += e.size();
        return e;
    }

    /**
     * pre-increment
     */
    BSONObjIterator& operator++() {
        next();
        return *this;
    }

    /**
     * post-increment
     */
    BSONObjIterator operator++(int) {
        BSONObjIterator oldPos = *this;
        next();
        return oldPos;
    }

    BSONElement operator*() {
        MONGO_verify(_pos <= _theend);
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

/**
 * Base class implementing ordered iteration through BSONElements.
 */
class BSONIteratorSorted {
    BSONIteratorSorted(const BSONIteratorSorted&) = delete;
    BSONIteratorSorted& operator=(const BSONIteratorSorted&) = delete;

public:
    ~BSONIteratorSorted() {
        MONGO_verify(_fields);
    }

    bool more() {
        return _cur < _nfields;
    }

    BSONElement next() {
        MONGO_verify(_fields);
        if (_cur < _nfields) {
            const auto& element = _fields[_cur++];
            return BSONElement(element.fieldName.rawData() - 1,  // Include type byte
                               element.fieldName.size() + 1,     // Add null terminator
                               element.totalSize);
        }

        return BSONElement();
    }

protected:
    class ElementFieldCmp;
    BSONIteratorSorted(const BSONObj& o, const ElementFieldCmp& cmp);

private:
    const int _nfields;
    struct Field {
        StringData fieldName;
        int totalSize;
    };
    const std::unique_ptr<Field[]> _fields;
    int _cur;
};

/**
 * Provides iteration of a BSONObj's BSONElements in lexical field order.
 */
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

inline BSONObj::iterator BSONObj::begin() const {
    return BSONObj::iterator(*this);
}
inline BSONObj::iterator BSONObj::end() const {
    return BSONObj::iterator::endOf(*this);
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
                       std::ptrdiff_t debug_offset) noexcept try {
        auto temp = BSONObj(ptr);
        auto len = temp.objsize();
        if (bson) {
            *bson = std::move(temp);
        }
        if (advanced) {
            *advanced = len;
        }
        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
    }

    static Status store(const BSONObj& bson,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset) noexcept;

    static BSONObj defaultConstruct() {
        return BSONObj();
    }
};

template <size_t N>
inline void BSONObj::getFields(const std::array<StringData, N>& fieldNames,
                               std::array<BSONElement, N>* fields) const {
    std::bitset<N> foundFields;
    for (auto&& el : *this) {
        auto fieldName = el.fieldNameStringData();
        for (std::size_t i = 0; i < N; ++i) {
            if (!foundFields.test(i) && (fieldNames[i] == fieldName)) {
                (*fields)[i] = std::move(el);
                foundFields.set(i);
                break;
            }
        }
        if (foundFields.all())
            break;
    }
}

template <class Container>
Container BSONObj::getFieldNames() const {
    Container fields;
    for (auto&& elem : *this) {
        if (elem.eoo())
            break;
        fields.insert(elem.fieldName());
    }
    return fields;
}

}  // namespace mongo
