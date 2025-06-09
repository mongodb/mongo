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

#include "mongo/base/static_assert.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/stdx/utility.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/intrusive_counter.h"

#include <algorithm>
#include <cstdlib>
#include <new>

#include <boost/intrusive_ptr.hpp>


namespace mongo {
class Document;
class DocumentStorage;
class Value;


/** An immutable reference-counted string of inline data. */
class RCString final : public RefCountable {
public:
    static boost::intrusive_ptr<const RCString> create(StringData s) {
        static constexpr size_t sizeLimit = BSONObjMaxUserSize;
        uassert(16493,
                fmt::format("RCString too large. Requires size={} < limit={}", s.size(), sizeLimit),
                s.size() < sizeLimit);
        return boost::intrusive_ptr{new (s) RCString{s}};
    }

    explicit operator StringData() const noexcept {
        return StringData{_data(), _size};
    }

    void* operator new(size_t, StringData s) {
        return ::operator new(_allocSize(s.size()));
    }

    /** Used if constructor fails after placement `new (StringData)`. */
    void operator delete(void* ptr, StringData s) {
        ::operator delete(ptr, _allocSize(s.size()));
    }

#if __cpp_lib_destroying_delete >= 201806L
    void operator delete(RCString* ptr, std::destroying_delete_t) {
        size_t sz = _allocSize(ptr->_size);
        ptr->~RCString();
        ::operator delete(ptr, sz);
    }
#else   // !__cpp_lib_destroying_delete
    /** Invoked by virtual destructor. */
    void operator delete(void* ptr) {
        ::operator delete(ptr);
    }
#endif  // __cpp_lib_destroying_delete

private:
    static size_t _allocSize(size_t stringSize) {
        return sizeof(RCString) + stringSize + 1;  // Incl. '\0'-terminator
    }

    /** Use static `create()` instead. */
    explicit RCString(StringData s) : _size{s.size()} {
        if (_size)
            memcpy(_data(), s.data(), _size);
        _data()[_size] = '\0';
    }

    const char* _data() const noexcept {
        return reinterpret_cast<const char*>(this + 1);
    }
    char* _data() noexcept {
        return const_cast<char*>(std::as_const(*this)._data());
    }

    size_t _size; /** Excluding '\0' terminator. */
};

// TODO: a MutableVector, similar to MutableDocument
/// A heap-allocated reference-counted std::vector
template <typename T>
class RCVector : public RefCountable {
public:
    RCVector() {}
    RCVector(std::vector<T> v) : vec(std::move(v)) {}
    std::vector<T> vec;
};

class RCCodeWScope : public RefCountable {
public:
    RCCodeWScope(const std::string& str, BSONObj obj) : code(str), scope(obj.getOwned()) {}
    const std::string code;
    const BSONObj scope;  // Not worth converting to Document for now
};

class RCDBRef : public RefCountable {
public:
    RCDBRef(const std::string& str, const OID& o) : ns(str), oid(o) {}
    const std::string ns;
    const OID oid;
};

class RCDecimal : public RefCountable {
public:
    RCDecimal(const Decimal128& decVal) : decimalValue(decVal) {}
    const Decimal128 decimalValue;
};

class ValueStorage {
public:
    // Note: it is important the memory is zeroed out (by calling zero()) at the start of every
    // constructor. Much code relies on every byte being predictably initialized to zero.

    // This is a "missing" Value
    ValueStorage() : ValueStorage(BSONType::eoo) {}

    explicit ValueStorage(BSONType t) {
        zero();
        type = stdx::to_underlying(t);
    }
    ValueStorage(BSONType t, int i) {
        zero();
        type = stdx::to_underlying(t);
        intValue = i;
    }
    ValueStorage(BSONType t, long long l) {
        zero();
        type = stdx::to_underlying(t);
        longValue = l;
    }
    ValueStorage(BSONType t, double d) {
        zero();
        type = stdx::to_underlying(t);
        doubleValue = d;
    }
    ValueStorage(BSONType t, const Decimal128& d) {
        zero();
        type = stdx::to_underlying(t);
        putDecimal(d);
    }
    ValueStorage(BSONType t, Timestamp r) {
        zero();
        type = stdx::to_underlying(t);
        timestampValue = r.asULL();
    }
    ValueStorage(BSONType t, bool b) {
        zero();
        type = stdx::to_underlying(t);
        boolValue = b;
    }
    ValueStorage(BSONType t, const Document& d) {
        zero();
        type = stdx::to_underlying(t);
        putDocument(d);
    }
    ValueStorage(BSONType t, Document&& d) {
        zero();
        type = stdx::to_underlying(t);
        putDocument(std::move(d));
    }
    ValueStorage(BSONType t, boost::intrusive_ptr<RCVector<Value>>&& a) {
        zero();
        type = stdx::to_underlying(t);
        putVector(std::move(a));
    }
    ValueStorage(BSONType t, StringData s) {
        zero();
        type = stdx::to_underlying(t);
        putString(s);
    }
    ValueStorage(BSONType t, const BSONBinData& bd) {
        zero();
        type = stdx::to_underlying(t);
        putBinData(bd);
    }
    ValueStorage(BSONType t, const BSONRegEx& re) {
        zero();
        type = stdx::to_underlying(t);
        putRegEx(re);
    }
    ValueStorage(BSONType t, const BSONCodeWScope& cs) {
        zero();
        type = stdx::to_underlying(t);
        putCodeWScope(cs);
    }
    ValueStorage(BSONType t, const BSONDBRef& dbref) {
        zero();
        type = stdx::to_underlying(t);
        putDBRef(dbref);
    }

