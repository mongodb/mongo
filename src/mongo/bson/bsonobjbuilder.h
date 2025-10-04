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

#include "mongo/base/checked_cast.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/time_support.h"
#include "mongo/util/tracking/allocator.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/types.h>

namespace mongo {

class BSONObjBuilder;

namespace bsonobjbuilder_details {
/** Detects that a value can be appended via `builder.append("foo", val)`. */
template <typename Appendee, typename Builder = BSONObjBuilder>
concept HasBuilderAppendMemberFunction =
    requires(Builder& b, StringData name, Appendee val) { b.append(name, val); };

/**
 * Detects that a value can be appended via `appendToBson(builder, "foo", val)`.
 * Considers overloads found by ADL.
 */
template <typename Appendee, typename Builder = BSONObjBuilder>
concept HasAdlAppendToBson =
    requires(Builder& b, StringData name, Appendee val) { appendToBson(b, name, val); };
}  // namespace bsonobjbuilder_details

/**
 * CRTP Base class for BSONObj builder classes. Do not use this directly.
 *
 * Template arguments:
 * -Derived: The Derived class
 * -B: The buffer builder to use. See BufBuilder for an example.
 *
 * If adding a new subclass of this, an explicit template instantiation must be added in the cpp
 * file. Derived classes also must implement a destructor which calls the base class _destruct()
 * method.
 */
template <class Derived, class B>
class BSONObjBuilderBase {
    BSONObjBuilderBase(const BSONObjBuilderBase<Derived, B>&) = delete;
    BSONObjBuilderBase& operator=(const BSONObjBuilderBase<Derived, B>&) = delete;

public:
    /** @param initsize this is just a hint as to the final size of the object */
    template <typename... BuilderArgs>
    BSONObjBuilderBase(int initsize = kDefaultSize, BuilderArgs&&... args)
        : _buf(initsize, std::forward<BuilderArgs>(args)...), _b(_buf) {
        // Skip over space for the object length. The length is filled in by _done.
        _b.skip(sizeof(int));

        // Reserve space for the EOO byte. This means _done() can't fail.
        _b.reserveBytes(1);
    }

    /** @param baseBuilder construct a BSONObjBuilder using an existing BufBuilder
     *  This is for more efficient adding of subobjects/arrays. See docs for subobjStart for
     *  example.
     */
    template <typename... BuilderArgs>
    BSONObjBuilderBase(B& baseBuilder, BuilderArgs&&... args)
        : _buf(0, std::forward<BuilderArgs>(args)...), _b(baseBuilder), _offset(baseBuilder.len()) {
        // Skip over space for the object length, which is filled in by _done. We don't need a
        // holder since we are a sub-builder, and some parent builder has already made the
        // reservation.
        _b.skip(sizeof(int));

        // Reserve space for the EOO byte. This means _done() can't fail.
        _b.reserveBytes(1);
    }

    // Tag for a special overload of BSONObjBuilder that allows the user to continue
    // building in to an existing BufBuilder that has already been built in to. Use with caution.
    struct ResumeBuildingTag {};

    template <typename... BuilderArgs>
    BSONObjBuilderBase(ResumeBuildingTag,
                       B& existingBuilder,
                       std::size_t offset = 0,
                       BuilderArgs&&... args)
        : _buf(0, std::forward<BuilderArgs>(args)...), _b(existingBuilder), _offset(offset) {
        invariant(_b.len() - offset >= BSONObj::kMinBSONLength);
        _b.setlen(_b.len() - 1);  // get rid of the previous EOO.
        // Reserve space for our EOO.
        _b.reserveBytes(1);
    }

    // Move constructible, but not assignable due to reference member.
    BSONObjBuilderBase(BSONObjBuilderBase&& other) noexcept
        : _buf(std::move(other._buf)),
          _b(&other._b == &other._buf ? _buf : other._b),
          _offset(std::move(other._offset)),
          _doneCalled(std::move(other._doneCalled)) {
        other.abandon();
    }

    Derived& asDerived() {
        return static_cast<Derived&>(*this);
    }

    const Derived& asDerived() const {
        return static_cast<const Derived&>(*this);
    }

    /**
     * The start offset of the object being built by this builder within its buffer.
     * Needed for the object-resuming constructor.
     */
    std::size_t offset() const {
        return _offset;
    }

    /** add all the fields from the object specified to this object */
    Derived& appendElements(const BSONObj& x);

    /**
     * Add all the fields from `x` to this object, replacing each field name with the corresponding
     * field name in the `newNames` object. If `keepTail` is set to true, and the `newNames` object
     * contains fewer entries than the `x` object, the fields in `x` that have no matching entry in
     * `newNames` will be added with their original field names. If `keepTail` is set to false,
     * these extra fields are not appended.
     */
    Derived& appendElementsRenamed(const BSONObj& x, const BSONObj& newNames, bool keepTail = true);

    /** add all the fields from the object specified to this object if they don't exist already */
    Derived& appendElementsUnique(const BSONObj& x);

    /** append element to the object we are building */
    Derived& append(const BSONElement& e) {
        // do not append eoo, that would corrupt us. the builder auto appends when done() is called.
        MONGO_verify(!e.eoo());
        _b.appendBuf((void*)e.rawdata(), e.size());
        return asDerived();
    }

    /** append an element but with a new name */
    Derived& appendAs(const BSONElement& e, StringData fieldName) {
        // do not append eoo, that would corrupt us. the builder auto appends when done() is called.
        MONGO_verify(!e.eoo());
        _b.appendNum((char)e.type());
        _b.appendCStr(fieldName);
        _b.appendBuf((void*)e.value(), e.valuesize());
        return asDerived();
    }

    /** add a subobject as a member */
    Derived& append(StringData fieldName, BSONObj subObj) {
        _b.appendNum((char)BSONType::object);
        _b.appendCStr(fieldName);
        _b.appendBuf((void*)subObj.objdata(), subObj.objsize());
        return asDerived();
    }

    /** add a subobject as a member */
    Derived& appendObject(StringData fieldName, const char* objdata, int size = 0) {
        MONGO_verify(objdata);
        if (size == 0) {
            size = ConstDataView(objdata).read<LittleEndian<int>>();
        }

        MONGO_verify(size > 4 && size < 100000000);

        _b.appendNum((char)BSONType::object);
        _b.appendCStr(fieldName);
        _b.appendBuf((void*)objdata, size);
        return asDerived();
    }

