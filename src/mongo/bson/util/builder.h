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

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/static_assert.hpp>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/platform/bits.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"
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

/**
 * This is the maximum size size of a buffer needed for storing a BSON object in a response message.
 */
const int kOpMsgReplyBSONBufferMaxSize = BSONObjMaxUserSize + (64 * 1024);

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

    // The buffer holder size for 'SharedBufferAllocator' comes from 'SharedBuffer'
    static constexpr size_t kBuffHolderSize = SharedBuffer::kHolderSize;

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

    SharedBufferFragment finish(size_t sz) {
        return _fragmentBuilder.finish(sz);
    }

    size_t capacity() const {
        return _fragmentBuilder.capacity();
    }

    char* get() const {
        return _fragmentBuilder.get();
    }

    // SharedBufferFragmentAllocator does not allocate any extra amount of memory for the buffer
    // holder.
    static constexpr size_t kBuffHolderSize = 0;

private:
    SharedBufferFragmentBuilder& _fragmentBuilder;
};

class UniqueBufferAllocator {
    UniqueBufferAllocator(const UniqueBufferAllocator&) = delete;
    UniqueBufferAllocator& operator=(const UniqueBufferAllocator&) = delete;

public:
    UniqueBufferAllocator() = default;
    UniqueBufferAllocator(size_t sz) {
        if (sz > 0)
            malloc(sz);
    }

    // Allow moving but not copying. It would be an error for two UniqueBufferAllocators to use the
    // same underlying buffer.
    UniqueBufferAllocator(UniqueBufferAllocator&&) = default;
    UniqueBufferAllocator& operator=(UniqueBufferAllocator&&) = default;

    void malloc(size_t sz) {
        _buf = UniqueBuffer::allocate(sz);
    }

    void realloc(size_t sz) {
        _buf.realloc(sz);
    }

    void free() {
        _buf = {};
    }

    UniqueBuffer release() {
        return std::move(_buf);
    }

    size_t capacity() const {
        return _buf.capacity();
    }

    char* get() const {
        return _buf.get();
    }

    // The buffer holder size for 'UniqueBufferAllocator' comes from 'UniqueBuffer'
    static constexpr size_t kBuffHolderSize = UniqueBuffer::kHolderSize;

private:
    UniqueBuffer _buf;
};

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

    // StackAllocator does not allocate any extra amount of memory for the buffer holder.
    static constexpr size_t kBuffHolderSize = 0;

private:
    char _buf[SZ];
    size_t _capacity = SZ;

    void* _ptr = _buf;
};

template <class BufferAllocator>
class BasicBufBuilder {
public:
    template <typename... AllocatorArgs>
    BasicBufBuilder(AllocatorArgs&&... args)
        : _buf(std::forward<AllocatorArgs>(args)...),
          _nextByte(_buf.get()),
          _end(_nextByte + _buf.capacity()) {}

    void kill() {
        _buf.free();
    }