    ValueStorage(BSONType t, const OID& o) {
        zero();
        type = stdx::to_underlying(t);
        memcpy(&oid, o.view().view(), OID::kOIDSize);
    }

    ValueStorage(const ValueStorage& rhs) {
        memcpy(bytes, rhs.bytes, sizeof(bytes));
        memcpyed();
    }

    ValueStorage(ValueStorage&& rhs) noexcept {
        memcpy(bytes, rhs.bytes, sizeof(bytes));
        rhs.zero();  // Reset rhs to the missing state. TODO consider only doing this if refCounter.
    }

    ~ValueStorage() {
        if (kDebugBuild)
            verifyRefCountingIfShould();
        if (refCounter)
            intrusive_ptr_release(genericRCPtr);
        if (kDebugBuild)
            memset(bytes, 0xee, sizeof(bytes));
    }

    ValueStorage& operator=(const ValueStorage& rhs) {
        // This is designed to be effectively a no-op on self-assign, without needing an explicit
        // check. This requires that rhs's refcount is incremented before ours is released, and that
        // we use memmove rather than memcpy.
        if (kDebugBuild)
            rhs.verifyRefCountingIfShould();
        if (rhs.refCounter)
            intrusive_ptr_add_ref(rhs.genericRCPtr);

        if (kDebugBuild)
            verifyRefCountingIfShould();
        if (refCounter)
            intrusive_ptr_release(genericRCPtr);

        memmove(bytes, rhs.bytes, sizeof(bytes));
        return *this;
    }

    ValueStorage& operator=(ValueStorage&& rhs) noexcept {
        if (kDebugBuild)
            verifyRefCountingIfShould();
        if (refCounter)
            intrusive_ptr_release(genericRCPtr);

        memmove(bytes, rhs.bytes, sizeof(bytes));
        rhs.zero();  // Reset rhs to the missing state. TODO consider only doing this if refCounter.
        return *this;
    }

    void swap(ValueStorage& rhs) {
        // Don't need to update ref-counts because they will be the same in the end
        char temp[sizeof(bytes)];
        memcpy(temp, bytes, sizeof(bytes));
        memcpy(bytes, rhs.bytes, sizeof(bytes));
        memcpy(rhs.bytes, temp, sizeof(bytes));
    }

    /// Call this after memcpying to update ref counts if needed
    void memcpyed() const {
        if (kDebugBuild)
            verifyRefCountingIfShould();
        if (refCounter)
            intrusive_ptr_add_ref(genericRCPtr);
    }

    /// These are only to be called during Value construction on an empty Value
    void putString(StringData s);
    void putVector(boost::intrusive_ptr<RCVector<Value>>&& v);
    void putDocument(const Document& d);
    void putDocument(Document&& d);
    void putRegEx(const BSONRegEx& re);
    void putBinData(const BSONBinData& bd) {
        putRefCountable(RCString::create(StringData(static_cast<const char*>(bd.data), bd.length)));
        binSubType = bd.type;
    }

    void putDBRef(const BSONDBRef& dbref) {
        putRefCountable(make_intrusive<RCDBRef>(std::string{dbref.ns}, dbref.oid));
    }

    void putCodeWScope(const BSONCodeWScope& cws) {
        putRefCountable(make_intrusive<RCCodeWScope>(std::string{cws.code}, cws.scope));
    }

    void putDecimal(const Decimal128& d) {
        putRefCountable(make_intrusive<RCDecimal>(d));
    }

    void putRefCountable(boost::intrusive_ptr<const RefCountable>&& ptr) {
        genericRCPtr = ptr.detach();

        if (genericRCPtr) {
            refCounter = true;
        }
        if (kDebugBuild)
            verifyRefCountingIfShould();
    }

    StringData getString() const {
        if (shortStr) {
            return StringData(shortStrStorage, shortStrSize);
        } else {
            dassert(typeid(*genericRCPtr) == typeid(const RCString));
            const RCString* stringPtr = static_cast<const RCString*>(genericRCPtr);
            return StringData{*stringPtr};
        }
    }

    const std::vector<Value>& getArray() const {
        dassert(typeid(*genericRCPtr) == typeid(const RCVector<Value>));
        const RCVector<Value>* arrayPtr = static_cast<const RCVector<Value>*>(genericRCPtr);
        return arrayPtr->vec;
    }

    boost::intrusive_ptr<const RCCodeWScope> getCodeWScope() const {
        dassert(typeid(*genericRCPtr) == typeid(const RCCodeWScope));
        return static_cast<const RCCodeWScope*>(genericRCPtr);
    }