    /** add header for a new subobject and return bufbuilder for writing to
     *  the subobject's body
     *
     *  example:
     *
     *  BSONObjBuilder b;
     *  BSONObjBuilder sub (b.subobjStart("fieldName"));
     *  // use sub
     *  sub.done()
     *  // use b and convert to object
     */
    B& subobjStart(StringData fieldName) {
        _b.appendNum((char)BSONType::object);
        _b.appendCStr(fieldName);
        return _b;
    }

    /** add a subobject as a member with type Array.  Thus arr object should have "0", "1", ...
        style fields in it.
    */
    Derived& appendArray(StringData fieldName, const BSONObj& subObj) {
        _b.appendNum((char)BSONType::array);
        _b.appendCStr(fieldName);
        _b.appendBuf((void*)subObj.objdata(), subObj.objsize());
        return asDerived();
    }
    Derived& append(StringData fieldName, BSONArray arr) {
        return appendArray(fieldName, arr);
    }

    /** add header for a new subarray and return bufbuilder for writing to
        the subarray's body */
    B& subarrayStart(StringData fieldName) {
        _b.appendNum((char)BSONType::array);
        _b.appendCStr(fieldName);
        return _b;
    }

    /** Append a boolean element */
    Derived& appendBool(StringData fieldName, bool val) {
        _b.appendNum((char)BSONType::boolean);
        _b.appendCStr(fieldName);
        _b.appendNum(char(val));
        return asDerived();
    }

    /** Append elements that have the BSONObjAppendFormat trait */
    template <typename T, typename = std::enable_if_t<IsBSONObjAppendable<T>::value>>
    Derived& append(StringData fieldName, const T& n) {
        constexpr BSONType type = BSONObjAppendFormat<T>::value;
        _b.appendNum(static_cast<char>(type));
        _b.appendCStr(fieldName);
        if constexpr (type == BSONType::boolean) {
            _b.appendNum(static_cast<char>(n));
        } else if constexpr (type == BSONType::numberInt) {
            _b.appendNum(static_cast<int>(n));
        } else {
            _b.appendNum(n);
        }
        return asDerived();
    }

    template <typename T,
              typename = std::enable_if_t<!IsBSONObjAppendable<T>::value && std::is_integral_v<T>>,
              typename = void>
    Derived& append(StringData fieldName, const T& n) = delete;

    /**
     * appendNumber is a series of method for appending the smallest sensible type
     * mostly for JS
     */
    Derived& appendNumber(StringData fieldName, int n) {
        return append(fieldName, n);
    }

    Derived& appendNumber(StringData fieldName, double d) {
        return append(fieldName, d);
    }

    Derived& appendNumber(StringData fieldName, Decimal128 decNumber) {
        return append(fieldName, decNumber);
    }

    Derived& appendNumber(StringData fieldName, long long llNumber) {
        static const long long maxInt = std::numeric_limits<int>::max();
        static const long long minInt = std::numeric_limits<int>::min();

        if (minInt <= llNumber && llNumber <= maxInt) {
            append(fieldName, static_cast<int>(llNumber));
        } else {
            append(fieldName, llNumber);
        }
        return asDerived();
    }

    /** Append a BSON Object ID (OID type).
        @deprecated Generally, it is preferred to use the append append(name, oid)
        method for this.
    */
    Derived& appendOID(StringData fieldName, OID* oid = nullptr, bool generateIfBlank = false) {
        _b.appendNum((char)BSONType::oid);
        _b.appendCStr(fieldName);
        if (oid)
            _b.appendBuf(oid->view().view(), OID::kOIDSize);
        else {
            OID tmp;
            if (generateIfBlank)
                tmp.init();
            else
                tmp.clear();
            _b.appendBuf(tmp.view().view(), OID::kOIDSize);
        }
        return asDerived();
    }

    /**
    Append a BSON Object ID.
    @param fieldName Field name, e.g., "_id".
    @returns the builder object
    */
    Derived& append(StringData fieldName, OID oid) {
        _b.appendNum((char)BSONType::oid);
        _b.appendCStr(fieldName);
        _b.appendBuf(oid.view().view(), OID::kOIDSize);
        return asDerived();
    }

    /**
    Generate and assign an object id for the _id field.
    _id should be the first element in the object for good performance.
    */
    Derived& genOID() {
        return append("_id", OID::gen());
    }

    /** Append a time_t date.
        @param dt a C-style 32 bit date value, that is
        the number of seconds since January 1, 1970, 00:00:00 GMT
    */
    Derived& appendTimeT(StringData fieldName, time_t dt) {
        _b.appendNum((char)BSONType::date);
        _b.appendCStr(fieldName);
        _b.appendNum(static_cast<unsigned long long>(dt) * 1000);
        return asDerived();
    }
    /** Append a date.
        @param dt a Java-style 64 bit date value, that is
        the number of milliseconds since January 1, 1970, 00:00:00 GMT
    */
    Derived& appendDate(StringData fieldName, Date_t dt);
    Derived& append(StringData fieldName, Date_t dt) {
        return appendDate(fieldName, dt);
    }

    /** Append a regular expression value
        @param regex the regular expression pattern
        @param regex options such as "i" or "g"
    */
    Derived& appendRegex(StringData fieldName, StringData regex, StringData options = "") {
        _b.appendNum((char)BSONType::regEx);
        _b.appendCStr(fieldName);
        _b.appendCStr(regex);
        _b.appendCStr(options);
        return asDerived();
    }

    Derived& append(StringData fieldName, const BSONRegEx& regex) {
        return appendRegex(fieldName, regex.pattern, regex.flags);
    }

    Derived& appendCode(StringData fieldName, StringData code) {
        _b.appendNum((char)BSONType::code);
        _b.appendCStr(fieldName);
        _b.appendNum((int)code.size() + 1);
        _b.appendStrBytesAndNul(code);
        return asDerived();
    }

    Derived& append(StringData fieldName, const BSONCode& code) {
        return appendCode(fieldName, code.code);
    }

