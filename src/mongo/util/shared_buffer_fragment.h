// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer.h"

#include <functional>
#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Immutable view of a fragment of a ref-counted buffer.
 * Shares reference count with the underlying buffer.
 */
class SharedBufferFragment {
public:
    SharedBufferFragment() : _offset(0), _size(0) {}
    explicit SharedBufferFragment(SharedBuffer buffer, size_t size)
        : SharedBufferFragment(std::move(buffer), 0, size) {}
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
 * underlying buffer or multiple buffers. Can only build one fragment at a time.
 *
 * Warning: This builder will hold references to all allocated buffers and will not release them
 * until freeUnused() is called. Memory is not reused. This means that failing to call this function
 * will result in an unbounded amount of memory usage for the lifetime of the builder. Even after
 * this builder is destructed, SharedBufferFragments can prevent memory from being freed.
 */
class SharedBufferFragmentBuilder {
public:
    static constexpr size_t kDefaultMaxBlockSize = 1024 * 1024;  // 1MB
    using GrowStrategy = std::function<size_t(size_t)>;
    SharedBufferFragmentBuilder(
        size_t blockSize, GrowStrategy growStrategy = DoubleGrowStrategy(kDefaultMaxBlockSize))
        : _offset(0), _blockSize(blockSize), _growStrategy(growStrategy) {}

    SharedBufferFragmentBuilder(SharedBufferFragmentBuilder&& other) = default;
    SharedBufferFragmentBuilder& operator=(SharedBufferFragmentBuilder&& other) = default;

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
    SharedBufferFragmentBuilder& start(size_t initialSize) {
        invariant(!_inUse);
        if (!_buffer.isShared()) {
            // Since there are no fragments sharing with this buffer, we can reset the offset to 0
            // to reuse unused space.
            _offset = 0;
        }

        if (_buffer.capacity() < (_offset + initialSize)) {
            // If the capacity is 0, this is our initial allocation and we should not use the grow
            // strategy.
            if (_buffer.capacity() > 0)
                _blockSize = _growStrategy(_blockSize);

            size_t allocSize = std::max(_blockSize, initialSize);
            _buffer = _alloc(std::move(_buffer), allocSize);
            _offset = 0;
        }
        _inUse = true;
        return *this;
    }

    // Grows the currently building memory fragment so it will fit at least 'size' bytes.
    // May only be called when building a fragment
    void grow(size_t size) {
        invariant(_inUse);
        auto currentCapacity = capacity();
        if (currentCapacity < size) {
            // If the capacity is 0, this is our initial allocation and we should not use the grow
            // strategy.
            if (currentCapacity > 0) {
                _blockSize = _growStrategy(_blockSize);
            }
            size_t allocSize = std::max(_blockSize, size);

            if (_buffer) {
                _buffer = _realloc(std::move(_buffer), _offset, currentCapacity, allocSize);
            } else {
                _buffer = _alloc(std::move(_buffer), allocSize);
            }
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
        _totalFragmentBytesUsed += totalSize;
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

    // Returns the memory used by all allocated buffers that are being tracked. This returns
    // instantaneous memory usage of all memory currently allocated, including any memory that is
    // not being used due to fragmentation in the SharedBufferFragmentBuilder.
    size_t memUsage() const {
        return _memUsage;
    }

    // Returns the cumulative memory used by all buffer memory fragments. Does not reset. This
    // excludes any unused memory due to fragmentation in the SharedBufferFragmentBuilder.
    size_t totalFragmentBytesUsed() const {
        return _totalFragmentBytesUsed;
    }

    // Frees all unreferenced buffers except for the most recently allocated one. The caller must
    // ensure that no references to any shared buffers remain to maintain useful memory usage
    // information.
    void freeUnused();

private:
    SharedBuffer _alloc(SharedBuffer&& existing, size_t allocSize) {
        return _realloc(std::move(existing), 0, 0, allocSize);
    }

    SharedBuffer _realloc(SharedBuffer&& existing,
                          size_t offset,
                          size_t existingSize,
                          size_t newSize);

    // The current working buffer of this builder.
    SharedBuffer _buffer;
    ptrdiff_t _offset;
    size_t _blockSize;
    GrowStrategy _growStrategy;
    bool _inUse{false};

    // This is a list of old buffers that may still be in use by other fragments. Counts towards
    // total memory usage and buffers must be freed by calling using freeUnused() when buffers are
    // no longer needed.
    std::vector<SharedBuffer> _activeBuffers;
    size_t _memUsage = 0;
    size_t _totalFragmentBytesUsed = 0;
};

}  // namespace mongo
