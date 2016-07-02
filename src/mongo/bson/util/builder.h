/* builder.h */

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

#include <cfloat>
#include <cstdint>
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <string>


#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/inline_decls.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer.h"

namespace mongo {

/* Note the limit here is rather arbitrary and is simply a standard. generally the code works
   with any object that fits in ram.

   Also note that the server has some basic checks to enforce this limit but those checks are not
   exhaustive for example need to check for size too big after
     update $push (append) operation
     various db.eval() type operations
*/
const int BSONObjMaxUserSize = 16 * 1024 * 1024;

/*
   Sometimes we need objects slightly larger - an object in the replication local.oplog
   is slightly larger than a user object for example.
*/
const int BSONObjMaxInternalSize = BSONObjMaxUserSize + (16 * 1024);

const int BufferMaxSize = 64 * 1024 * 1024;

template <typename Allocator>
class StringBuilderImpl;

class SharedBufferAllocator {
    MONGO_DISALLOW_COPYING(SharedBufferAllocator);

public:
    SharedBufferAllocator() = default;

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

    char* get() const {
        return _buf.get();
    }

private:
    SharedBuffer _buf;
};

class StackAllocator {
    MONGO_DISALLOW_COPYING(StackAllocator);

public:
    StackAllocator() = default;

    enum { SZ = 512 };
    void malloc(size_t sz) {
        if (sz > SZ)
            _ptr = mongoMalloc(sz);
    }
    void realloc(size_t sz) {
        if (_ptr == _buf) {
            if (sz > SZ) {
                _ptr = mongoMalloc(sz);
                memcpy(_ptr, _buf, SZ);
            }
        } else {
            _ptr = mongoRealloc(_ptr, sz);
        }
    }
    void free() {
        if (_ptr != _buf)
            ::free(_ptr);
        _ptr = _buf;
    }

    // Not supported on this allocator.
    void release() = delete;

    char* get() const {
        return static_cast<char*>(_ptr);
    }

private:
    char _buf[SZ];
    void* _ptr = _buf;
};

template <class BufferAllocator>
class _BufBuilder {
    MONGO_DISALLOW_COPYING(_BufBuilder);

public:
    _BufBuilder(int initsize = 512) : size(initsize) {
        if (size > 0) {
            _buf.malloc(size);
        }
        l = 0;
        reservedBytes = 0;
    }
    ~_BufBuilder() {
        kill();
    }

    void kill() {
        _buf.free();
    }