    /** Append a string element.
        @param sz size includes terminating null character */
    Derived& append(StringData fieldName, const char* str, int sz) {
        _b.appendNum((char)BSONType::string);
        _b.appendCStr(fieldName);
        _b.appendNum((int)sz);
        _b.appendBuf(str, sz);
        return asDerived();
    }
    /** Append a string element */
    Derived& append(StringData fieldName, const char* str) {
        return append(fieldName, str, (int)strlen(str) + 1);
    }
    /** Append a string element */
    Derived& append(StringData fieldName, StringData str) {
        _b.appendNum((char)BSONType::string);
        _b.appendCStr(fieldName);
        _b.appendNum((int)str.size() + 1);
        _b.appendStrBytesAndNul(str);
        return asDerived();
    }

    Derived& appendSymbol(StringData fieldName, StringData symbol) {
        _b.appendNum((char)BSONType::symbol);
        _b.appendCStr(fieldName);
        _b.appendNum((int)symbol.size() + 1);
        _b.appendStrBytesAndNul(symbol);
        return asDerived();
    }

    Derived& append(StringData fieldName, const BSONSymbol& symbol) {
        return appendSymbol(fieldName, symbol.symbol);
    }

    /** Append a Null element to the object */
    Derived& appendNull(StringData fieldName) {
        _b.appendNum((char)BSONType::null);
        _b.appendCStr(fieldName);
        return asDerived();
    }

    // Append an element that is less than all other keys.
    Derived& appendMinKey(StringData fieldName) {
        _b.appendNum((char)BSONType::minKey);
        _b.appendCStr(fieldName);
        return asDerived();
    }
    // Append an element that is greater than all other keys.
    Derived& appendMaxKey(StringData fieldName) {
        _b.appendNum((char)BSONType::maxKey);
        _b.appendCStr(fieldName);
        return asDerived();
    }

    // Append a Timestamp field -- will be updated to next server Timestamp
    Derived& appendTimestamp(StringData fieldName) {
        return append(fieldName, Timestamp());
    }

    Derived& appendTimestamp(StringData fieldName, unsigned long long val) {
        return append(fieldName, Timestamp(val));
    }

    /**
     * To store a Timestamp in BSON, use this function.
     * This captures both the secs and inc fields.
     */
    Derived& append(StringData fieldName, Timestamp timestamp) {
        timestamp.append<B>(_b, fieldName);
        return asDerived();
    }

    /*
    Append an element of the deprecated DBRef type.
    @deprecated
    */
    Derived& appendDBRef(StringData fieldName, StringData ns, const OID& oid) {
        _b.appendNum((char)BSONType::dbRef);
        _b.appendCStr(fieldName);
        _b.appendNum((int)ns.size() + 1);
        _b.appendStrBytesAndNul(ns);
        _b.appendBuf(oid.view().view(), OID::kOIDSize);

        return asDerived();
    }

    Derived& append(StringData fieldName, const BSONDBRef& dbref) {
        return appendDBRef(fieldName, dbref.ns, dbref.oid);
    }

    /** Append a binary data element
        @param fieldName name of the field
        @param len length of the binary data in bytes
        @param subtype subtype information for the data. @see enum BinDataType in bsontypes.h.
               Use BinDataGeneral if you don't care about the type.
        @param data the byte array
    */
    Derived& appendBinData(StringData fieldName, int len, BinDataType type, const void* data) {
        _b.appendNum((char)BSONType::binData);
        _b.appendCStr(fieldName);
        _b.appendNum(len);
        _b.appendNum((char)type);
        _b.appendBuf(data, len);

        return asDerived();
    }

    Derived& append(StringData fieldName, const BSONBinData& bd) {
        return appendBinData(fieldName, bd.length, bd.type, bd.data);
    }

    /**
    Subtype 2 is deprecated.
    Append a BSON bindata bytearray element.
    @param data a byte array
    @param len the length of data
    */
    Derived& appendBinDataArrayDeprecated(const char* fieldName, const void* data, int len) {
        _b.appendNum((char)BSONType::binData);
        _b.appendCStr(fieldName);
        _b.appendNum(len + 4);
        _b.appendNum((char)0x2);
        _b.appendNum(len);
        _b.appendBuf(data, len);

        return asDerived();
    }

    /** Append to the BSON object a field of type CodeWScope.  This is a javascript code
        fragment accompanied by some scope that goes with it.
    */
    Derived& appendCodeWScope(StringData fieldName, StringData code, const BSONObj& scope) {
        _b.appendNum((char)BSONType::codeWScope);
        _b.appendCStr(fieldName);
        _b.appendNum((int)(4 + 4 + code.size() + 1 + scope.objsize()));
        _b.appendNum((int)code.size() + 1);
        _b.appendStrBytesAndNul(code);
        _b.appendBuf((void*)scope.objdata(), scope.objsize());

        return asDerived();
    }

    Derived& append(StringData fieldName, const BSONCodeWScope& cws) {
        return appendCodeWScope(fieldName, cws.code, cws.scope);
    }

    Derived& appendUndefined(StringData fieldName) {
        _b.appendNum((char)BSONType::undefined);
        _b.appendCStr(fieldName);
        return asDerived();
    }

    /* helper function -- see Query::where() for primary way to do this. */
    Derived& appendWhere(StringData code, const BSONObj& scope) {
        return appendCodeWScope("$where", code, scope);
    }

    /**
       these are the min/max when comparing, not strict min/max elements for a given type
    */
    Derived& appendMinForType(StringData fieldName, int type);
    Derived& appendMaxForType(StringData fieldName, int type);

    /** Append an array of values. */
    template <class T>
    Derived& append(StringData fieldName, const std::vector<T>& vals);

    template <class T>
    Derived& append(StringData fieldName, const std::list<T>& vals);

    /** Append a set of values. */
    template <class T>
    Derived& append(StringData fieldName, const std::set<T>& vals);

    /**
     * Append a map of values as a sub-object.
     * Note: the keys of the map should be StringData-compatible (i.e. strings).
     */
    template <typename Map>
    requires std::is_convertible_v<decltype(std::declval<Map>().begin()->first), StringData>
    Derived& append(StringData fieldName, const Map& map) {
        typename std::remove_reference<Derived>::type bob;
        for (auto&& [k, v] : map) {
            bob.append(k, v);
        }

        append(fieldName, bob.obj());
        return asDerived();
    }

