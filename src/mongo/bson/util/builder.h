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

#include <cfloat>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <type_traits>

#include <boost/optional.hpp>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concepts.h"
#include "mongo/util/itoa.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/shared_buffer_fragment.h"

namespace mongo {

/* Note the limit here is rather arbitrary and is simply a standard. generally the code works
   with any object that fits in ram.

   Also note that the server has some basic checks to enforce this limit but those checks are not
   exhaustive for example need to check for size too big after
     update $push (append) operation
*/
const int BSONObjMaxUserSize = 16 * 1024 * 1024;

/*
   Sometimes we need objects slightly larger - an object in the replication local.oplog
   is slightly larger than a user object for example.
*/
const int BSONObjMaxInternalSize = BSONObjMaxUserSize + (16 * 1024);

const int BufferMaxSize = 64 * 1024 * 1024;

template <typename Builder>
class StringBuilderImpl;

class SharedBufferAllocator {
    SharedBufferAllocator(const SharedBufferAllocator&) = delete;
    SharedBufferAllocator& operator=(const SharedBufferAllocator&) = delete;

public:
    SharedBufferAllocator() = default;
    SharedBufferAllocator(size_t sz) {
        if (sz > 0)
            malloc(sz);
    }
    SharedBufferAllocator(SharedBuffer buf) : _buf(std::move(buf)) {
        invariant(!_buf.isShared());
    }

    // Allow moving but not copying. It would be an error for two SharedBufferAllocators to use the
    // same underlying buffer.
    SharedBufferAllocator(SharedBufferAllocator&&) = default;
    SharedBufferAllocator& operator=(SharedBufferAllocator&&) = default;

    void malloc(size_t sz) {
        _buf = SharedBuffer::allocate(sz);
    }

    void realloc(size_t sz) {
        _buf.realloc(sz);
    }

    void free() {
        _buf = {};
    }

    SharedBuffer release() {
        return std::move(_buf);
    }

    size_t capacity() const {
        return _buf.capacity();
    }

    char* get() const {
        return _buf.get();
    }

private:
    SharedBuffer _buf;
};

class SharedBufferFragmentAllocator {
    SharedBufferFragmentAllocator(const SharedBufferFragmentAllocator&) = delete;
    SharedBufferFragmentAllocator& operator=(const SharedBufferFragmentAllocator&) = delete;

public:
    SharedBufferFragmentAllocator(SharedBufferFragmentBuilder& fragmentBuilder)
        : _fragmentBuilder(fragmentBuilder) {}
    ~SharedBufferFragmentAllocator() {
        // Discard if the build was not finished at the time of destruction.
        if (_fragmentBuilder.building()) {
            free();
        }
    }

    // Allow moving but not copying. It would be an error for two SharedBufferFragmentAllocator to
    // use the same underlying builder at the same time.
    SharedBufferFragmentAllocator(SharedBufferFragmentAllocator&&) = default;
    SharedBufferFragmentAllocator& operator=(SharedBufferFragmentAllocator&&) = default;

    void malloc(size_t sz) {
        start(sz);
    }

    void realloc(size_t sz) {
        auto capacity = _fragmentBuilder.capacity();
        if (capacity < sz)
            _fragmentBuilder.grow(sz);
    }

    void free() {
        _fragmentBuilder.discard();
    }

    void start(size_t sz) {
        _fragmentBuilder.start(sz);
    }

    SharedBufferFragment finish(int sz) {
        return _fragmentBuilder.finish(sz);
    }

    size_t capacity() const {
        return _fragmentBuilder.capacity();
    }

    char* get() const {
        return _fragmentBuilder.get();
    }

private:
    SharedBufferFragmentBuilder& _fragmentBuilder;
};

enum { StackSizeDefault = 512 };
template <size_t SZ>
class StackAllocator {
    StackAllocator(const StackAllocator&) = delete;
    StackAllocator& operator=(const StackAllocator&) = delete;
    StackAllocator(StackAllocator&&) = delete;
    StackAllocator& operator=(StackAllocator&&) = delete;

public:
    StackAllocator() = default;
    ~StackAllocator() {
        free();
    }

    void malloc(size_t sz) {
        if (sz > SZ) {
            _ptr = mongoMalloc(sz);
            _capacity = sz;
        }
    }

    void realloc(size_t sz) {
        if (_ptr == _buf) {
            if (sz > SZ) {
                _ptr = mongoMalloc(sz);
                memcpy(_ptr, _buf, SZ);
                _capacity = sz;
            } else {
                _capacity = SZ;
            }
        } else {
            _ptr = mongoRealloc(_ptr, sz);
            _capacity = sz;
        }
    }