    boost::intrusive_ptr<const RCDBRef> getDBRef() const {
        dassert(typeid(*genericRCPtr) == typeid(const RCDBRef));
        return static_cast<const RCDBRef*>(genericRCPtr);
    }

    Decimal128 getDecimal() const {
        dassert(typeid(*genericRCPtr) == typeid(const RCDecimal));
        const RCDecimal* decPtr = static_cast<const RCDecimal*>(genericRCPtr);
        return decPtr->decimalValue;
    }

    // Document is incomplete here so this can't be inline
    Document getDocument() const;

    BSONType bsonType() const {
        return BSONType(type);
    }

    BinDataType binDataType() const {
        dassert(type == stdx::to_underlying(BSONType::binData));
        return BinDataType(binSubType);
    }

    void zero() {
        memset(bytes, 0, sizeof(bytes));
    }

    void verifyRefCountingIfShould() const {
        switch (type) {
            case stdx::to_underlying(BSONType::minKey):
            case stdx::to_underlying(BSONType::maxKey):
            case stdx::to_underlying(BSONType::oid):
            case stdx::to_underlying(BSONType::date):
            case stdx::to_underlying(BSONType::timestamp):
            case stdx::to_underlying(BSONType::eoo):
            case stdx::to_underlying(BSONType::null):
            case stdx::to_underlying(BSONType::undefined):
            case stdx::to_underlying(BSONType::boolean):
            case stdx::to_underlying(BSONType::numberInt):
            case stdx::to_underlying(BSONType::numberLong):
            case stdx::to_underlying(BSONType::numberDouble):
                // the above types never reference external data
                MONGO_verify(!refCounter);
                break;

            case stdx::to_underlying(BSONType::string):
            case stdx::to_underlying(BSONType::regEx):
            case stdx::to_underlying(BSONType::code):
            case stdx::to_underlying(BSONType::symbol):
                // If this is using the short-string optimization, it must not have a ref-counted
                // pointer.
                invariant(!shortStr || !refCounter);

                // If this is _not_ using the short string optimization, it must be storing a
                // ref-counted pointer. One exception: in the BSONElement constructor of Value, it
                // is possible for this ValueStorage to get constructed as a type but never
                // initialized; the ValueStorage gets left as a nullptr and not marked as
                // ref-counted, which is ok (SERVER-43205).
                invariant(shortStr || (refCounter || !genericRCPtr));
                break;

            case stdx::to_underlying(BSONType::numberDecimal):
            case stdx::to_underlying(
                BSONType::binData):  // TODO this should probably support short-string optimization
            case stdx::to_underlying(
                BSONType::array):  // TODO this should probably support empty-is-NULL optimization
            case stdx::to_underlying(BSONType::dbRef):
            case stdx::to_underlying(BSONType::codeWScope):
                // the above types always reference external data.
                invariant(refCounter);
                invariant(bool(genericRCPtr));
                break;

            case stdx::to_underlying(BSONType::object):
                // Objects either hold a NULL ptr or should be ref-counting
                invariant(refCounter == bool(genericRCPtr));
                break;
        }
    }

    // This data is public because this should only be used by Value which would be a friend
    union {
        // cover the whole ValueStorage
        uint8_t bytes[16];
#pragma pack(1)
        struct {
            // bytes[0]
            signed char type;

            // bytes[1]
            struct {
                uint8_t refCounter : 1;  // bit 0: true if we need to refCount
                uint8_t shortStr : 1;    // bit 1: true if we are using short strings
                uint8_t reservedFlags : 6;
            };

            // bytes[2:15]
            union {
                unsigned char oid[12];

                struct {
                    char shortStrSize;  // TODO Consider moving into flags union (4 bits)
                    char shortStrStorage[sizeof(bytes) - 3 /*offset*/ - 1 /*NUL byte*/];
                    union {
                        char nulTerminator;
                    };
                };

                struct {
                    union {
                        unsigned char binSubType;
                        char pad[6];
                        char stringCache[6];  // TODO copy first few bytes of strings in here
                    };
                    union {  // 8 bytes long and 8-byte aligned
                        // There should be no pointers to non-const data
                        const RefCountable* genericRCPtr;

                        double doubleValue;
                        bool boolValue;
                        int intValue;
                        long long longValue;
                        unsigned long long timestampValue;
                        long long dateValue;
                    };
                };
            };
        };
#pragma pack()

        // Select void* alignment without interfering with any active pack directives. Can't use
        // alignas(void*) on this union because that would prohibit ValueStorage from being tightly
        // packed into a packed struct (though GCC does the tight packing anyway).
        //
        // Note that MSVC's behavior is GCC-incompatible. It obeys alignas even when a pack pragma
        // is active. That causes padding on MSVC when ValueStorage is used as a member of class
        // Value, which in turn is used as a member of packed class ValueElement.
        // http://lists.llvm.org/pipermail/cfe-dev/2014-July/thread.html#38174
        void* pointerAlignment;
    };
};
MONGO_STATIC_ASSERT(sizeof(ValueStorage) == 16);
MONGO_STATIC_ASSERT(alignof(ValueStorage) >= alignof(void*));
}  // namespace mongo