    /** Append a range of values between two iterators. */
    template <class It>
    Derived& append(StringData fieldName, It begin, It end);

    /**
     * Resets this BSONObjBulder to an empty state. All previously added fields are lost.  If this
     * BSONObjBuilderBase is using an externally provided BufBuilder, this method does not affect
     * the bytes before the start of this object.
     *
     * Invalid to call if done() has already been called in order to finalize the BSONObj.
     */
    void resetToEmpty() {
        invariant(!_doneCalled);
        // Reset the position the next write will go to right after our size reservation.
        _b.setlen(_offset + sizeof(int));
    }

    /** Fetch the object we have built.
        BSONObjBuilderBase still frees the object when the builder goes out of
        scope -- very important to keep in mind.  Use obj() if you
        would like the BSONObj to last longer than the builder.
    */
    template <typename BSONTraits = BSONObj::DefaultSizeTrait>
    BSONObj done() {
        return BSONObj(asDerived()._done(), BSONTraits{});
    }

    // Like 'done' above, but does not construct a BSONObj to return to the caller.
    void doneFast() {
        (void)asDerived()._done();
    }

    /** Peek at what is in the builder, but leave the builder ready for more appends.
        The returned object is only valid until the next modification or destruction of the builder.
        Intended use case: append a field if not already there.
    */
    BSONObj asTempObj() {
        const char* const buffer = asDerived()._done();

        // None of the code which resets this builder to the not-done state is expected to throw.
        // If it does, that would be a violation of our expectations.
        ScopeGuard resetObjectState([this]() {
            // Immediately after the buffer for the ephemeral space created by the call to `_done()`
            // is ready, reset our state to not-done.
            _doneCalled = false;

            _b.setlen(_b.len() - 1);  // next append should overwrite the EOO
            _b.reserveBytes(1);       // Rereserve room for the real EOO
        });

        return BSONObj(buffer, BSONObj::LargeSizeTrait());
    }

    /** Make it look as if "done" has been called, so that our destructor is a no-op. Do
     *  this if you know that you don't care about the contents of the builder you are
     *  destroying.
     *
     *  Note that it is invalid to call any method other than the destructor after invoking
     *  this method.
     */
    void abandon() {
        _doneCalled = true;
    }

    bool isArray() const {
        return false;
    }

    /** @return true if we are using our own bufbuilder, and not an alternate that was given to us
     * in our constructor */
    bool owned() const {
        return &_b == &_buf;
    }

    BSONObjIterator iterator() const;

    bool hasField(StringData name) const;

    int len() const {
        return _b.len();
    }

    B& bb() {
        return _b;
    }

protected:
    constexpr static size_t kDefaultSize = 512;

    // Initializes the builder without allocating any space. Only used by subclasses.
    struct InitEmptyTag {};
    template <typename... BuilderArgs>
    BSONObjBuilderBase(InitEmptyTag, BuilderArgs&&... args)
        : _buf(0, std::forward<BuilderArgs>(args)...), _b(_buf) {}

    // Intentionally non-virtual.
    ~BSONObjBuilderBase() {
        // It is the derived class's responsibility to ensure that done() is called.
        invariant(!needsDone());
    }

    char* _done() {
        if (_doneCalled)
            return _b.buf() + _offset;

        _b.claimReservedBytes(1);  // Prevents adding EOO from failing.
        _b.appendNum((char)BSONType::eoo);

        char* data = _b.buf() + _offset;
        int size = _b.len() - _offset;
        DataView(data).write(tagLittleEndian(size));

        // Only set `_doneCalled` to true when all functions which could throw haven't thrown.
        _doneCalled = true;
        return data;
    }

    bool needsDone() const {
        // If 'done' has not already been called, and we have a reference to an owning
        // BufBuilder but do not own it ourselves, then we must call _done to write in the
        // length. Otherwise, we own this memory and its lifetime ends with us, therefore
        // we can elide the write.
        return !_doneCalled && _b.buf() && _buf.capacity() == 0;
    }

    // Must be called by derived class destructors.
    void _destruct() {
        if (needsDone()) {
            _done();
        }
    }

    B _buf;
    B& _b;
    int _offset = 0;
    bool _doneCalled = false;
};

// The following forward declaration exists to enable the extern
// declaration, which must come before the use of the matching
// instantiation of the base class of BSONObjBuilder. Do not remove or
// re-order these lines w.r.t BSONObjBuilderBase or BSONObjBuilder
// without being sure that you are not undoing the advantages of the
// extern template declaration.
class BSONObjBuilder;

extern template class BSONObjBuilderBase<BSONObjBuilder, BufBuilder>;

// BSONObjBuilder needs this forward declared in order to declare the
// ArrayBuilder typedef. This forward declaration is also required to
// allow one of the extern template declarations for
// BSONArrayBuilderBase below.
class BSONArrayBuilder;

/**
 * "Standard" class used for constructing BSONObj on the fly. Stores the BSON in a refcounted
 * buffer.
 */
class BSONObjBuilder : public BSONObjBuilderBase<BSONObjBuilder, BufBuilder> {
private:
    using Super = BSONObjBuilderBase<BSONObjBuilder, BufBuilder>;

public:
    using ArrayBuilder = BSONArrayBuilder;

    class ValueStream {
    public:
        ValueStream(StringData fieldName, BSONObjBuilder* builder)
            : _fieldName{fieldName}, _builder{builder} {}

        ValueStream(const ValueStream&&) = delete;
        ValueStream&& operator=(ValueStream&&) = delete;

        /**
         * `subobjStart` and `subarrayStart` provide API compatibility with `BSONArrayBuilder`.
         * This allows generic code to start a nested object with no fieldName.
         */
        BufBuilder& subobjStart() {
            return _builder->subobjStart(_fieldName);
        }

        BufBuilder& subarrayStart() {
            return _builder->subarrayStart(_fieldName);
        }

        BSONObjBuilder& builder() const {
            return *_builder;
        }

        StringData fieldName() const {
            return _fieldName;
        }

