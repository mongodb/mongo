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

/**
 * Classes in this file:
 * BSONObjBuilder
 * BSONArrayBuilder
 */

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <type_traits>

#include "mongo/base/data_view.h"
#include "mongo/base/parse_number.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

#if defined(_WIN32)
// warning: 'this' : used in base member initializer list
#pragma warning(disable : 4355)
#endif

/** Utility for creating a BSONObj.
    See also the BSON() and BSON_ARRAY() macros.
*/
class BSONObjBuilder {
    BSONObjBuilder(const BSONObjBuilder&) = delete;
    BSONObjBuilder& operator=(const BSONObjBuilder&) = delete;

public:
    /** @param initsize this is just a hint as to the final size of the object */
    BSONObjBuilder(int initsize = 512) : _b(_buf), _buf(initsize), _s(this) {
        // Skip over space for the object length. The length is filled in by _done.
        _b.skip(sizeof(int));

        // Reserve space for the EOO byte. This means _done() can't fail.
        _b.reserveBytes(1);
    }

    /** @param baseBuilder construct a BSONObjBuilder using an existing BufBuilder
     *  This is for more efficient adding of subobjects/arrays. See docs for subobjStart for
     *  example.
     */
    BSONObjBuilder(BufBuilder& baseBuilder)
        : _b(baseBuilder), _buf(0), _offset(baseBuilder.len()), _s(this) {
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

    BSONObjBuilder(ResumeBuildingTag, BufBuilder& existingBuilder, std::size_t offset = 0)
        : _b(existingBuilder), _buf(0), _offset(offset), _s(this) {
        invariant(_b.len() - offset >= BSONObj::kMinBSONLength);
        _b.setlen(_b.len() - 1);  // get rid of the previous EOO.
        // Reserve space for our EOO.
        _b.reserveBytes(1);
    }

    BSONObjBuilder(const BSONSizeTracker& tracker)
        : _b(_buf),
          _buf(tracker.getSize()),
          _s(this),
          _tracker(const_cast<BSONSizeTracker*>(&tracker)) {
        // See the comments in the first constructor for details.
        _b.skip(sizeof(int));

        // Reserve space for the EOO byte. This means _done() can't fail.
        _b.reserveBytes(1);
    }

    /**
     * Creates a new BSONObjBuilder prefixed with the fields in 'prefix'.
     *
     * If prefix is an rvalue referring to the only view of the underlying BSON buffer, it will be
     * able to avoid copying and will just reuse the buffer. Therefore, you should try to std::move
     * into this constructor where possible.
     */
    BSONObjBuilder(BSONObj prefix) : _b(_buf), _buf(0), _s(this) {
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

    // Move constructible, but not assignable due to reference member.
    BSONObjBuilder(BSONObjBuilder&& other)
        : _b(&other._b == &other._buf ? _buf : other._b),
          _buf(std::move(other._buf)),
          _offset(std::move(other._offset)),
          _s(this),  // Don't move from other._s because that will leave it pointing to other.
          _tracker(std::move(other._tracker)),
          _doneCalled(std::move(other._doneCalled)) {
        other.abandon();
    }

    ~BSONObjBuilder();

    /**
     * The start offset of the object being built by this builder within its buffer.
     * Needed for the object-resuming constructor.
     */
    std::size_t offset() const {
        return _offset;
    }

    /** add all the fields from the object specified to this object */
    BSONObjBuilder& appendElements(const BSONObj& x);

    /** add all the fields from the object specified to this object if they don't exist already */
    BSONObjBuilder& appendElementsUnique(const BSONObj& x);

    /** append element to the object we are building */
    BSONObjBuilder& append(const BSONElement& e) {
        // do not append eoo, that would corrupt us. the builder auto appends when done() is called.
        verify(!e.eoo());
        _b.appendBuf((void*)e.rawdata(), e.size());
        return *this;
    }

    /** append an element but with a new name */
    BSONObjBuilder& appendAs(const BSONElement& e, StringData fieldName) {
        // do not append eoo, that would corrupt us. the builder auto appends when done() is called.
        verify(!e.eoo());
        _b.appendNum((char)e.type());
        _b.appendStr(fieldName);
        _b.appendBuf((void*)e.value(), e.valuesize());
        return *this;
    }

    /** add a subobject as a member */
    BSONObjBuilder& append(StringData fieldName, BSONObj subObj) {
        _b.appendNum((char)Object);
        _b.appendStr(fieldName);
        _b.appendBuf((void*)subObj.objdata(), subObj.objsize());
        return *this;
    }

    /** add a subobject as a member */
    BSONObjBuilder& appendObject(StringData fieldName, const char* objdata, int size = 0) {
        verify(objdata);
        if (size == 0) {
            size = ConstDataView(objdata).read<LittleEndian<int>>();
        }

        verify(size > 4 && size < 100000000);

        _b.appendNum((char)Object);
        _b.appendStr(fieldName);
        _b.appendBuf((void*)objdata, size);
        return *this;
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
    BufBuilder& subobjStart(StringData fieldName) {
        _b.appendNum((char)Object);
        _b.appendStr(fieldName);
        return _b;
    }

    /** add a subobject as a member with type Array.  Thus arr object should have "0", "1", ...
        style fields in it.
    */
    BSONObjBuilder& appendArray(StringData fieldName, const BSONObj& subObj) {
        _b.appendNum((char)Array);
        _b.appendStr(fieldName);
        _b.appendBuf((void*)subObj.objdata(), subObj.objsize());
        return *this;
    }
    BSONObjBuilder& append(StringData fieldName, BSONArray arr) {
        return appendArray(fieldName, arr);
    }

    /** add header for a new subarray and return bufbuilder for writing to
        the subarray's body */
    BufBuilder& subarrayStart(StringData fieldName) {
        _b.appendNum((char)Array);
        _b.appendStr(fieldName);
        return _b;
    }

    /** Append a boolean element */
    BSONObjBuilder& appendBool(StringData fieldName, int val) {
        _b.appendNum((char)Bool);
        _b.appendStr(fieldName);
        _b.appendNum((char)(val ? 1 : 0));
        return *this;
    }

    /** Append elements that have the BSONObjAppendFormat trait */
    template <typename T, typename = std::enable_if_t<IsBSONObjAppendable<T>::value>>
    BSONObjBuilder& append(StringData fieldName, const T& n) {
        constexpr BSONType type = BSONObjAppendFormat<T>::value;
        _b.appendNum(static_cast<char>(type));
        _b.appendStr(fieldName);
        if constexpr (type == Bool) {
            _b.appendNum(static_cast<char>(n));
        } else if constexpr (type == NumberInt) {
            _b.appendNum(static_cast<int>(n));
        } else {
            _b.appendNum(n);
        }
        return *this;
    }

    template <typename T,
              typename = std::enable_if_t<!IsBSONObjAppendable<T>::value && std::is_integral_v<T>>,
              typename = void>
    BSONObjBuilder& append(StringData fieldName, const T& n) = delete;

    /** appends a number.  if n < max(int)/2 then uses int, otherwise long long */
    BSONObjBuilder& appendIntOrLL(StringData fieldName, long long n) {
        // extra () to avoid max macro on windows
        static const long long maxInt = (std::numeric_limits<int>::max)() / 2;
        static const long long minInt = -maxInt;
        if (minInt < n && n < maxInt) {
            append(fieldName, static_cast<int>(n));
        } else {
            append(fieldName, n);
        }
        return *this;
    }

    /**
     * appendNumber is a series of method for appending the smallest sensible type
     * mostly for JS
     */
    BSONObjBuilder& appendNumber(StringData fieldName, int n) {
        return append(fieldName, n);
    }

    BSONObjBuilder& appendNumber(StringData fieldName, double d) {
        return append(fieldName, d);
    }

    BSONObjBuilder& appendNumber(StringData fieldName, size_t n) {
        static const size_t maxInt = (1 << 30);
        if (n < maxInt)
            append(fieldName, static_cast<int>(n));
        else
            append(fieldName, static_cast<long long>(n));
        return *this;
    }

    BSONObjBuilder& appendNumber(StringData fieldName, Decimal128 decNumber) {
        return append(fieldName, decNumber);
    }

    BSONObjBuilder& appendNumber(StringData fieldName, long long llNumber) {
        static const long long maxInt = (1LL << 30);
        static const long long minInt = -maxInt;
        static const long long maxDouble = (1LL << 40);
        static const long long minDouble = -maxDouble;

        if (minInt < llNumber && llNumber < maxInt) {
            append(fieldName, static_cast<int>(llNumber));
        } else if (minDouble < llNumber && llNumber < maxDouble) {
            append(fieldName, static_cast<double>(llNumber));
        } else {
            append(fieldName, llNumber);
        }

        return *this;
    }

    /** Append a BSON Object ID (OID type).
        @deprecated Generally, it is preferred to use the append append(name, oid)
        method for this.
    */
    BSONObjBuilder& appendOID(StringData fieldName,
                              OID* oid = nullptr,
                              bool generateIfBlank = false) {
        _b.appendNum((char)jstOID);
        _b.appendStr(fieldName);
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
        return *this;
    }

    /**
    Append a BSON Object ID.
    @param fieldName Field name, e.g., "_id".
    @returns the builder object
    */
    BSONObjBuilder& append(StringData fieldName, OID oid) {
        _b.appendNum((char)jstOID);
        _b.appendStr(fieldName);
        _b.appendBuf(oid.view().view(), OID::kOIDSize);
        return *this;
    }

    /**
    Generate and assign an object id for the _id field.
    _id should be the first element in the object for good performance.
    */
    BSONObjBuilder& genOID() {
        return append("_id", OID::gen());
    }

    /** Append a time_t date.
        @param dt a C-style 32 bit date value, that is
        the number of seconds since January 1, 1970, 00:00:00 GMT
    */
    BSONObjBuilder& appendTimeT(StringData fieldName, time_t dt) {
        _b.appendNum((char)Date);
        _b.appendStr(fieldName);
        _b.appendNum(static_cast<unsigned long long>(dt) * 1000);
        return *this;
    }
    /** Append a date.
        @param dt a Java-style 64 bit date value, that is
        the number of milliseconds since January 1, 1970, 00:00:00 GMT
    */
    BSONObjBuilder& appendDate(StringData fieldName, Date_t dt);
    BSONObjBuilder& append(StringData fieldName, Date_t dt) {
        return appendDate(fieldName, dt);
    }

    /** Append a regular expression value
        @param regex the regular expression pattern
        @param regex options such as "i" or "g"
    */
    BSONObjBuilder& appendRegex(StringData fieldName, StringData regex, StringData options = "") {
        _b.appendNum((char)RegEx);
        _b.appendStr(fieldName);
        _b.appendStr(regex);
        _b.appendStr(options);
        return *this;
    }

    BSONObjBuilder& append(StringData fieldName, const BSONRegEx& regex) {
        return appendRegex(fieldName, regex.pattern, regex.flags);
    }

    BSONObjBuilder& appendCode(StringData fieldName, StringData code) {
        _b.appendNum((char)Code);
        _b.appendStr(fieldName);
        _b.appendNum((int)code.size() + 1);
        _b.appendStr(code);
        return *this;
    }

    BSONObjBuilder& append(StringData fieldName, const BSONCode& code) {
        return appendCode(fieldName, code.code);
    }

    /** Append a string element.
        @param sz size includes terminating null character */
    BSONObjBuilder& append(StringData fieldName, const char* str, int sz) {
        _b.appendNum((char)String);
        _b.appendStr(fieldName);
        _b.appendNum((int)sz);
        _b.appendBuf(str, sz);
        return *this;
    }
    /** Append a string element */
    BSONObjBuilder& append(StringData fieldName, const char* str) {
        return append(fieldName, str, (int)strlen(str) + 1);
    }
    /** Append a string element */
    BSONObjBuilder& append(StringData fieldName, StringData str) {
        _b.appendNum((char)String);
        _b.appendStr(fieldName);
        _b.appendNum((int)str.size() + 1);
        _b.appendStr(str, true);
        return *this;
    }

    BSONObjBuilder& appendSymbol(StringData fieldName, StringData symbol) {
        _b.appendNum((char)Symbol);
        _b.appendStr(fieldName);
        _b.appendNum((int)symbol.size() + 1);
        _b.appendStr(symbol);
        return *this;
    }

    BSONObjBuilder& append(StringData fieldName, const BSONSymbol& symbol) {
        return appendSymbol(fieldName, symbol.symbol);
    }

    /** Append a Null element to the object */
    BSONObjBuilder& appendNull(StringData fieldName) {
        _b.appendNum((char)jstNULL);
        _b.appendStr(fieldName);
        return *this;
    }

    // Append an element that is less than all other keys.
    BSONObjBuilder& appendMinKey(StringData fieldName) {
        _b.appendNum((char)MinKey);
        _b.appendStr(fieldName);
        return *this;
    }
    // Append an element that is greater than all other keys.
    BSONObjBuilder& appendMaxKey(StringData fieldName) {
        _b.appendNum((char)MaxKey);
        _b.appendStr(fieldName);
        return *this;
    }

    // Append a Timestamp field -- will be updated to next server Timestamp
    BSONObjBuilder& appendTimestamp(StringData fieldName);

    BSONObjBuilder& appendTimestamp(StringData fieldName, unsigned long long val);

    /**
     * To store a Timestamp in BSON, use this function.
     * This captures both the secs and inc fields.
     */
    BSONObjBuilder& append(StringData fieldName, Timestamp timestamp);

    /*
    Append an element of the deprecated DBRef type.
    @deprecated
    */
    BSONObjBuilder& appendDBRef(StringData fieldName, StringData ns, const OID& oid) {
        _b.appendNum((char)DBRef);
        _b.appendStr(fieldName);
        _b.appendNum((int)ns.size() + 1);
        _b.appendStr(ns);
        _b.appendBuf(oid.view().view(), OID::kOIDSize);
        return *this;
    }

    BSONObjBuilder& append(StringData fieldName, const BSONDBRef& dbref) {
        return appendDBRef(fieldName, dbref.ns, dbref.oid);
    }

    /** Append a binary data element
        @param fieldName name of the field
        @param len length of the binary data in bytes
        @param subtype subtype information for the data. @see enum BinDataType in bsontypes.h.
               Use BinDataGeneral if you don't care about the type.
        @param data the byte array
    */
    BSONObjBuilder& appendBinData(StringData fieldName,
                                  int len,
                                  BinDataType type,
                                  const void* data) {
        _b.appendNum((char)BinData);
        _b.appendStr(fieldName);
        _b.appendNum(len);
        _b.appendNum((char)type);
        _b.appendBuf(data, len);
        return *this;
    }

    BSONObjBuilder& append(StringData fieldName, const BSONBinData& bd) {
        return appendBinData(fieldName, bd.length, bd.type, bd.data);
    }

    /**
    Subtype 2 is deprecated.
    Append a BSON bindata bytearray element.
    @param data a byte array
    @param len the length of data
    */
    BSONObjBuilder& appendBinDataArrayDeprecated(const char* fieldName, const void* data, int len) {
        _b.appendNum((char)BinData);
        _b.appendStr(fieldName);
        _b.appendNum(len + 4);
        _b.appendNum((char)0x2);
        _b.appendNum(len);
        _b.appendBuf(data, len);
        return *this;
    }

    /** Append to the BSON object a field of type CodeWScope.  This is a javascript code
        fragment accompanied by some scope that goes with it.
    */
    BSONObjBuilder& appendCodeWScope(StringData fieldName, StringData code, const BSONObj& scope) {
        _b.appendNum((char)CodeWScope);
        _b.appendStr(fieldName);
        _b.appendNum((int)(4 + 4 + code.size() + 1 + scope.objsize()));
        _b.appendNum((int)code.size() + 1);
        _b.appendStr(code);
        _b.appendBuf((void*)scope.objdata(), scope.objsize());
        return *this;
    }

    BSONObjBuilder& append(StringData fieldName, const BSONCodeWScope& cws) {
        return appendCodeWScope(fieldName, cws.code, cws.scope);
    }

    BSONObjBuilder& appendUndefined(StringData fieldName) {
        _b.appendNum((char)Undefined);
        _b.appendStr(fieldName);
        return *this;
    }

    /* helper function -- see Query::where() for primary way to do this. */
    BSONObjBuilder& appendWhere(StringData code, const BSONObj& scope) {
        return appendCodeWScope("$where", code, scope);
    }

    /**
       these are the min/max when comparing, not strict min/max elements for a given type
    */
    BSONObjBuilder& appendMinForType(StringData fieldName, int type);
    BSONObjBuilder& appendMaxForType(StringData fieldName, int type);

    /** Append an array of values. */
    template <class T>
    BSONObjBuilder& append(StringData fieldName, const std::vector<T>& vals);

    template <class T>
    BSONObjBuilder& append(StringData fieldName, const std::list<T>& vals);

    /** Append a set of values. */
    template <class T>
    BSONObjBuilder& append(StringData fieldName, const std::set<T>& vals);

    /**
     * Append a map of values as a sub-object.
     * Note: the keys of the map should be StringData-compatible (i.e. strings).
     */
    template <class K, class T>
    BSONObjBuilder& append(StringData fieldName, const std::map<K, T>& vals);

    /**
     * Resets this BSONObjBulder to an empty state. All previously added fields are lost.  If this
     * BSONObjBuilder is using an externally provided BufBuilder, this method does not affect the
     * bytes before the start of this object.
     *
     * Invalid to call if done() has already been called in order to finalize the BSONObj.
     */
    void resetToEmpty() {
        invariant(!_doneCalled);
        _s.reset();
        // Reset the position the next write will go to right after our size reservation.
        _b.setlen(_offset + sizeof(int));
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

    /** Fetch the object we have built.
        BSONObjBuilder still frees the object when the builder goes out of
        scope -- very important to keep in mind.  Use obj() if you
        would like the BSONObj to last longer than the builder.
    */
    template <typename BSONTraits = BSONObj::DefaultSizeTrait>
    BSONObj done() {
        return BSONObj(_done(), BSONTraits{});
    }

    // Like 'done' above, but does not construct a BSONObj to return to the caller.
    void doneFast() {
        (void)_done();
    }

    /** Peek at what is in the builder, but leave the builder ready for more appends.
        The returned object is only valid until the next modification or destruction of the builder.
        Intended use case: append a field if not already there.
    */
    BSONObj asTempObj() {
        const char* const buffer = _done();

        // None of the code which resets this builder to the not-done state is expected to throw.
        // If it does, that would be a violation of our expectations.
        auto resetObjectState = makeGuard([this]() noexcept {
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

    /** Stream oriented way to add field names and values. */
    BSONObjBuilderValueStream& operator<<(StringData name) {
        _s.endField(name);
        return _s;
    }

    /** Stream oriented way to add field names and values. */
    BSONObjBuilder& operator<<(GENOIDLabeler) {
        return genOID();
    }

    Labeler operator<<(const Labeler::Label& l) {
        massert(10336, "No subobject started", _s.subobjStarted());
        return _s << l;
    }

    template <typename T>
    BSONObjBuilderValueStream& operator<<(const BSONField<T>& f) {
        _s.endField(f.name());
        return _s;
    }

    template <typename T>
    BSONObjBuilder& operator<<(const BSONFieldValue<T>& v) {
        append(v.name(), v.value());
        return *this;
    }

    BSONObjBuilder& operator<<(const BSONElement& e) {
        append(e);
        return *this;
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

    BufBuilder& bb() {
        return _b;
    }

private:
    char* _done() {
        if (_doneCalled)
            return _b.buf() + _offset;

        // TODO remove this or find some way to prevent it from failing. Since this is intended
        // for use with BSON() literal queries, it is less likely to result in oversized BSON.
        _s.endField();

        _b.claimReservedBytes(1);  // Prevents adding EOO from failing.
        _b.appendNum((char)EOO);

        char* data = _b.buf() + _offset;
        int size = _b.len() - _offset;
        DataView(data).write(tagLittleEndian(size));
        if (_tracker)
            _tracker->got(size);

        // Only set `_doneCalled` to true when all functions which could throw haven't thrown.
        _doneCalled = true;
        return data;
    }

    BufBuilder& _b;
    BufBuilder _buf;
    int _offset = 0;
    BSONObjBuilderValueStream _s;
    BSONSizeTracker* _tracker = nullptr;
    bool _doneCalled = false;
};

class BSONArrayBuilder {
public:
    BSONArrayBuilder() {}
    BSONArrayBuilder(BufBuilder& _b) : _b(_b) {}
    BSONArrayBuilder(int initialSize) : _b(initialSize) {}

    template <typename T>
    BSONArrayBuilder& append(const T& x) {
        _b.append(_fieldCount, x);
        ++_fieldCount;
        return *this;
    }

    BSONArrayBuilder& append(const BSONElement& e) {
        _b.appendAs(e, _fieldCount);
        ++_fieldCount;
        return *this;
    }

    BSONArrayBuilder& operator<<(const BSONElement& e) {
        return append(e);
    }

    template <typename T>
    BSONArrayBuilder& operator<<(const T& x) {
        _b << _fieldCount << x;
        ++_fieldCount;
        return *this;
    }

    BSONArrayBuilder& appendNull() {
        _b.appendNull(_fieldCount);
        ++_fieldCount;
        return *this;
    }

    BSONArrayBuilder& appendUndefined() {
        _b.appendUndefined(_fieldCount);
        ++_fieldCount;
        return *this;
    }

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

    BSONObj done() {
        return _b.done();
    }

    void doneFast() {
        _b.doneFast();
    }

    template <class T>
    BSONArrayBuilder& append(const std::list<T>& vals);

    template <class T>
    BSONArrayBuilder& append(const std::set<T>& vals);

    // These two just use next position
    BufBuilder& subobjStart() {
        return _b.subobjStart(_fieldCount++);
    }
    BufBuilder& subarrayStart() {
        return _b.subarrayStart(_fieldCount++);
    }

    BSONArrayBuilder& appendRegex(StringData regex, StringData options = "") {
        _b.appendRegex(_fieldCount, regex, options);
        ++_fieldCount;
        return *this;
    }

    BSONArrayBuilder& appendBinData(int len, BinDataType type, const void* data) {
        _b.appendBinData(_fieldCount, len, type, data);
        ++_fieldCount;
        return *this;
    }

    BSONArrayBuilder& appendCode(StringData code) {
        _b.appendCode(_fieldCount, code);
        ++_fieldCount;
        return *this;
    }

    BSONArrayBuilder& appendCodeWScope(StringData code, const BSONObj& scope) {
        _b.appendCodeWScope(_fieldCount, code, scope);
        ++_fieldCount;
        return *this;
    }

    BSONArrayBuilder& appendTimeT(time_t dt) {
        _b.appendTimeT(_fieldCount, dt);
        ++_fieldCount;
        return *this;
    }

    BSONArrayBuilder& appendDate(Date_t dt) {
        _b.appendDate(_fieldCount, dt);
        ++_fieldCount;
        return *this;
    }

    BSONArrayBuilder& appendBool(bool val) {
        _b.appendBool(_fieldCount, val);
        ++_fieldCount;
        return *this;
    }

    BSONArrayBuilder& appendTimestamp(unsigned long long ts) {
        _b.appendTimestamp(_fieldCount, ts);
        ++_fieldCount;
        return *this;
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

    BufBuilder& bb() {
        return _b.bb();
    }

private:
    DecimalCounter<uint32_t> _fieldCount;
    BSONObjBuilder _b;
};

template <class T>
inline BSONObjBuilder& BSONObjBuilder::append(StringData fieldName, const std::vector<T>& vals) {
    BSONObjBuilder arrBuilder(subarrayStart(fieldName));
    DecimalCounter<size_t> n;
    for (unsigned int i = 0; i < vals.size(); ++i) {
        arrBuilder.append(StringData{n}, vals[i]);
        ++n;
    }
    return *this;
}

template <class L>
inline BSONObjBuilder& _appendIt(BSONObjBuilder& _this, StringData fieldName, const L& vals) {
    BSONObjBuilder arrBuilder;
    DecimalCounter<size_t> n;
    for (typename L::const_iterator i = vals.begin(); i != vals.end(); i++) {
        arrBuilder.append(StringData{n}, *i);
        ++n;
    }
    _this.appendArray(fieldName, arrBuilder.done());
    return _this;
}

template <class T>
inline BSONObjBuilder& BSONObjBuilder::append(StringData fieldName, const std::list<T>& vals) {
    return _appendIt<std::list<T>>(*this, fieldName, vals);
}

template <class T>
inline BSONObjBuilder& BSONObjBuilder::append(StringData fieldName, const std::set<T>& vals) {
    return _appendIt<std::set<T>>(*this, fieldName, vals);
}

template <class K, class T>
inline BSONObjBuilder& BSONObjBuilder::append(StringData fieldName, const std::map<K, T>& vals) {
    BSONObjBuilder bob;
    for (typename std::map<K, T>::const_iterator i = vals.begin(); i != vals.end(); ++i) {
        bob.append(i->first, i->second);
    }
    append(fieldName, bob.obj());
    return *this;
}

template <class L>
inline BSONArrayBuilder& _appendArrayIt(BSONArrayBuilder& _this, const L& vals) {
    for (typename L::const_iterator i = vals.begin(); i != vals.end(); i++)
        _this.append(*i);
    return _this;
}

template <class T>
inline BSONArrayBuilder& BSONArrayBuilder::append(const std::list<T>& vals) {
    return _appendArrayIt<std::list<T>>(*this, vals);
}

template <class T>
inline BSONArrayBuilder& BSONArrayBuilder::append(const std::set<T>& vals) {
    return _appendArrayIt<std::set<T>>(*this, vals);
}

template <typename T>
inline BSONFieldValue<BSONObj> BSONField<T>::query(const char* q, const T& t) const {
    BSONObjBuilder b;
    b.append(q, t);
    return BSONFieldValue<BSONObj>(_name, b.obj());
}

inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(const DateNowLabeler& id) {
    _builder->appendDate(_fieldName, jsTime());
    _fieldName = StringData();
    return *_builder;
}

inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(const NullLabeler& id) {
    _builder->appendNull(_fieldName);
    _fieldName = StringData();
    return *_builder;
}

inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(const UndefinedLabeler& id) {
    _builder->appendUndefined(_fieldName);
    _fieldName = StringData();
    return *_builder;
}

inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(const MinKeyLabeler& id) {
    _builder->appendMinKey(_fieldName);
    _fieldName = StringData();
    return *_builder;
}

inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(const MaxKeyLabeler& id) {
    _builder->appendMaxKey(_fieldName);
    _fieldName = StringData();
    return *_builder;
}

template <class T>
inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(T value) {
    _builder->append(_fieldName, value);
    _fieldName = StringData();
    return *_builder;
}

template <class T>
BSONObjBuilder& Labeler::operator<<(T value) {
    s_->subobj()->append(l_.l_, value);
    return *s_->_builder;
}

inline BSONObjBuilder& BSONObjBuilder::append(StringData fieldName, Timestamp optime) {
    optime.append(_b, fieldName);
    return *this;
}

inline BSONObjBuilder& BSONObjBuilder::appendTimestamp(StringData fieldName) {
    return append(fieldName, Timestamp());
}

inline BSONObjBuilder& BSONObjBuilder::appendTimestamp(StringData fieldName,
                                                       unsigned long long val) {
    return append(fieldName, Timestamp(val));
}

}  // namespace mongo
