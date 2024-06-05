/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <list>
#include <memory>
#include <new>
#include <ratio>
#include <utility>
#include <vector>

#include "mongo/platform/compiler.h"

/*
 * This Resource system is like `std::pmr::memory_resource`, but not polymorphic.
 * This makes it much faster in benchmarks.
 *
 * Also works around the missing `<memory_resource>` on our old xcode.
 */
namespace mongo::inline_memory {

class NewDeleteResource {
public:
    void* allocate(size_t sz, size_t al = alignof(std::max_align_t)) {
        return ::operator new (sz, std::align_val_t{al});
    }

    void deallocate(void* p, size_t sz, size_t al) {
        ::operator delete (p, sz, std::align_val_t{al});
    }

    /** Stateless, so all instances are equal to each other. */
    bool operator==(const NewDeleteResource& o) const {
        return true;
    }
};

/**
 * Gradually consumes a single buffer. When it's exhausted,
 * a chunk is allocated from an upstream resource and used to satisfy
 * the allocation. The chunk size grows exponentially.
 */
template <typename Upstream = NewDeleteResource>
class MonotonicBufferResource {
public:
    using ChunkGrowth = std::ratio<3, 2>;
    static constexpr size_t initialChunkSize = 128 * sizeof(void*);

    MonotonicBufferResource(void* buf, size_t sz, Upstream upstream = {})
        : _upstream{std::move(upstream)}, _buf{buf}, _bufSize{sz} {}

    ~MonotonicBufferResource() {
        while (_chunks) {
            auto ch = std::exchange(_chunks, _chunks->next);
            _upstream.deallocate(ch->buf, ch->sz, alignof(std::max_align_t));
            _upstream.deallocate(ch, sizeof(Chunk), alignof(Chunk));
        }
    }

    void* allocate(size_t sz, size_t al = alignof(std::max_align_t)) {
        auto p = std::align(al, sz, _free, _freeSize);
        if (MONGO_unlikely(!p)) {
            _newChunk(sz, al);
            p = _free;
        }
        _free = static_cast<std::byte*>(_free) + sz;
        _freeSize -= sz;
        return p;
    }

    void deallocate(void* p, size_t sz, size_t al) {}

    /** Has state, so only equal to itself. */
    bool operator==(const MonotonicBufferResource& o) const {
        return this == &o;
    }

    friend std::ostream& operator<<(std::ostream& os, const MonotonicBufferResource& o) {
        return os << "MonotonicBufferResource{_buf=" << o._buf << ", _bufSize=" << o._bufSize
                  << ", _free=" << o._free << ", _freeSize=" << o._freeSize << "}";
    }

private:
    struct Chunk {
        Chunk* next;
        void* buf;
        size_t sz;
    };

    MONGO_COMPILER_NOINLINE MONGO_COMPILER_COLD_FUNCTION void _newChunk(size_t sz, size_t al) {
        auto newChunk = static_cast<Chunk*>(_upstream.allocate(sizeof(Chunk), alignof(Chunk)));
        try {
            auto chunkSize = std::max(_nextBufSize, sz);
            newChunk->buf = _upstream.allocate(chunkSize, alignof(std::max_align_t));
            newChunk->sz = chunkSize;
        } catch (...) {
            _upstream.deallocate(newChunk, sizeof(Chunk), alignof(Chunk));
            throw;
        }
        newChunk->next = std::exchange(_chunks, newChunk);
        _free = newChunk->buf;
        _freeSize = newChunk->sz;
        _nextBufSize = newChunk->sz * ChunkGrowth::num / ChunkGrowth::den;
    }

    Chunk* _chunks = nullptr;
    size_t _nextBufSize = initialChunkSize;

    MONGO_COMPILER_NO_UNIQUE_ADDRESS Upstream _upstream;
    void* const _buf;
    const size_t _bufSize;
    void* _free = _buf;
    size_t _freeSize = _bufSize;
};

/** Refers to another unowned Resource by pointer. */
template <typename Resource>
class ExternalResource {
public:
    explicit ExternalResource(Resource* ext) : _ext{ext} {}

    ExternalResource(const ExternalResource&) = default;
    ExternalResource& operator=(const ExternalResource&) = default;

    void* allocate(size_t sz, size_t al = alignof(std::max_align_t)) {
        return _ext->allocate(sz, al);
    }

    void deallocate(void* p, size_t sz, size_t al) {
        return _ext->deallocate(p, sz, al);
    }