        BSONObjBuilder& operator<<(const BSONElement& e) const {
            return _builder->appendAs(e, _fieldName);
        }

        BSONObjBuilder& operator<<(
            const bsonobjbuilder_details::HasAdlAppendToBson auto& value) const {
            appendToBson(builder(), fieldName(), value);
            return builder();
        }

    private:
        StringData _fieldName;
        BSONObjBuilder* _builder;
    };

    using Super::Super;

    /**
     * Creates a new BSONObjBuilder prefixed with the fields in 'prefix'.
     *
     * If prefix is an rvalue referring to the only view of the underlying BSON buffer, it will be
     * able to avoid copying and will just reuse the buffer. Therefore, you should try to std::move
     * into this constructor where possible.
     */
    BSONObjBuilder(BSONObj prefix) : Super(Super::InitEmptyTag{}) {
        // If prefix wasn't owned or we don't have exclusive access to it, we must copy.
        if (!prefix.isOwned() || prefix.sharedBuffer().isShared()) {
            _b.grow(prefix.objsize());  // Make sure we won't need to realloc().
            _b.setlen(sizeof(int));     // Skip over size bytes (see first constructor).
            _b.reserveBytes(1);         // Reserve room for our EOO byte.
            appendElements(prefix);
            return;
        }

        const auto size = prefix.objsize();
        const char* const firstByte = prefix.objdata();
        auto buf = prefix.releaseSharedBuffer().constCast();
        _offset = firstByte - buf.get();
        _b.useSharedBuffer(std::move(buf));
        _b.setlen(_offset + size - 1);  // Position right before prefix's EOO byte.
        _b.reserveBytes(1);             // Reserve room for our EOO byte.
    }

    BSONObjBuilder(BSONObjBuilder&& other) noexcept : Super(std::move(other)) {}

    ~BSONObjBuilder() {
        Super::_destruct();
    }

    /**
     * destructive
     * The returned BSONObj will free the buffer when it is finished.
     * @return owned BSONObj
     */
    template <typename BSONTraits = BSONObj::DefaultSizeTrait>
    BSONObj obj() {
        massert(10335, "builder does not own memory", owned());
        auto out = done<BSONTraits>();
        out.shareOwnershipWith(_b.release());
        return out;
    }

    /** Stream oriented way to add field names and values. */
    template <typename T>
    BSONObjBuilder& operator<<(const BSONFieldValue<T>& v) {
        append(v.name(), v.value());
        return *this;
    }

    BSONObjBuilder& operator<<(const BSONElement& e) {
        append(e);
        return *this;
    }

    ValueStream operator<<(StringData name) {
        return BSONObjBuilder::ValueStream{name, this};
    }

    template <typename T>
    ValueStream operator<<(const BSONField<T>& f) {
        return ValueStream(f.name(), this);
    }

    /**
     * The fallback overload for appendToBson.
     * This just tries `bob.append(fieldName, value)` as a default behavior.
     * ADL is used to find overloads that might be associated with the type of `value`.
     * So customize this behavior by defining a "hidden friend" of the value type, or similarly
     * providing an ADL-compatible overload.
     */
    friend void appendToBson(
        BSONObjBuilder& bob,
        StringData fieldName,
        const bsonobjbuilder_details::HasBuilderAppendMemberFunction auto& value) {
        bob.append(fieldName, value);
    }
};
namespace allocator_aware {
// The following forward declaration exists to enable the extern declaration, which must come before
// the use of the matching instantiation of the base class of allocator_aware::BSONObjBuilder. Do
// not remove or re-order these lines w.r.t BSONObjBuilderBase or allocator_aware::BSONObjBuilder
// without being sure that you are not undoing the advantages of the extern template declaration.
template <class>
class BSONObjBuilder;
}  // namespace allocator_aware

extern template class BSONObjBuilderBase<allocator_aware::BSONObjBuilder<std::allocator<void>>,
                                         allocator_aware::BufBuilder<std::allocator<void>>>;
extern template class BSONObjBuilderBase<allocator_aware::BSONObjBuilder<tracking::Allocator<void>>,
                                         allocator_aware::BufBuilder<tracking::Allocator<void>>>;

namespace allocator_aware {
template <class Allocator = std::allocator<void>>
class BSONObjBuilder : public BSONObjBuilderBase<BSONObjBuilder<Allocator>, BufBuilder<Allocator>> {
    using Super = BSONObjBuilderBase<BSONObjBuilder<Allocator>, BufBuilder<Allocator>>;

public:
    BSONObjBuilder() = default;

    explicit BSONObjBuilder(const Allocator& allocator) : Super(Super::kDefaultSize, allocator) {}

    explicit BSONObjBuilder(BufBuilder<Allocator>& builder) : Super(builder, builder.allocator()) {}

    ~BSONObjBuilder() {
        Super::_destruct();
    }
};
}  // namespace allocator_aware

// The following forward declaration exists to enable the extern
// declaration, which must come before the use of the matching
// instantiation of the base class of UniqueBSONObjBuilder. Do not
// remove or re-order these lines w.r.t BSONObjBuilderBase or
// UniqueBSONObjBuilder without being sure that you are not undoing
// the advantages of the extern template declaration.
class UniqueBSONObjBuilder;
extern template class BSONObjBuilderBase<UniqueBSONObjBuilder, UniqueBufBuilder>;

// UniqueBSONObjBuilder needs this forward declared in order to
// declare the ArrayBuilder typedef. This forward declaration is also
// required to allow one of the extern template declarations for
// BSONArrayBuilderBase below.
class UniqueBSONArrayBuilder;

/**
 * Alternative to BSONObjBuilder which uses a non-refcounted buffer (UniqueBuffer) instead of a
 * refcounted buffer (SharedBuffer).
 *
 * This should only be used when you care about having direct ownership over the BSONObj's
 * underlying memory.
 */
class UniqueBSONObjBuilder : public BSONObjBuilderBase<UniqueBSONObjBuilder, UniqueBufBuilder> {
private:
    using Super = BSONObjBuilderBase<UniqueBSONObjBuilder, UniqueBufBuilder>;

public:
    using Super::BSONObjBuilderBase;
    using ArrayBuilder = UniqueBSONArrayBuilder;

