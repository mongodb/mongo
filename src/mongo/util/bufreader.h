// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * Parse a memory region into usable pieces.
 */

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_terminated.h"
#include "mongo/bson/util/builder.h"
#include "mongo/platform/strnlen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <utility>

namespace mongo {

/** helper to read and parse a block of memory
    methods throw the eof exception if the operation would pass the end of the
    buffer with which we are working.
*/
class [[MONGO_MOD_PUBLIC]] BufReader {
    BufReader(const BufReader&) = delete;
    BufReader& operator=(const BufReader&) = delete;

public:
    BufReader(const void* p, unsigned len)
        : _start(reinterpret_cast<const char*>(p)), _pos(_start), _end(_start + len) {}

    bool atEof() const {
        return _pos == _end;
    }

    /** read in the object specified, and advance buffer pointer */
    template <typename T>
    requires(isEndiannessSpecified<T>())
    void read(T& t) {
        ConstDataRangeCursor cdrc(_pos, _end);
        cdrc.readAndAdvance(&t);
        _pos = cdrc.data();
    }

    /** read in and return an object of the specified type, and advance buffer pointer */
    template <typename T>
    T read() {
        T out{};
        read(out);
        return out;
    }

    /** read in the object specified, but do not advance buffer pointer */
    template <typename T>
    requires(isEndiannessSpecified<T>())
    void peek(T& t) const {
        ConstDataRange(_pos, _end).readInto(&t);
    }

    /** read in and return an object of the specified type, but do not advance buffer pointer */
    template <typename T>
    T peek() const {
        T out{};
        peek(out);
        return out;
    }

    /** return current offset into buffer */
    unsigned offset() const {
        return _pos - _start;
    }

    /** return remaining bytes */
    unsigned remaining() const {
        return _end - _pos;
    }

    /** back up by nbytes */
    void rewind(unsigned nbytes) {
        _pos = _pos - nbytes;
        invariant(_pos >= _start);
    }

    /** back up to beginging of buffer */
    void rewindToStart() {
        _pos = _start;
    }

    /** return current position pointer, and advance by len */
    const void* skip(unsigned len) {
        ConstDataRangeCursor cdrc(_pos, _end);
        cdrc.advance(len);
        return std::exchange(_pos, cdrc.data());
    }

    /// reads a NUL terminated string
    std::string_view readCStr() {
        auto range = read<Terminated<'\0', ConstDataRange>>().value;

        return std::string_view(range.data(), range.length());
    }

    void readStr(std::string& s) {
        s = std::string{readCStr()};
    }

    /**
     * Return a view of the next len bytes and advance by len.
     */
    std::string_view readBytes(size_t len) {
        // Note: the call to skip() includes a check that at least 'len' bytes remain in the buffer.
        return std::string_view(reinterpret_cast<const char*>(skip(len)), len);
    }

    const void* pos() {
        return _pos;
    }
    const void* start() {
        return _start;
    }

private:
    const char* _start;
    const char* _pos;
    const char* _end;
};
}  // namespace mongo
