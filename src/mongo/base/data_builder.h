// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type.h"
#include "mongo/base/status.h"
#include "mongo/util/allocator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

[[MONGO_MOD_PUBLIC]];

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
        size_t size = other.size();
        _buf = std::move(other._buf);
        _capacity = other._capacity;
        char* start = _buf.get() + size;
        char* end = _buf.get() + _capacity;
        _unwrittenSpaceCursor = {start, end};

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

        auto status = _unwrittenSpaceCursor.writeNoThrow(value, offset);

        if (!status.isOK()) {
            reserve(_getSerializedSize(value));
            status = _unwrittenSpaceCursor.writeNoThrow(value, offset);
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
        auto status = _unwrittenSpaceCursor.writeAndAdvanceNoThrow(value);

        if (!status.isOK()) {
            reserve(_getSerializedSize(value));
            status = _unwrittenSpaceCursor.writeAndAdvanceNoThrow(value);
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
        DataType::store(value, nullptr, std::numeric_limits<std::size_t>::max(), &advance, 0)
            .transitional_ignore();

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
