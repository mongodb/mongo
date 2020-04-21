/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/util/shared_buffer.h"

#include <functional>

namespace mongo {

/**
 * Immutable view of a fragment of a ref-counted buffer.
 * Shares reference count with the underlying buffer.
 */
class SharedBufferFragment {
public:
    SharedBufferFragment() : _offset(0), _size(0) {}
    explicit SharedBufferFragment(SharedBuffer buffer, size_t size)
        : _buffer(std::move(buffer)), _offset(0), _size(size) {}
    explicit SharedBufferFragment(SharedBuffer buffer, ptrdiff_t offset, size_t size)
        : _buffer(std::move(buffer)), _offset(offset), _size(size) {}

    void swap(SharedBufferFragment& other) {
        _buffer.swap(other._buffer);
        std::swap(_offset, other._offset);
        std::swap(_size, other._size);
    }

    const char* get() const {
        return _buffer.get() + _offset;
    }

    size_t size() const {
        return _size;
    }

    explicit operator bool() const {
        return (bool)_buffer;
    }

    /**
     * Returns true if this object has exclusive access to the underlying buffer.
     * (That is, reference count == 1).
     */
    bool isShared() const {
        return _buffer.isShared();
    }

    /**
     * Returns the allocation size of the underlying buffer.
     */
    size_t underlyingCapacity() const {
        return _buffer.capacity();
    }

private:
    SharedBuffer _buffer;
    ptrdiff_t _offset;
    size_t _size;
};


/**
 * Builder of SharedBufferFragment where multiple fragments are using different parts of the same
 * underlying buffer. Can only build one fragment at a time
 */
class SharedBufferFragmentBuilder {
public:
    static constexpr size_t kDefaultMaxBlockSize = 1024 * 1024;  // 1MB
    using GrowStrategy = std::function<size_t(size_t)>;
    SharedBufferFragmentBuilder(
        size_t blockSize, GrowStrategy growStrategy = DoubleGrowStrategy(kDefaultMaxBlockSize))
        : _offset(0), _blockSize(blockSize), _growStrategy(growStrategy) {}

    struct ConstantGrowStrategy {
        size_t operator()(size_t current) const {
            return current;
        }
    };

    struct DoubleGrowStrategy {
        DoubleGrowStrategy(size_t maxBlockSize) : _maxBlockSize(maxBlockSize) {}
        size_t operator()(size_t current) const {
            return std::min(current * 2, _maxBlockSize);
        }

    private:
        size_t _maxBlockSize;
    };

    // Starts building a memory fragment with at least 'initialSize' capacity.
    // May only be called if we are not currently building a fragment
    void start(size_t initialSize) {
        invariant(!_inUse);
        if (_buffer.capacity() < (_offset + initialSize)) {
            // If capacity is 0, then this is our initial allocation and we should not use the grow
            // strategy
            if (_buffer.capacity() > 0)
                _blockSize = _growStrategy(_blockSize);
            size_t allocSize = std::max(_blockSize, initialSize);
            _buffer = SharedBuffer::allocate(allocSize);
            _offset = 0;
        }
        _inUse = true;
    }

    // Grows the currently building memory fragment so it will fit at least 'size' bytes.
    // May only be called when building a fragment
    void grow(size_t size) {
        invariant(_inUse);
        auto currentCapacity = capacity();
        if (currentCapacity < size) {
            _blockSize = _growStrategy(_blockSize);
            size_t allocSize = std::max(_blockSize, size);

            // If nothing else is using the internal buffer it would be safe to use realloc. But as
            // this potentially is a large buffer realloc would need copy all of it as it doesn't
            // know how much is actually used. So we create a new buffer in all cases and reset the
            // offset to 0. We only need to copy the memory of the fragment we are currently
            // building.
            auto newBuffer = SharedBuffer::allocate(allocSize);
            if (_buffer)
                memcpy(newBuffer.get(), _buffer.get() + _offset, currentCapacity);
            _buffer = std::move(newBuffer);
            _offset = 0;
        }
    }

    // Finishes building a memory fragment. 'totalSize' should indicate total of bytes used.
    // Returns a reference counted memory fragment
    // May only be called when building a fragment and will put the builder back into a 'not
    // building' state.
    SharedBufferFragment finish(size_t totalSize) {
        invariant(_inUse);
        SharedBufferFragment fragment(_buffer, _offset, totalSize);
        _offset += totalSize;
        _inUse = false;
        return fragment;
    }

    // Discards the memory fragment currently building and puts the builder back into a 'not
    // building' state. May only be called when building a fragment
    void discard() {
        invariant(_inUse);
        _inUse = false;
    }

    // Returns the available capacity that may be used for building a memory fragment.
    // If more capacity is needed the user needs to call grow()
    size_t capacity() const {
        return _buffer.capacity() - _offset;
    }

    // Returns the beginning of the memory fragment we are currently building
    // May only be called when building a fragment
    char* get() const {
        invariant(_inUse);
        return _buffer.get() + _offset;
    }

    // Returns whether or not a memory fragment is currently being built.
    bool building() const {
        return _inUse;
    }

private:
    SharedBuffer _buffer;
    ptrdiff_t _offset;
    size_t _blockSize;
    GrowStrategy _growStrategy;
    bool _inUse{false};
};


}  // namespace mongo