    /**
     * Creates a new UniqueBSONObjBuilder prefixed with the fields in 'prefix'.
     */
    UniqueBSONObjBuilder(BSONObj prefix) : Super(Super::InitEmptyTag{}) {
        _b.grow(prefix.objsize());  // Make sure we won't need to realloc().
        _b.setlen(sizeof(int));     // Skip over size bytes (see first constructor).
        _b.reserveBytes(1);         // Reserve room for our EOO byte.
        appendElements(prefix);
    }

    UniqueBSONObjBuilder(UniqueBSONObjBuilder&&) = default;
    UniqueBSONObjBuilder(const UniqueBSONObjBuilder&) = delete;
    UniqueBSONObjBuilder& operator=(const UniqueBSONObjBuilder&) = delete;

    ~UniqueBSONObjBuilder() {
        Super::_destruct();
    }

    /**
     * destructive
     * The returned BSONObj will free the buffer when it is finished.
     */
    template <typename BSONTraits = BSONObj::DefaultSizeTrait>
    BSONObj obj() {
        massert(5318300, "builder does not own memory", owned());
        auto out = done<BSONTraits>();
        out.shareOwnershipWith(SharedBuffer(_b.release()));
        return out;
    }
};

/**
 * Base class for building BSON arrays. Similar to BSONObjBuilderBase.
 */
template <class Derived, class BSONObjBuilderType>
class BSONArrayBuilderBase {
public:
    template <typename... BuilderArgs>
    BSONArrayBuilderBase(BuilderArgs&&... args) : _b(std::forward<BuilderArgs>(args)...) {}

    template <typename... BuilderArgs>
    BSONArrayBuilderBase(int initialSize, BuilderArgs&&... args)
        : _b(initialSize, std::forward<BuilderArgs>(args)...) {}

    Derived& asDerived() {
        return static_cast<Derived&>(*this);
    }
    const Derived& asDerived() const {
        return static_cast<const Derived&>(*this);
    }

    template <typename T>
    Derived& append(const T& x) {
        _b.append(_fieldCount, x);
        ++_fieldCount;
        return asDerived();
    }

    Derived& append(const BSONElement& e) {
        _b.appendAs(e, _fieldCount);
        ++_fieldCount;
        return asDerived();
    }

    Derived& operator<<(const BSONElement& e) {
        return append(e);
    }

    template <typename T>
    Derived& operator<<(const T& x) {
        _b << _fieldCount << x;
        ++_fieldCount;
        return asDerived();
    }

    Derived& appendNull() {
        _b.appendNull(_fieldCount);
        ++_fieldCount;
        return asDerived();
    }

    Derived& appendUndefined() {
        _b.appendUndefined(_fieldCount);
        ++_fieldCount;
        return asDerived();
    }

    Derived& appendMinKey() {
        _b.appendMinKey(_fieldCount);
        ++_fieldCount;
        return asDerived();
    }

    Derived& appendMaxKey() {
        _b.appendMaxKey(_fieldCount);
        ++_fieldCount;
        return asDerived();
    }

    BSONObj done() {
        return _b.done();
    }

    void doneFast() {
        _b.doneFast();
    }

    template <class T>
    Derived& append(const std::list<T>& vals);

    template <class T>
    Derived& append(const std::set<T>& vals);

    template <class It>
    Derived& append(It begin, It end);

    // These two just use next position
    auto& subobjStart() {
        return _b.subobjStart(_fieldCount++);
    }
    auto& subarrayStart() {
        return _b.subarrayStart(_fieldCount++);
    }

    Derived& appendRegex(StringData regex, StringData options = "") {
        _b.appendRegex(_fieldCount, regex, options);
        ++_fieldCount;
        return asDerived();
    }

    Derived& appendBinData(int len, BinDataType type, const void* data) {
        _b.appendBinData(_fieldCount, len, type, data);
        ++_fieldCount;
        return asDerived();
    }

    Derived& appendCode(StringData code) {
        _b.appendCode(_fieldCount, code);
        ++_fieldCount;
        return asDerived();
    }

    Derived& appendCodeWScope(StringData code, const BSONObj& scope) {
        _b.appendCodeWScope(_fieldCount, code, scope);
        ++_fieldCount;
        return asDerived();
    }

    Derived& appendTimeT(time_t dt) {
        _b.appendTimeT(_fieldCount, dt);
        ++_fieldCount;
        return asDerived();
    }

    Derived& appendDate(Date_t dt) {
        _b.appendDate(_fieldCount, dt);
        ++_fieldCount;
        return asDerived();
    }

    Derived& appendBool(bool val) {
        _b.appendBool(_fieldCount, val);
        ++_fieldCount;
        return asDerived();
    }

    Derived& appendTimestamp(unsigned long long ts) {
        _b.appendTimestamp(_fieldCount, ts);
        ++_fieldCount;
        return asDerived();
    }

    bool isArray() const {
        return true;
    }

    int len() const {
        return _b.len();
    }
    int arrSize() const {
        return _fieldCount;
    }

    auto& bb() {
        return _b.bb();
    }

protected:
    template <class BufBuilderType>
    BSONArrayBuilderBase(BufBuilderType& builder) : _b(builder) {}

    DecimalCounter<uint32_t> _fieldCount;
    BSONObjBuilderType _b;
};

// The following extern template declaration must come after the
// forward declaration of BSONArrayBuilder above, and before the use
// of the matching instantiation of the base class of
// BSONArrayBuilder. Do not remove or re-order these lines w.r.t
// BSONArrayBuilderBase or BSONArrayBuilder without being sure that
// you are not undoing the advantages of the extern template
// declaration.
extern template class BSONArrayBuilderBase<BSONArrayBuilder, BSONObjBuilder>;

/**
 * "Standard" class used for building BSON arrays.
 */
class BSONArrayBuilder : public BSONArrayBuilderBase<BSONArrayBuilder, BSONObjBuilder> {
public:
    using ObjBuilder = BSONObjBuilder;

    using BSONArrayBuilderBase<BSONArrayBuilder, BSONObjBuilder>::BSONArrayBuilderBase;
    BSONArrayBuilder(BufBuilder& bufBuilder)
        : BSONArrayBuilderBase<BSONArrayBuilder, BSONObjBuilder>(bufBuilder) {}

