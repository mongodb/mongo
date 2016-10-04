/*    Copyright 2015 MongoDB Inc.
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

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>

#include "mongo/base/data_range_cursor.h"
#include "mongo/util/allocator.h"

namespace mongo {

/**
 * DataBuilder provides a reallocing buffer underneath the DataRangeCursor API.
 * This allows consumers to write() or writeAndAdvance() without first ensuring
 * they have the correct amount of space pre-allocated.
 *
 * The underlying strategy is optimistic, specifically it blindly tries all
 * writes once. For any failure, we then call the store api with a null output
 * ptr, which returns what space would have been used. That amount is used to
 * guide growth in the buffer, after which we attempt the write again.
 */
class DataBuilder {
    /**
     * The dtor type used in the unique_ptr which holds the buffer
     */
    struct FreeBuf {
        void operator()(char* buf) {
            std::free(buf);
        }
    };

    static const std::size_t kInitialBufferSize = 64;

public:
    DataBuilder() = default;

    /**
     * Construct a DataBuilder with a specified initial capacity
     */
    explicit DataBuilder(std::size_t bytes) {
        if (bytes)
            resize(bytes);
    }

    DataBuilder(DataBuilder&& other) {
        *this = std::move(other);
    }

    DataBuilder& operator=(DataBuilder&& other) {
        _buf = std::move(other._buf);
        _capacity = other._capacity;
        _unwrittenSpaceCursor = {_buf.get(), _buf.get() + other.size()};

        other._capacity = 0;
        other._unwrittenSpaceCursor = {nullptr, nullptr};

        return *this;
    }

    /**
     * Write a value at an offset into the buffer.
     */
    template <typename T>
    Status write(const T& value, std::size_t offset = 0) {
        _ensureStorage();

        auto status = _unwrittenSpaceCursor.write(value, offset);

        if (!status.isOK()) {
            reserve(_getSerializedSize(value));
            status = _unwrittenSpaceCursor.write(value, offset);
        }

        return status;
    }

    /**
     * Write a value and advance to the byte past the last byte written.
     */
    template <typename T>
    Status writeAndAdvance(const T& value) {
        _ensureStorage();

        // TODO: We should offer:
        //
        // 1. A way to check if the type has a constant size
        // 2. A way to perform a runtime write which can fail with "too little
        //    size" without status generation
        auto status = _unwrittenSpaceCursor.writeAndAdvance(value);

        if (!status.isOK()) {
            reserve(_getSerializedSize(value));
            status = _unwrittenSpaceCursor.writeAndAdvance(value);
        }

        return status;
    }

    /**
     * Get a writable cursor that covers the range of the currently written
     * bytes
     */
    DataRangeCursor getCursor() {
        return {_buf.get(), _buf.get() + size()};
    }

    /**
     * Get a read-only cursor that covers the range of the currently written
     * bytes
     */
    ConstDataRangeCursor getCursor() const {
        return {_buf.get(), _buf.get() + size()};
    }

    /**
     * The size of the currently written region
     */
    std::size_t size() const {
        if (!_buf) {
            return 0;
        }

        return _capacity - _unwrittenSpaceCursor.length();
    }

    /**
     * The total size of the buffer, including reserved but not written bytes.
     */
    std::size_t capacity() const {
        return _capacity;
    }

    /**
     * Resize the buffer to exactly newSize bytes. This can shrink the range or
     * grow it.
     */
    void resize(std::size_t newSize) {
        if (newSize == _capacity)
            return;

        if (newSize == 0) {
            *this = DataBuilder{};
            return;
        }

        std::size_t oldSize = size();

        auto ptr = _buf.release();

        _buf.reset(static_cast<char*>(mongoRealloc(ptr, newSize)));

        _capacity = newSize;

        // If we downsized, truncate. If we upsized keep the old size
        _unwrittenSpaceCursor = {_buf.get() + std::min(oldSize, _capacity), _buf.get() + _capacity};
    }

    /**
     * Reserve needed bytes. If there are already enough bytes in the buffer,
     * it will not be changed. If there aren't enough bytes, we'll grow the
     * buffer to meet the requirement by expanding along a 1.5^n curve.
     */
    void reserve(std::size_t needed) {
        std::size_t oldSize = size();

        std::size_t newSize = _capacity ? _capacity : kInitialBufferSize;

        while ((newSize < oldSize) || (newSize - oldSize < needed)) {
            // growth factor of about 1.5

            newSize = ((newSize * 3) + 1) / 2;
        }

        invariant(newSize >= oldSize);

        resize(newSize);
    }

    /**
     * Clear the buffer. This retains the existing buffer, merely resetting the
     * internal data pointers.
     */
    void clear() {
        _unwrittenSpaceCursor = {_buf.get(), _buf.get() + _capacity};
    }

    /**
     * Release the buffer. After this the builder is left in the default
     * constructed state.
     */
    std::unique_ptr<char, FreeBuf> release() {
        auto buf = std::move(_buf);

        *this = DataBuilder{};

        return buf;
    }

private:
    /**
     * Returns the serialized size of a T. We verify this by using the
     * DataType::store invocation without an output pointer, which asks for the
     * number of bytes that would have been written.
     */
    template <typename T>
    std::size_t _getSerializedSize(const T& value) {
        std::size_t advance = 0;
        DataType::store(value, nullptr, std::numeric_limits<std::size_t>::max(), &advance, 0);

        return advance;
    }

    /**
     * If any writing methods are called on a default constructed or moved from
     * DataBuilder, we use this method to initialize the buffer.
     */
    void _ensureStorage() {
        if (!_buf) {
            resize(kInitialBufferSize);
        }
    }

    std::unique_ptr<char, FreeBuf> _buf;
    std::size_t _capacity = 0;
    DataRangeCursor _unwrittenSpaceCursor = {nullptr, nullptr};
};

}  // namespace mongo