    void free() {
        if (_ptr != _buf)
            ::free(_ptr);
        _ptr = _buf;
        _capacity = SZ;
    }

    size_t capacity() const {
        return _capacity;
    }

    char* get() const {
        return static_cast<char*>(_ptr);
    }

private:
    char _buf[SZ];
    size_t _capacity = SZ;

    void* _ptr = _buf;
};

template <class BufferAllocator>
class BasicBufBuilder {
public:
    template <typename... AllocatorArgs>
    BasicBufBuilder(AllocatorArgs&&... args) : _buf(std::forward<AllocatorArgs>(args)...) {}

    void kill() {
        _buf.free();
    }

    void reset() {
        l = 0;
        reservedBytes = 0;
    }
    void reset(size_t maxSize) {
        l = 0;
        reservedBytes = 0;
        if (maxSize && _buf.capacity() > maxSize) {
            _buf.free();
            _buf.malloc(maxSize);
        }
    }

    /** leave room for some stuff later
        @return point to region that was skipped.  pointer may change later (on realloc), so for
        immediate use only
    */
    char* skip(int n) {
        return grow(n);
    }

    /* note this may be deallocated (realloced) if you keep writing. */
    char* buf() {
        return _buf.get();
    }
    const char* buf() const {
        return _buf.get();
    }

    void appendUChar(unsigned char j) {
        MONGO_STATIC_ASSERT(CHAR_BIT == 8);
        appendNumImpl(j);
    }
    void appendChar(char j) {
        appendNumImpl(j);
    }
    void appendNum(char j) {
        appendNumImpl(j);
    }
    void appendNum(short j) {
        MONGO_STATIC_ASSERT(sizeof(short) == 2);
        appendNumImpl(j);
    }
    void appendNum(int j) {
        MONGO_STATIC_ASSERT(sizeof(int) == 4);
        appendNumImpl(j);
    }
    void appendNum(unsigned j) {
        appendNumImpl(j);
    }

    // Bool does not have a well defined encoding.
    void appendNum(bool j) = delete;

    void appendNum(double j) {
        MONGO_STATIC_ASSERT(sizeof(double) == 8);
        appendNumImpl(j);
    }
    void appendNum(long long j) {
        MONGO_STATIC_ASSERT(sizeof(long long) == 8);
        appendNumImpl(j);
    }

    void appendNum(Decimal128 j) {
        BOOST_STATIC_ASSERT(sizeof(Decimal128::Value) == 16);
        Decimal128::Value value = j.getValue();
        long long low = value.low64;
        long long high = value.high64;
        appendNumImpl(low);
        appendNumImpl(high);
    }

    REQUIRES_FOR_NON_TEMPLATE(!std::is_same_v<int64_t, long long>)
    void appendNum(int64_t j) {
        appendNumImpl(j);
    }

    void appendNum(unsigned long long j) {
        appendNumImpl(j);
    }

    void appendBuf(const void* src, size_t len) {
        if (len)
            memcpy(grow((int)len), src, len);
    }

    template <class T>
    void appendStruct(const T& s) {
        appendBuf(&s, sizeof(T));
    }

    void appendStr(StringData str, bool includeEndingNull = true) {
        const int len = str.size() + (includeEndingNull ? 1 : 0);
        str.copyTo(grow(len), includeEndingNull);
    }

    /** @return length of current std::string */
    int len() const {
        return l;
    }
    void setlen(int newLen) {
        l = newLen;
    }
    /** @return size of the buffer */
    int getSize() const {
        return _buf.capacity();
    }

    /* returns the pre-grow write position */
    inline char* grow(int by) {
        int oldlen = l;
        int newLen = l + by;
        size_t minSize = newLen + reservedBytes;
        if (minSize > _buf.capacity()) {
            grow_reallocate(minSize);
        }
        l = newLen;
        return _buf.get() + oldlen;
    }

    /**
     * Reserve room for some number of bytes to be claimed at a later time.
     */
    void reserveBytes(size_t bytes) {
        size_t minSize = l + reservedBytes + bytes;
        if (minSize > _buf.capacity())
            grow_reallocate(minSize);

        // This must happen *after* any attempt to grow.
        reservedBytes += bytes;
    }

    /**
     * Claim an earlier reservation of some number of bytes. These bytes must already have been
     * reserved. Appends of up to this many bytes immediately following a claim are
     * guaranteed to succeed without a need to reallocate.
     */
    void claimReservedBytes(int bytes) {
        invariant(reservedBytes >= bytes);
        reservedBytes -= bytes;
    }

