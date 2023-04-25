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
 * Parse a memory region into usable pieces.
 */

#include <utility>

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_terminated.h"
#include "mongo/bson/util/builder.h"
#include "mongo/platform/strnlen.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/** helper to read and parse a block of memory
    methods throw the eof exception if the operation would pass the end of the
    buffer with which we are working.
*/
class BufReader {
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

    /** return current position pointer, and advance by len */
    const void* skip(unsigned len) {
        ConstDataRangeCursor cdrc(_pos, _end);
        cdrc.advance(len);
        return std::exchange(_pos, cdrc.data());
    }

    /// reads a NUL terminated string
    StringData readCStr() {
        auto range = read<Terminated<'\0', ConstDataRange>>().value;

        return StringData(range.data(), range.length());
    }

    void readStr(std::string& s) {
        s = readCStr().toString();
    }

    /**
     * Return a view of the next len bytes and advance by len.
     */
    StringData readBytes(size_t len) {
        // Note: the call to skip() includes a check that at least 'len' bytes remain in the buffer.
        return StringData(reinterpret_cast<const char*>(skip(len)), len);
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
