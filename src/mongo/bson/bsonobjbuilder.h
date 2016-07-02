/* bsonobjbuilder.h

   Classes in this file:
   BSONObjBuilder
   BSONArrayBuilder
*/

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

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>

#include "mongo/base/data_view.h"
#include "mongo/base/parse_number.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/itoa.h"

namespace mongo {

#if defined(_WIN32)
// warning: 'this' : used in base member initializer list
#pragma warning(disable : 4355)
#endif

/** Utility for creating a BSONObj.
    See also the BSON() and BSON_ARRAY() macros.
*/
class BSONObjBuilder {
    MONGO_DISALLOW_COPYING(BSONObjBuilder);

public:
    /** @param initsize this is just a hint as to the final size of the object */
    BSONObjBuilder(int initsize = 512)
        : _b(_buf), _buf(initsize), _offset(0), _s(this), _tracker(0), _doneCalled(false) {
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
        : _b(baseBuilder),
          _buf(0),
          _offset(baseBuilder.len()),
          _s(this),
          _tracker(0),
          _doneCalled(false) {
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
        : _b(existingBuilder),
          _buf(0),
          _offset(offset),
          _s(this),
          _tracker(nullptr),
          _doneCalled(false) {
        invariant(_b.len() >= BSONObj::kMinBSONLength);
        _b.setlen(_b.len() - 1);  // get rid of the previous EOO.
        // Reserve space for our EOO.
        _b.reserveBytes(1);
    }

    BSONObjBuilder(const BSONSizeTracker& tracker)
        : _b(_buf),
          _buf(tracker.getSize()),
          _offset(0),
          _s(this),
          _tracker(const_cast<BSONSizeTracker*>(&tracker)),
          _doneCalled(false) {
        // See the comments in the first constructor for details.
        _b.skip(sizeof(int));

        // Reserve space for the EOO byte. This means _done() can't fail.
        _b.reserveBytes(1);
    }

    ~BSONObjBuilder() {
        // If 'done' has not already been called, and we have a reference to an owning
        // BufBuilder but do not own it ourselves, then we must call _done to write in the
        // length. Otherwise, we own this memory and its lifetime ends with us, therefore
        // we can elide the write.
        if (!_doneCalled && _b.buf() && _buf.getSize() == 0) {
            _done();
        }
    }

    /** add all the fields from the object specified to this object */
    BSONObjBuilder& appendElements(BSONObj x);

    /** add all the fields from the object specified to this object if they don't exist already */
    BSONObjBuilder& appendElementsUnique(BSONObj x);

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

    /** Append a boolean element */
    BSONObjBuilder& append(StringData fieldName, bool val) {
        _b.appendNum((char)Bool);
        _b.appendStr(fieldName);
        _b.appendNum((char)(val ? 1 : 0));
        return *this;
    }

    /** Append a 32 bit integer element */
    BSONObjBuilder& append(StringData fieldName, int n) {
        _b.appendNum((char)NumberInt);
        _b.appendStr(fieldName);
        _b.appendNum(n);
        return *this;
    }

    /** Append a 32 bit unsigned element - cast to a signed int. */
    BSONObjBuilder& append(StringData fieldName, unsigned n) {
        return append(fieldName, (int)n);
    }

    /** Append a NumberDecimal */
    BSONObjBuilder& append(StringData fieldName, Decimal128 n) {
        _b.appendNum(static_cast<char>(NumberDecimal));
        _b.appendStr(fieldName);
        // Make sure we write data in a Little Endian conforming manner
        _b.appendNum(n);
        return *this;
    }

    /** Append a NumberLong */
    BSONObjBuilder& append(StringData fieldName, long long n) {
        _b.appendNum((char)NumberLong);
        _b.appendStr(fieldName);
        _b.appendNum(n);
        return *this;
    }

    /**
     * Append a NumberLong (if int64_t isn't the same as long long)
     */
    template <typename Int64_t,
              typename = stdx::enable_if_t<std::is_same<Int64_t, int64_t>::value &&
                                           !std::is_same<int64_t, long long>::value>>
    BSONObjBuilder& append(StringData fieldName, Int64_t n) {
        _b.appendNum((char)NumberLong);
        _b.appendStr(fieldName);
        _b.appendNum(n);
        return *this;
    }

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

    /** Append a double element */
    BSONObjBuilder& append(StringData fieldName, double n) {
        _b.appendNum((char)NumberDouble);
        _b.appendStr(fieldName);
        _b.appendNum(n);
        return *this;
    }

    /** Append a BSON Object ID (OID type).
        @deprecated Generally, it is preferred to use the append append(name, oid)
        method for this.
    */
    BSONObjBuilder& appendOID(StringData fieldName, OID* oid = 0, bool generateIfBlank = false) {
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

    /** Append a std::string element.
        @param sz size includes terminating null character */
    BSONObjBuilder& append(StringData fieldName, const char* str, int sz) {
        _b.appendNum((char)String);
        _b.appendStr(fieldName);
        _b.appendNum((int)sz);
        _b.appendBuf(str, sz);
        return *this;
    }
    /** Append a std::string element */
    BSONObjBuilder& append(StringData fieldName, const char* str) {
        return append(fieldName, str, (int)strlen(str) + 1);
    }
    /** Append a std::string element */
    BSONObjBuilder& append(StringData fieldName, const std::string& str) {
        return append(fieldName, str.c_str(), (int)str.size() + 1);
    }
    /** Append a std::string element */
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

    /** Implements builder interface but no-op in ObjBuilder */
    void appendNull() {
        msgasserted(16234, "Invalid call to appendNull in BSONObj Builder.");
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

    void appendUndefined(StringData fieldName) {
        _b.appendNum((char)Undefined);
        _b.appendStr(fieldName);
    }

    /* helper function -- see Query::where() for primary way to do this. */
    void appendWhere(StringData code, const BSONObj& scope) {
        appendCodeWScope("$where", code, scope);
    }

    /**
       these are the min/max when comparing, not strict min/max elements for a given type
    */
    void appendMinForType(StringData fieldName, int type);
    void appendMaxForType(StringData fieldName, int type);

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
    BSONObj obj() {
        massert(10335, "builder does not own memory", owned());
        doneFast();
        return BSONObj(_b.release());
    }

    /** Fetch the object we have built.
        BSONObjBuilder still frees the object when the builder goes out of
        scope -- very important to keep in mind.  Use obj() if you
        would like the BSONObj to last longer than the builder.
    */
    BSONObj done() {
        return BSONObj(_done());
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
        BSONObj temp(_done());
        _b.setlen(_b.len() - 1);  // next append should overwrite the EOO
        _b.reserveBytes(1);       // Rereserve room for the real EOO
        _doneCalled = false;
        return temp;
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

    void appendKeys(const BSONObj& keyPattern, const BSONObj& values);

    static std::string numStr(int i) {
        if (i >= 0 && i < 100 && numStrsReady)
            return numStrs[i];
        StringBuilder o;
        o << i;
        return o.str();
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

        _doneCalled = true;

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
        return data;
    }

    BufBuilder& _b;
    BufBuilder _buf;
    int _offset;
    BSONObjBuilderValueStream _s;
    BSONSizeTracker* _tracker;
    bool _doneCalled;

    static const std::string numStrs[100];  // cache of 0 to 99 inclusive
    static bool numStrsReady;               // for static init safety
};

class BSONArrayBuilder {
    MONGO_DISALLOW_COPYING(BSONArrayBuilder);

public:
    BSONArrayBuilder() : _i(0), _b() {}
    BSONArrayBuilder(BufBuilder& _b) : _i(0), _b(_b) {}
    BSONArrayBuilder(int initialSize) : _i(0), _b(initialSize) {}

    template <typename T>
    BSONArrayBuilder& append(const T& x) {
        ItoA itoa(_i++);
        _b.append(itoa, x);
        return *this;
    }

    BSONArrayBuilder& append(const BSONElement& e) {
        ItoA itoa(_i++);
        _b.appendAs(e, itoa);
        return *this;
    }

    BSONArrayBuilder& operator<<(const BSONElement& e) {
        return append(e);
    }

    template <typename T>
    BSONArrayBuilder& operator<<(const T& x) {
        ItoA itoa(_i++);
        _b << itoa << x;
        return *this;
    }

    void appendNull() {
        ItoA itoa(_i++);
        _b.appendNull(itoa);
    }

    void appendUndefined() {
        ItoA itoa(_i++);
        _b.appendUndefined(itoa);
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
        ItoA itoa(_i++);
        return _b.subobjStart(itoa);
    }
    BufBuilder& subarrayStart() {
        ItoA itoa(_i++);
        return _b.subarrayStart(itoa);
    }

    BSONArrayBuilder& appendRegex(StringData regex, StringData options = "") {
        ItoA itoa(_i++);
        _b.appendRegex(itoa, regex, options);
        return *this;
    }

    BSONArrayBuilder& appendBinData(int len, BinDataType type, const void* data) {
        ItoA itoa(_i++);
        _b.appendBinData(itoa, len, type, data);
        return *this;
    }

    BSONArrayBuilder& appendCode(StringData code) {
        ItoA itoa(_i++);
        _b.appendCode(itoa, code);
        return *this;
    }

    BSONArrayBuilder& appendCodeWScope(StringData code, const BSONObj& scope) {
        ItoA itoa(_i++);
        _b.appendCodeWScope(itoa, code, scope);
        return *this;
    }

    BSONArrayBuilder& appendTimeT(time_t dt) {
        ItoA itoa(_i++);
        _b.appendTimeT(itoa, dt);
        return *this;
    }

    BSONArrayBuilder& appendDate(Date_t dt) {
        ItoA itoa(_i++);
        _b.appendDate(itoa, dt);
        return *this;
    }

    BSONArrayBuilder& appendBool(bool val) {
        ItoA itoa(_i++);
        _b.appendBool(itoa, val);
        return *this;
    }

    BSONArrayBuilder& appendTimestamp(unsigned long long ts) {
        ItoA itoa(_i++);
        _b.appendTimestamp(itoa, ts);
        return *this;
    }

    bool isArray() const {
        return true;
    }

    int len() const {
        return _b.len();
    }
    int arrSize() const {
        return _i;
    }

    BufBuilder& bb() {
        return _b.bb();
    }

private:
    std::uint32_t _i;
    BSONObjBuilder _b;
};

template <class T>
inline BSONObjBuilder& BSONObjBuilder::append(StringData fieldName, const std::vector<T>& vals) {
    BSONObjBuilder arrBuilder(subarrayStart(fieldName));
    for (unsigned int i = 0; i < vals.size(); ++i)
        arrBuilder.append(numStr(i), vals[i]);
    return *this;
}

template <class L>
inline BSONObjBuilder& _appendIt(BSONObjBuilder& _this, StringData fieldName, const L& vals) {
    BSONObjBuilder arrBuilder;
    int n = 0;
    for (typename L::const_iterator i = vals.begin(); i != vals.end(); i++)
        arrBuilder.append(BSONObjBuilder::numStr(n++), *i);
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

// $or helper: OR(BSON("x" << GT << 7), BSON("y" << LT 6));
inline BSONObj OR(const BSONObj& a, const BSONObj& b) {
    return BSON("$or" << BSON_ARRAY(a << b));
}
inline BSONObj OR(const BSONObj& a, const BSONObj& b, const BSONObj& c) {
    return BSON("$or" << BSON_ARRAY(a << b << c));
}
inline BSONObj OR(const BSONObj& a, const BSONObj& b, const BSONObj& c, const BSONObj& d) {
    return BSON("$or" << BSON_ARRAY(a << b << c << d));
}
inline BSONObj OR(
    const BSONObj& a, const BSONObj& b, const BSONObj& c, const BSONObj& d, const BSONObj& e) {
    return BSON("$or" << BSON_ARRAY(a << b << c << d << e));
}
inline BSONObj OR(const BSONObj& a,
                  const BSONObj& b,
                  const BSONObj& c,
                  const BSONObj& d,
                  const BSONObj& e,
                  const BSONObj& f) {
    return BSON("$or" << BSON_ARRAY(a << b << c << d << e << f));
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