    /**
     * Replaces the buffer backing this BufBuilder with the passed in SharedBuffer.
     * Only legal to call when this builder is empty and when the SharedBuffer isn't shared.
     */
    REQUIRES_FOR_NON_TEMPLATE(std::is_same_v<BufferAllocator, SharedBufferAllocator>)
    void useSharedBuffer(SharedBuffer buf) {
        invariant(l == 0);  // Can only do this while empty.
        invariant(reservedBytes == 0);
        _buf = SharedBufferAllocator(std::move(buf));
    }

protected:
    template <typename T>
    void appendNumImpl(T t) {
        // NOTE: For now, we assume that all things written
        // by a BufBuilder are intended for external use: either written to disk
        // or to the wire. Since all of our encoding formats are little endian,
        // we bake that assumption in here. This decision should be revisited soon.
        DataView(grow(sizeof(t))).write(tagLittleEndian(t));
    }
    /* "slow" portion of 'grow()'  */
    void grow_reallocate(int minSize) {
        if (minSize > BufferMaxSize) {
            std::stringstream ss;
            ss << "BufBuilder attempted to grow() to " << minSize << " bytes, past the 64MB limit.";
            msgasserted(13548, ss.str().c_str());
        }

        int a = 64;
        while (a < minSize)
            a = a * 2;

        _buf.realloc(a);
    }


    BufferAllocator _buf;
    int l{0};
    int reservedBytes{0};  // eagerly grow_reallocate to keep this many bytes of spare room.

    template <class Builder>
    friend class StringBuilderImpl;
};

class BufBuilder : public BasicBufBuilder<SharedBufferAllocator> {
public:
    static constexpr size_t kDefaultInitSizeBytes = 512;
    BufBuilder(size_t initsize = kDefaultInitSizeBytes) : BasicBufBuilder(initsize) {}

    /* assume ownership of the buffer */
    SharedBuffer release() {
        return _buf.release();
    }
};
class PooledFragmentBuilder : public BasicBufBuilder<SharedBufferFragmentAllocator> {
public:
    PooledFragmentBuilder(SharedBufferFragmentBuilder& fragmentBuilder)
        : BasicBufBuilder(fragmentBuilder) {
        // Indicate that we are starting to build a fragment but rely on the builder for the block
        // size
        _buf.start(0);
    }

    SharedBufferFragment done() {
        return _buf.finish(l);
    }
};
MONGO_STATIC_ASSERT(std::is_move_constructible_v<BufBuilder>);

/** The StackBufBuilder builds smaller datasets on the stack instead of using malloc.
      this can be significantly faster for small bufs.  However, you can not release() the
      buffer with StackBufBuilder.
    While designed to be a variable on the stack, if you were to dynamically allocate one,
      nothing bad would happen.  In fact in some circumstances this might make sense, say,
      embedded in some other object.
*/
template <size_t SZ>
class StackBufBuilderBase : public BasicBufBuilder<StackAllocator<SZ>> {
public:
    StackBufBuilderBase() : BasicBufBuilder<StackAllocator<SZ>>() {}
    StackBufBuilderBase(const StackBufBuilderBase&) = delete;
    StackBufBuilderBase(StackBufBuilderBase&&) = delete;
};
using StackBufBuilder = StackBufBuilderBase<StackSizeDefault>;
MONGO_STATIC_ASSERT(!std::is_move_constructible<StackBufBuilder>::value);

/** std::stringstream deals with locale so this is a lot faster than std::stringstream for UTF8 */
template <typename Builder>
class StringBuilderImpl {
public:
    // Sizes are determined based on the number of characters in 64-bit + the trailing '\0'
    static const size_t MONGO_DBL_SIZE = 3 + DBL_MANT_DIG - DBL_MIN_EXP + 1;
    static const size_t MONGO_S32_SIZE = 12;
    static const size_t MONGO_U32_SIZE = 11;
    static const size_t MONGO_S64_SIZE = 23;
    static const size_t MONGO_U64_SIZE = 22;
    static const size_t MONGO_S16_SIZE = 7;
    static const size_t MONGO_PTR_SIZE = 19;  // Accounts for the 0x prefix

    StringBuilderImpl() {}