    bool operator==(const ExternalResource& o) const {
        return _ext == o._ext;
    }

private:
    Resource* _ext;
};

/**
 * A standard Allocator that allocates from a "Resource". `Resource` are
 * stackable allocation layers conforming to requirements that are met by the
 * Resource classes in this file, but there can be others.
 */
template <typename T, typename Resource>
class ResourceAllocator {
public:
    using value_type = T;

    explicit ResourceAllocator(Resource resource) : _resource{std::move(resource)} {}

    /** Convertible from my own rebind instances. */
    template <typename U>
    explicit(false) ResourceAllocator(const ResourceAllocator<U, Resource>& o)
        : _resource{o.resource()} {}

    [[nodiscard]] value_type* allocate(size_t n) {
        return static_cast<value_type*>(
            _resource.allocate(n * sizeof(value_type), alignof(value_type)));
    }

    void deallocate(value_type* p, size_t n) {
        _resource.deallocate(p, n * sizeof(value_type), alignof(T));
    }

    const Resource& resource() const {
        return _resource;
    }

    Resource& resource() {
        return _resource;
    }

    /** Equal to any allocator that's backed by an equal resource. */
    template <typename U>
    bool operator==(const ResourceAllocator<U, Resource>& b) const {
        return resource() == b.resource();
    }

private:
    MONGO_COMPILER_NO_UNIQUE_ADDRESS Resource _resource;
};

/** Provides allocators all backed by the same inline storage. */
template <size_t bytes, size_t alignment>
class MemoryBuffer {
public:
    template <typename T>
    using AllocatorType = ResourceAllocator<T, ExternalResource<MonotonicBufferResource<>>>;

    MemoryBuffer() = default;

    MemoryBuffer(const MemoryBuffer&) = delete;
    MemoryBuffer& operator=(const MemoryBuffer&) = delete;

    template <typename T>
    AllocatorType<T> makeAllocator() {
        return AllocatorType<T>{ExternalResource{&_monotonic}};
    }

private:
    alignas(alignment) std::array<std::byte, bytes> _buf;
    MonotonicBufferResource<> _monotonic{_buf.data(), _buf.size()};
};

namespace detail {
/** Model the size and alignment of the list node allocator rebind. */
template <typename T>
struct FakeListNode {
    void* next;
    void* prev;
    T value;
};
}  // namespace detail

/** List-like sequence with some inline storage. */
template <typename T,
          size_t inlineElementCount,
          typename MemoryBase = MemoryBuffer<inlineElementCount * sizeof(detail::FakeListNode<T>),
                                             alignof(detail::FakeListNode<T>)>,
          typename Allocator = typename MemoryBase::template AllocatorType<T>,
          typename ContainerBase = std::list<T, Allocator>>
class List : MemoryBase, public ContainerBase {
public:
    using typename ContainerBase::size_type;
    using typename ContainerBase::value_type;

    // `std::list` constructors
    List() : ContainerBase{_alloc()} {}
    explicit List(size_type count) : ContainerBase{count, _alloc()} {}
    List(size_type count, const value_type& value) : ContainerBase{count, value, _alloc()} {}
    template <class InputIt>
    List(InputIt first, InputIt last)
        : ContainerBase{std::move(first), std::move(last), _alloc()} {}
    explicit(false) List(std::initializer_list<T> init) : ContainerBase{init, _alloc()} {}

private:
    Allocator _alloc() {
        return this->template makeAllocator<T>();
    }
};

/** Yet another kind of `std::vector` with some inline storage. */
template <typename T,
          size_t inlineElementCount,
          typename MemoryBase = MemoryBuffer<inlineElementCount * sizeof(detail::FakeListNode<T>),
                                             alignof(detail::FakeListNode<T>)>,
          typename Alloc = typename MemoryBase::template AllocatorType<T>,
          typename ContainerBase = std::vector<T, Alloc>>
class Vector : private MemoryBase, public ContainerBase {
public:
    using typename ContainerBase::size_type;
    using typename ContainerBase::value_type;

    // `std::vector` constructors
    Vector() : ContainerBase{_alloc()} {}
    explicit Vector(size_type count) : ContainerBase{count, _alloc()} {}
    Vector(size_type count, const value_type& value) : ContainerBase{count, value, _alloc()} {}
    explicit(false) Vector(std::initializer_list<value_type> init)
        : ContainerBase{init, _alloc()} {}
    template <class InIt>
    Vector(InIt first, InIt last) : ContainerBase{first, last, _alloc()} {}

    Vector(const Vector&) = default;
    Vector& operator=(const Vector&) = default;
    Vector(Vector&&) noexcept = default;
    Vector& operator=(Vector&&) noexcept = default;

private:
    Alloc _alloc() {
        return this->template makeAllocator<T>();
    }
};

}  // namespace mongo::inline_memory