    void reset() {
        _nextByte = _buf.get();
        _end = _nextByte + _buf.capacity();
    }
    void reset(size_t maxSize) {
        if (maxSize && _buf.capacity() > maxSize) {
            _buf.free();
            _buf.malloc(maxSize);
        }
        reset();
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

    template <int...>
    requires(!std::is_same_v<int64_t, long long>) void appendNum(int64_t j) {
        appendNumImpl(j);
    }

    void appendNum(unsigned long long j) {
        appendNumImpl(j);
    }

    void appendNum(unsigned long int j) {
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

    /** Returns the length of data in the current buffer */
    int len() const {
        if (MONGO_unlikely(!_nextByte || !_end)) {
            return 0;
        }
        return _nextByte - _buf.get();
    }
    void setlen(int newLen) {
        _nextByte = _buf.get() + newLen;
    }
    /** Returns the capacity of the buffer */
    int capacity() const {
        return _buf.capacity();
    }

    /* returns the pre-grow write position */
    char* grow(int by) {
        if (MONGO_likely(by <= _end - _nextByte)) {
            char* oldNextByte = _nextByte;
            _nextByte += by;
            return oldNextByte;
        }
        return _growOutOfLineSlowPath(by);
    }

    /**
     * Reserve room for some number of bytes to be claimed at a later time via claimReservedBytes.
     */
    void reserveBytes(size_t bytes) {
        dassert(_nextByte && _end);
        if (MONGO_likely((_end - bytes) >= _nextByte)) {
            _end -= bytes;
            return;
        }

        _growOutOfLineSlowPath(bytes);

        // _growOutOfLineSlowPath adds to _nextByte to speed up the
        // common case of grow(). Now remove those bytes, and put them
        // after _end.
        _nextByte -= bytes;
        _end -= bytes;
    }

    /**
     * Claim an earlier reservation of some number of bytes. These bytes must already have been
     * reserved. Appends of up to this many bytes immediately following a claim are
     * guaranteed to succeed without a need to reallocate.
     */
    void claimReservedBytes(size_t bytes) {
        invariant(reservedBytes() >= bytes);
        _end += bytes;
    }

    /**
     * Replaces the buffer backing this BufBuilder with the passed in SharedBuffer.
     * Only legal to call when this builder is empty and when the SharedBuffer isn't shared.
     */
    template <int...>
    requires std::is_same_v<BufferAllocator, SharedBufferAllocator>
    void useSharedBuffer(SharedBuffer buf) {
        invariant(len() == 0);  // Can only do this while empty.
        invariant(reservedBytes() == 0);
        _buf = SharedBufferAllocator(std::move(buf));
        reset();
    }

protected:
    /**
     * Returns the reservedBytes in this buffer
     */
    size_t reservedBytes() const {
        if (MONGO_unlikely(!_nextByte || !_end)) {
            return 0;
        }
        return _buf.capacity() - (_end - _buf.get());
    }

    template <typename T>
    void appendNumImpl(T t) {
        // NOTE: For now, we assume that all things written
        // by a BufBuilder are intended for external use: either written to disk
        // or to the wire. Since all of our encoding formats are little endian,
        // we bake that assumption in here. This decision should be revisited soon.
        DataView(grow(sizeof(t))).write(tagLittleEndian(t));
    }

    /**
     * The "slow" portion of 'grow()', for when we actually need to go
     * to the underlying allocator for more memory. This function must
     * not be inlined.
     */
    MONGO_COMPILER_NOINLINE char* _growOutOfLineSlowPath(size_t by) {
        const size_t oldLen = len();
        const size_t oldReserved = reservedBytes();
        size_t minSize = oldLen + by + oldReserved;

        // Going beyond the maximum buffer size is not likely.
        if (MONGO_unlikely(minSize > BufferMaxSize)) {
            std::stringstream ss;
            ss << "BufBuilder attempted to grow() to " << minSize << " bytes, past the 64MB limit.";
            msgasserted(13548, ss.str().c_str());
        }

        // We add 'BufferAllocator::kBuffHolderSize' to the requested reallocation size, as it will
        // be required later in '_buf.realloc'.
        minSize += BufferAllocator::kBuffHolderSize;

        // We find the next power of two greater than the requested size, as it's
        // commonly more friendly with the underlying (system) memory allocators.
        size_t reallocSize = 1ull << (64 - countLeadingZeros64(minSize - 1));

        // Even though allocating some memory between BSONObjMaxUserSize and
        // kOpMsgReplyBSONBufferMaxSize is common, but compared to very many small
        // allocation done during the execution, it counts as an unlikely scenario. Still,
        // it has a significant implact on the memory efficiency of the system.
        if (MONGO_unlikely(
                (minSize >= BSONObjMaxUserSize && minSize <= kOpMsgReplyBSONBufferMaxSize) ||
                reallocSize == BSONObjMaxUserSize)) {
            // BSONObjMaxUserSize and kOpMsgReplyBSONBufferMaxSize are two common sizes that we
            // might allocate memory for. If the requested size is anywhere between
            // BSONObjMaxUserSize and kOpMsgReplyBSONBufferMaxSize, we allocate
            // kOpMsgReplyBSONBufferMaxSize bytes to avoid potential reallocation due to the
            // additional header objects that wrap the maximum size of a BSON.
            reallocSize = kOpMsgReplyBSONBufferMaxSize;
        } else if (MONGO_unlikely(reallocSize < 64)) {
            // The minimum allocation is 64 bytes.
            reallocSize = 64;
        } else if (MONGO_unlikely(minSize > BufferMaxSize)) {
            // If adding 'kBuffHolderSize' to 'minSize' pushes it beyond 'BufferMaxSize', then we'll
            // allocate enough memory according to the 'BufferMaxSize'.
            reallocSize = BufferMaxSize + BufferAllocator::kBuffHolderSize;
        }

        // As we've added 'BufferAllocator::kBuffHolderSize' to 'minSize' in the beginning, we
        // subtract it here from 'reallocSize' to account for the same amount that will be added
        // later in '_buf.realloc'. Without this, we will end up allocating an amount of memory,
        // which is not a power of two and defeats the purpose of the above logic to find the
        // next power of two for being friendly to the system memory allocators and avoid memory
        // fragmentation.
        _buf.realloc(reallocSize - BufferAllocator::kBuffHolderSize);
        _nextByte = _buf.get() + oldLen + by;
        _end = _buf.get() + _buf.capacity() - oldReserved;

        invariant(_nextByte >= _buf.get());
        invariant(_end >= _nextByte);
        invariant(_buf.get() + _buf.capacity() >= _end);

        return _buf.get() + oldLen;
    }

    BufferAllocator _buf;
    char* _nextByte;
    char* _end;

    template <class Builder>
    friend class StringBuilderImpl;
};

// The following extern template declaration must follow
// BasicBufBuilder and come before its instantiation as a base class
// for BufBuilder. Do not remove or re-order these lines w.r.t those
// types without being sure that you are not undoing the advantages of
// the extern template declaration.
extern template class BasicBufBuilder<SharedBufferAllocator>;

class BufBuilder : public BasicBufBuilder<SharedBufferAllocator> {
public:
    static constexpr size_t kDefaultInitSizeBytes = 512;
    BufBuilder(size_t initsize = kDefaultInitSizeBytes) : BasicBufBuilder(initsize) {}

    /**
     * Assume ownership of the buffer.
     * Note: There should not be any other method calls on this object after a call to 'release'.
     */
    SharedBuffer release() {
        return _buf.release();
    }
};

// The following extern template declaration must follow
// BasicBufBuilder and come before its instantiation as a base class
// for PooledFragmentBuilder. Do not remove or re-order these lines
// w.r.t those types without being sure that you are not undoing the
// advantages of the extern template declaration.
extern template class BasicBufBuilder<SharedBufferFragmentAllocator>;

class PooledFragmentBuilder : public BasicBufBuilder<SharedBufferFragmentAllocator> {
public:
    PooledFragmentBuilder(SharedBufferFragmentBuilder& fragmentBuilder)
        : BasicBufBuilder(fragmentBuilder.start(0)) {}

    SharedBufferFragment done() {
        return _buf.finish(len());
    }
};
MONGO_STATIC_ASSERT(std::is_move_constructible_v<BufBuilder>);

// The following extern template declaration must follow
// BasicBufBuilder and come before its instantiation as a base class
// for UniqueBufBuilder. Do not remove or re-order these lines w.r.t
// those types without being sure that you are not undoing the
// advantages of the extern template declaration.
extern template class BasicBufBuilder<UniqueBufferAllocator>;

class UniqueBufBuilder : public BasicBufBuilder<UniqueBufferAllocator> {
public:
    static constexpr size_t kDefaultInitSizeBytes = 512;
    UniqueBufBuilder(size_t initsize = kDefaultInitSizeBytes) : BasicBufBuilder(initsize) {}

    /**
     * Assume ownership of the buffer.
     * Note: There should not be any other method calls on this object after a call to 'release'.
     */
    UniqueBuffer release() {
        return _buf.release();
    }
};

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
MONGO_STATIC_ASSERT(!std::is_move_constructible<StackBufBuilder>::value);

// This extern template declaration must follow the declaration of
// StackBufBuilderBase, and must come before the extern template
// declarations of StringBuilder below. Do not remove or re-order
// these lines w.r.t those StackBufBuilderBase or the other extern
// template declarations without being sure that you are not undoing
// the advantages of the extern template declaration.
extern template class StackBufBuilderBase<StackSizeDefault>;

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
        const int prev = _buf.len();
        const int maxSize = 32;
        char* start = _buf.grow(maxSize);
        int z = snprintf(start, maxSize, "%.16g", x);
        MONGO_verify(z >= 0);
        MONGO_verify(z < maxSize);
        _buf.setlen(prev + z);
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
        return std::string(_buf.buf(), _buf.len());
    }

    /**
     * stringView() returns a view of this string without copying.
     *
     * WARNING: The view is invalidated when this StringBuilder is modified or destroyed.
     */
    std::string_view stringView() const {
        return std::string_view(_buf.buf(), _buf.len());
    }

    /**
     * stringData() returns a view of this string without copying.
     *
     * WARNING: The view is invalidated when this StringBuilder is modified or destroyed.
     */
    StringData stringData() const {
        return StringData(_buf.buf(), _buf.len());
    }

    /** size of current std::string */
    int len() const {
        return _buf.len();
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
        int prev = _buf.len();
        int z = snprintf(_buf.grow(maxSize), maxSize, macro, (val));
        MONGO_verify(z >= 0);
        MONGO_verify(z < maxSize);
        _buf.setlen(prev + z);
        return *this;
    }

    Builder _buf;
};


// The following extern template declaration must follow the
// declaration of StringBuilderImpl and StackBufBuilderBase along with
// the extern template delarations for that type. Do not remove or
// re-order these lines w.r.t those types without being sure that you
// are not undoing the advantages of the extern template declaration.
extern template class StringBuilderImpl<BufBuilder>;
extern template class StringBuilderImpl<StackBufBuilderBase<StackSizeDefault>>;

}  // namespace mongo