    StringBuilderImpl& operator<<(double x) {
        return SBNUM(x, MONGO_DBL_SIZE, "%g");
    }
    StringBuilderImpl& operator<<(int x) {
        return appendIntegral(x, MONGO_S32_SIZE);
    }
    StringBuilderImpl& operator<<(unsigned x) {
        return appendIntegral(x, MONGO_U32_SIZE);
    }
    StringBuilderImpl& operator<<(long x) {
        return appendIntegral(x, MONGO_S64_SIZE);
    }
    StringBuilderImpl& operator<<(unsigned long x) {
        return appendIntegral(x, MONGO_U64_SIZE);
    }
    StringBuilderImpl& operator<<(long long x) {
        return appendIntegral(x, MONGO_S64_SIZE);
    }
    StringBuilderImpl& operator<<(unsigned long long x) {
        return appendIntegral(x, MONGO_U64_SIZE);
    }
    StringBuilderImpl& operator<<(short x) {
        return appendIntegral(x, MONGO_S16_SIZE);
    }
    StringBuilderImpl& operator<<(const void* x) {
        if (sizeof(x) == 8) {
            return SBNUM(x, MONGO_PTR_SIZE, "0x%llX");
        } else {
            return SBNUM(x, MONGO_PTR_SIZE, "0x%lX");
        }
    }
    StringBuilderImpl& operator<<(bool val) {
        *_buf.grow(1) = val ? '1' : '0';
        return *this;
    }
    StringBuilderImpl& operator<<(char c) {
        _buf.grow(1)[0] = c;
        return *this;
    }
    StringBuilderImpl& operator<<(const char* str) {
        return *this << StringData(str);
    }
    StringBuilderImpl& operator<<(StringData str) {
        append(str);
        return *this;
    }
    StringBuilderImpl& operator<<(BSONType type) {
        append(typeName(type));
        return *this;
    }
    StringBuilderImpl& operator<<(ErrorCodes::Error code) {
        append(ErrorCodes::errorString(code));
        return *this;
    }

    template <typename T>
    StringBuilderImpl& operator<<(const boost::optional<T>& optional) {
        return optional ? *this << *optional : *this << "(None)";
    }

    /**
     * Fail to compile if passed an unevaluated function, rather than allow it to decay and invoke
     * the bool overload. This catches both passing std::hex (which isn't supported by this type)
     * and forgetting to add () when doing `stream << someFuntion`.
     */
    template <typename R, typename... Args>
    StringBuilderImpl& operator<<(R (*val)(Args...)) = delete;

    void appendDoubleNice(double x) {
        const int prev = _buf.l;
        const int maxSize = 32;
        char* start = _buf.grow(maxSize);
        int z = snprintf(start, maxSize, "%.16g", x);
        verify(z >= 0);
        verify(z < maxSize);
        _buf.l = prev + z;
        if (strchr(start, '.') == nullptr && strchr(start, 'E') == nullptr &&
            strchr(start, 'N') == nullptr) {
            write(".0", 2);
        }
    }

    void write(const char* buf, int len) {
        memcpy(_buf.grow(len), buf, len);
    }

    void append(StringData str) {
        str.copyTo(_buf.grow(str.size()), false);
    }

    void reset(int maxSize = 0) {
        _buf.reset(maxSize);
    }

    std::string str() const {
        return std::string(_buf.buf(), _buf.l);
    }

    /**
     * Returns a view of this string without copying.
     *
     * WARNING: the view expires when this StringBuilder is modified or destroyed.
     */
    StringData stringData() const {
        return StringData(_buf.buf(), _buf.l);
    }

    /** size of current std::string */
    int len() const {
        return _buf.l;
    }

private:
    template <typename T>
    StringBuilderImpl& appendIntegral(T val, int maxSize) {
        MONGO_STATIC_ASSERT(!std::is_same<T, char>());  // char shouldn't append as number.
        MONGO_STATIC_ASSERT(std::is_integral<T>());

        if (val < 0) {
            *this << '-';
            append(StringData(ItoA(0 - uint64_t(val))));  // Send the magnitude to ItoA.
        } else {
            append(StringData(ItoA(uint64_t(val))));
        }

        return *this;
    }

    template <typename T>
    StringBuilderImpl& SBNUM(T val, int maxSize, const char* macro) {
        int prev = _buf.l;
        int z = snprintf(_buf.grow(maxSize), maxSize, macro, (val));
        verify(z >= 0);
        verify(z < maxSize);
        _buf.l = prev + z;
        return *this;
    }

    Builder _buf;
};

using StringBuilder = StringBuilderImpl<BufBuilder>;
using StackStringBuilder = StringBuilderImpl<StackBufBuilderBase<StackSizeDefault>>;

extern template class StringBuilderImpl<BufBuilder>;
extern template class StringBuilderImpl<StackBufBuilderBase<StackSizeDefault>>;

}  // namespace mongo