    /**
     * destructive - ownership moves to returned BSONArray
     * @return owned BSONArray
     */
    BSONArray arr() {
        return BSONArray(_b.obj());
    }
    BSONObj obj() {
        return _b.obj();
    }
};

namespace allocator_aware {
// The following forward declaration exists to enable the extern declaration, which must come before
// the use of the matching instantiation of the base class of allocator_aware::BSONArrayBuilder. Do
// not remove or re-order these lines w.r.t BSONArrayBuilderBase or
// allocator_aware::BSONArrayBuilder without being sure that you are not undoing the advantages of
// the extern template declaration.
template <class>
class BSONArrayBuilder;
}  // namespace allocator_aware

extern template class BSONArrayBuilderBase<allocator_aware::BSONArrayBuilder<std::allocator<void>>,
                                           allocator_aware::BSONObjBuilder<std::allocator<void>>>;
extern template class BSONArrayBuilderBase<
    allocator_aware::BSONArrayBuilder<tracking::Allocator<void>>,
    allocator_aware::BSONObjBuilder<tracking::Allocator<void>>>;

namespace allocator_aware {
template <class Allocator>
class BSONArrayBuilder
    : public BSONArrayBuilderBase<BSONArrayBuilder<Allocator>, BSONObjBuilder<Allocator>> {
public:
    BSONArrayBuilder(BufBuilder<Allocator>& bufBuilder)
        : BSONArrayBuilderBase<BSONArrayBuilder, BSONObjBuilder<Allocator>>(bufBuilder) {}
};
}  // namespace allocator_aware

// The following extern template declaration must come after the
// forward delaration of UniqueBSONArrayBuilder above, and before the
// use of the matching instantiation of the base class of
// UniqueBSONArrayBuilder. Do not remove or re-order these lines w.r.t
// BSONArrayBuilderBase or UniqueBSONArrayBuilder without being sure
// that you are not undoing the advantages of the extern template
// declaration.
extern template class BSONArrayBuilderBase<UniqueBSONArrayBuilder, UniqueBSONObjBuilder>;

/**
 * Alternative to BSONArrayBuilder. This class is analogous to UniqueBSONObjBuilder.
 */
class UniqueBSONArrayBuilder
    : public BSONArrayBuilderBase<UniqueBSONArrayBuilder, UniqueBSONObjBuilder> {
public:
    using ObjBuilder = UniqueBSONObjBuilder;

    using BSONArrayBuilderBase<UniqueBSONArrayBuilder, UniqueBSONObjBuilder>::BSONArrayBuilderBase;
    UniqueBSONArrayBuilder(UniqueBufBuilder& bufBuilder)
        : BSONArrayBuilderBase<UniqueBSONArrayBuilder, UniqueBSONObjBuilder>(bufBuilder) {}

    /**
     * destructive - ownership moves to returned BSONArray
     * @return owned BSONArray
     */
    BSONArray arr() {
        return BSONArray(_b.obj());
    }
    BSONObj obj() {
        return _b.obj();
    }
};

template <class Derived, class B>
template <class T>
inline Derived& BSONObjBuilderBase<Derived, B>::append(StringData fieldName,
                                                       const std::vector<T>& vals) {
    return append(fieldName, vals.begin(), vals.end());
}

template <class Derived, class B>
template <class T>
inline Derived& BSONObjBuilderBase<Derived, B>::append(StringData fieldName,
                                                       const std::list<T>& vals) {
    return append(fieldName, vals.begin(), vals.end());
}

template <class Derived, class B>
template <class T>
inline Derived& BSONObjBuilderBase<Derived, B>::append(StringData fieldName,
                                                       const std::set<T>& vals) {
    return append(fieldName, vals.begin(), vals.end());
}

template <class Derived, class B>
template <class It>
inline Derived& BSONObjBuilderBase<Derived, B>::append(StringData fieldName, It begin, It end) {
    Derived arrBuilder(subarrayStart(fieldName));
    DecimalCounter<size_t> n;
    for (; begin != end; ++begin) {
        arrBuilder.append(StringData{n}, *begin);
        ++n;
    }
    return asDerived();
}

template <class Derived, class BSONObjBuilderType>
template <class T>
inline Derived& BSONArrayBuilderBase<Derived, BSONObjBuilderType>::append(
    const std::list<T>& vals) {
    return append(vals.begin(), vals.end());
}

template <class Derived, class BSONObjBuilderType>
template <class T>
inline Derived& BSONArrayBuilderBase<Derived, BSONObjBuilderType>::append(const std::set<T>& vals) {
    return append(vals.begin(), vals.end());
}

template <class Derived, class BSONObjBuilderType>
template <class It>
inline Derived& BSONArrayBuilderBase<Derived, BSONObjBuilderType>::append(It begin, It end) {
    auto& derivedThis = static_cast<Derived&>(*this);
    for (; begin != end; ++begin) {
        derivedThis.append(*begin);
    }
    return derivedThis;
}

template <typename T>
inline BSONFieldValue<BSONObj> BSONField<T>::query(const char* q, const T& t) const {
    BSONObjBuilder b;
    b.append(q, t);
    return BSONFieldValue<BSONObj>(_name, b.obj());
}

/* Utility class to add a Date element with the current time
   Example:
     std::cout << BSON( "created" << DATENOW ); // { created : "2009-10-09 11:41:42" }
*/
struct DateNowLabeler {
    friend void appendToBson(BSONObjBuilder& bob, StringData fieldName, DateNowLabeler) {
        bob.appendDate(fieldName, Date_t::now());
    }
};
constexpr inline DateNowLabeler DATENOW;

/* Utility class to assign a NULL value to a given attribute
   Example:
     std::cout << BSON( "a" << BSONNULL ); // { a : null }
*/
struct NullLabeler {
    friend void appendToBson(BSONObjBuilder& bob, StringData fieldName, NullLabeler) {
        bob.appendNull(fieldName);
    }
};
constexpr inline NullLabeler BSONNULL;

/* Utility class to assign an Undefined value to a given attribute
   Example:
     std::cout << BSON( "a" << BSONUndefined ); // { a : undefined }
*/
struct UndefinedLabeler {
    friend void appendToBson(BSONObjBuilder& bob, StringData fieldName, UndefinedLabeler) {
        bob.appendUndefined(fieldName);
    }
};
constexpr inline UndefinedLabeler BSONUndefined;

/**
 * Utility class to add the minKey (minus infinity) to a given attribute
 * Example:
 *   std::cout << BSON( "a" << MINKEY ); // { "a" : { "$minKey" : 1 } }
 */
struct MinKeyLabeler {
    friend void appendToBson(BSONObjBuilder& bob, StringData fieldName, MinKeyLabeler) {
        bob.appendMinKey(fieldName);
    }
};
constexpr inline MinKeyLabeler MINKEY;

/**
 * Utility class to add the maxKey (infinity) to a given attribute
 * Example:
 *   std::cout << BSON( "a" << MAXKEY ); // { "a" : { "$maxKey" : 1 } }
 */
struct MaxKeyLabeler {
    friend void appendToBson(BSONObjBuilder& bob, StringData fieldName, MaxKeyLabeler) {
        bob.appendMaxKey(fieldName);
    }
};
constexpr inline MaxKeyLabeler MAXKEY;

/**
 * These labels like GT, GTE, LT, LTE, NE can be helpful for stream-oriented
 * construction of a BSONObj, particularly when assembling a query.
 * See (https://www.mongodb.com/docs/manual/reference/operator/query/).
 *
 * Each accepts a following value operand with a subsequent `<<`.
 * Any number of such pairs may follow a fieldname, which
 * will have the effect of creating that field as holding a subobject
 * mapping all such labels to their corresponding operands.
 *
 * E.g.:
 *     BSON("a" << GT<<123) => {"a":{"$gt":123}}
 *     BSON("a" << GT<<123 << NE<<30) => {"a":{"$gt":123,"$ne":30}}
 *     BSON("a" << GT<<123 << "b" << 2) => {"a":{"$gt":123},"b":2}
 */
class QueryConstraintLabel {
public:
    // This class and its public methods are marked [[nodiscard]] because it is required to do
    // *something* with the results. We open a subobject and you must either continue appending
    // until you append a "normal" field, or you must convert it to BSONObjBuilder&, which happens
    // automatically when the BSON() macro is closed.
    class [[nodiscard]] ArgumentAcceptor {
        class AfterArgument;