    void reset() {
        l = 0;
        reservedBytes = 0;
    }
    void reset(int maxSize) {
        l = 0;
        reservedBytes = 0;
        if (maxSize && size > maxSize) {
            _buf.free();
            _buf.malloc(maxSize);
            size = maxSize;
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

    /* assume ownership of the buffer */
    SharedBuffer release() {
        return _buf.release();
    }

    void appendUChar(unsigned char j) {
        static_assert(CHAR_BIT == 8, "CHAR_BIT == 8");
        appendNumImpl(j);
    }
    void appendChar(char j) {
        appendNumImpl(j);
    }
    void appendNum(char j) {
        appendNumImpl(j);
    }
    void appendNum(short j) {
        static_assert(sizeof(short) == 2, "sizeof(short) == 2");
        appendNumImpl(j);
    }
    void appendNum(int j) {
        static_assert(sizeof(int) == 4, "sizeof(int) == 4");
        appendNumImpl(j);
    }
    void appendNum(unsigned j) {
        appendNumImpl(j);
    }

    // Bool does not have a well defined encoding.
    void appendNum(bool j) = delete;

    void appendNum(double j) {
        static_assert(sizeof(double) == 8, "sizeof(double) == 8");
        appendNumImpl(j);
    }
    void appendNum(long long j) {
        static_assert(sizeof(long long) == 8, "sizeof(long long) == 8");
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

    template <typename Int64_t,
              typename = stdx::enable_if_t<std::is_same<Int64_t, int64_t>::value &&
                                           !std::is_same<int64_t, long long>::value>>
    void appendNum(Int64_t j) {
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
        return size;
    }

    /* returns the pre-grow write position */
    inline char* grow(int by) {
        int oldlen = l;
        int newLen = l + by;
        int minSize = newLen + reservedBytes;
        if (minSize > size) {
            grow_reallocate(minSize);
        }
        l = newLen;
        return _buf.get() + oldlen;
    }

    /**
     * Reserve room for some number of bytes to be claimed at a later time.
     */
    void reserveBytes(int bytes) {
        int minSize = l + reservedBytes + bytes;
        if (minSize > size)
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

private:
    template <typename T>
    void appendNumImpl(T t) {
        // NOTE: For now, we assume that all things written
        // by a BufBuilder are intended for external use: either written to disk
        // or to the wire. Since all of our encoding formats are little endian,
        // we bake that assumption in here. This decision should be revisited soon.
        DataView(grow(sizeof(t))).write(tagLittleEndian(t));
    }
    /* "slow" portion of 'grow()'  */
    void NOINLINE_DECL grow_reallocate(int minSize) {
        int a = 64;
        while (a < minSize)
            a = a * 2;

        if (a > BufferMaxSize) {
            std::stringstream ss;
            ss << "BufBuilder attempted to grow() to " << a << " bytes, past the 64MB limit.";
            msgasserted(13548, ss.str().c_str());
        }
        _buf.realloc(a);
        size = a;
    }

    BufferAllocator _buf;
    int l;
    int size;
    int reservedBytes;  // eagerly grow_reallocate to keep this many bytes of spare room.

    friend class StringBuilderImpl<BufferAllocator>;
};

typedef _BufBuilder<SharedBufferAllocator> BufBuilder;

/** The StackBufBuilder builds smaller datasets on the stack instead of using malloc.
      this can be significantly faster for small bufs.  However, you can not release() the
      buffer with StackBufBuilder.
    While designed to be a variable on the stack, if you were to dynamically allocate one,
      nothing bad would happen.  In fact in some circumstances this might make sense, say,
      embedded in some other object.
*/
class StackBufBuilder : public _BufBuilder<StackAllocator> {
public:
    StackBufBuilder() : _BufBuilder<StackAllocator>(StackAllocator::SZ) {}
    void release() = delete;  // not allowed. not implemented.
};

/** std::stringstream deals with locale so this is a lot faster than std::stringstream for UTF8 */
template <typename Allocator>
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
        return SBNUM(x, MONGO_S32_SIZE, "%d");
    }
    StringBuilderImpl& operator<<(unsigned x) {
        return SBNUM(x, MONGO_U32_SIZE, "%u");
    }
    StringBuilderImpl& operator<<(long x) {
        return SBNUM(x, MONGO_S64_SIZE, "%ld");
    }
    StringBuilderImpl& operator<<(unsigned long x) {
        return SBNUM(x, MONGO_U64_SIZE, "%lu");
    }
    StringBuilderImpl& operator<<(long long x) {
        return SBNUM(x, MONGO_S64_SIZE, "%lld");
    }
    StringBuilderImpl& operator<<(unsigned long long x) {
        return SBNUM(x, MONGO_U64_SIZE, "%llu");
    }
    StringBuilderImpl& operator<<(short x) {
        return SBNUM(x, MONGO_S16_SIZE, "%hd");
    }
    StringBuilderImpl& operator<<(const void* x) {
        if (sizeof(x) == 8) {
            return SBNUM(x, MONGO_PTR_SIZE, "0x%llX");
        } else {
            return SBNUM(x, MONGO_PTR_SIZE, "0x%lX");
        }
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

    void appendDoubleNice(double x) {
        const int prev = _buf.l;
        const int maxSize = 32;
        char* start = _buf.grow(maxSize);
        int z = snprintf(start, maxSize, "%.16g", x);
        verify(z >= 0);
        verify(z < maxSize);
        _buf.l = prev + z;
        if (strchr(start, '.') == 0 && strchr(start, 'E') == 0 && strchr(start, 'N') == 0) {
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

    /** size of current std::string */
    int len() const {
        return _buf.l;
    }

private:
    _BufBuilder<Allocator> _buf;

    // non-copyable, non-assignable
    StringBuilderImpl(const StringBuilderImpl&);
    StringBuilderImpl& operator=(const StringBuilderImpl&);

    template <typename T>
    StringBuilderImpl& SBNUM(T val, int maxSize, const char* macro) {
        int prev = _buf.l;
        int z = snprintf(_buf.grow(maxSize), maxSize, macro, (val));
        verify(z >= 0);
        verify(z < maxSize);
        _buf.l = prev + z;
        return *this;
    }
};

typedef StringBuilderImpl<SharedBufferAllocator> StringBuilder;
typedef StringBuilderImpl<StackAllocator> StackStringBuilder;
}  // namespace mongo