    public:
        ArgumentAcceptor(StringData label, BSONObjBuilder::ValueStream* stream)
            : _label{label}, _stream{stream}, _subObjBuilder{_stream->subobjStart()} {}

        AfterArgument& operator<<(const bsonobjbuilder_details::HasAdlAppendToBson auto& value) {
            appendToBson(_subObjBuilder, _label, value);
            return _after;
        }

        /**
         * The value of `e` is appended, but with this
         * label replacing its fieldName.
         *     "age" << GT << BSONElement{"dummy", 123}
         *     {"age":{"$gt":123}}
         */
        AfterArgument& operator<<(const BSONElement& e) {
            _subObjBuilder.appendAs(e, _label);
            return _after;
        }

    private:
        class AfterArgument {
        public:
            explicit AfterArgument(ArgumentAcceptor* stream) : _stream{stream} {}

            explicit operator BSONObjBuilder&() {
                _stream->_subObjBuilder.done();
                return _stream->_stream->builder();
            }

            decltype(auto) operator<<(const auto& x) {
                return static_cast<BSONObjBuilder&>(*this) << x;
            }

            [[nodiscard]] ArgumentAcceptor& operator<<(const QueryConstraintLabel& qcl) {
                _stream->_label = qcl.label();
                return *_stream;
            }

        private:
            ArgumentAcceptor* _stream;
        };

        StringData _label;
        BSONObjBuilder::ValueStream* _stream;
        AfterArgument _after{this};
        BSONObjBuilder _subObjBuilder;
    };

    constexpr explicit QueryConstraintLabel(StringData l) : _label(l) {}

    StringData label() const {
        return _label;
    }

    friend ArgumentAcceptor operator<<(BSONObjBuilder::ValueStream& valueStream,
                                       const QueryConstraintLabel& qcl) {
        return {qcl.label(), &valueStream};
    }

    friend ArgumentAcceptor operator<<(BSONObjBuilder::ValueStream&& valueStream,
                                       const QueryConstraintLabel& qcl) {
        return valueStream << qcl;
    }

private:
    StringData _label;
};

constexpr inline QueryConstraintLabel GT{"$gt"};
constexpr inline QueryConstraintLabel GTE{"$gte"};
constexpr inline QueryConstraintLabel LT{"$lt"};
constexpr inline QueryConstraintLabel LTE{"$lte"};
constexpr inline QueryConstraintLabel NE{"$ne"};
constexpr inline QueryConstraintLabel NIN{"$nin"};
constexpr inline QueryConstraintLabel BSIZE{"$size"};

constexpr inline int kBSONObjBuilderInitialCapacity = 64;

/** Use BSON macro to build a BSONObj from a stream

    e.g.,
       BSON( "name" << "joe" << "age" << 33 )

    with auto-generated object id:
       BSON( "_id" << OID::gen() << "name" << "joe" << "age" << 33 )

    The labels GT, GTE, LT, LTE, NE can be helpful for stream-oriented construction
    of a BSONObj, particularly when assembling a Query.  For example,
    BSON( "a" << GT << 23.4 << NE << 30 << "b" << 2 ) produces the object
    { a: { \$gt: 23.4, \$ne: 30 }, b: 2 }.
*/
#define BSON(x)                                                                 \
    (static_cast<::mongo::BSONObjBuilder&>(                                     \
         ::mongo::BSONObjBuilder{::mongo::kBSONObjBuilderInitialCapacity} << x) \
         .obj())

/** Use BSON_ARRAY macro like BSON macro, but without keys

    BSONArray arr = BSON_ARRAY( "hello" << 1 <<
                        BSON( "foo" << BSON_ARRAY( "bar" << "baz" << "qux" ) ) );

 */
#define BSON_ARRAY(x)                                                                             \
    ((::mongo::BSONArrayBuilder(::mongo::BSONObjBuilder{::mongo::kBSONObjBuilderInitialCapacity}) \
      << x)                                                                                       \
         .arr())

}  // namespace mongo
