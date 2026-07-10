/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <array>
#include <iterator>
#include <list>
#include <memory_resource>
#include <utility>

/* In debug mode, MSCV enables iterator debugging, which additional checks are
 * executed for consistency. So, when two iterators are compared, it is tested
 * that they point to elements of the same container. If the check fails, then
 * the program is aborted.
 *
 * This checks are implemented through additionally allocated objects in default
 * STL containers, which slightly modifies the normal workflow of the allocator.
 * Due to that, we have to handle such cases of compiler hardening differently.
 */
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL >= 2
#define MONGO_MSVC_ITERATOR_DEBUG 1
#else
#define MONGO_MSVC_ITERATOR_DEBUG 0
#endif

namespace mongo {

/**
 * Thin adapter making std::pmr::memory_resource usable as a non-polymorphic
 * upstream resource for RecyclingFixedSizeBufferResource. Defaults to new/delete.
 */
class [[MONGO_MOD_PRIVATE]] PmrUpstreamResource {
public:
    PmrUpstreamResource() : _mr{std::pmr::new_delete_resource()} {}
    explicit PmrUpstreamResource(std::pmr::memory_resource* mr) : _mr{mr} {}

    void* allocate(size_t sz, size_t al) {
        return _mr->allocate(sz, al);
    }

    void deallocate(void* p, size_t sz, size_t al) {
        _mr->deallocate(p, sz, al);
    }

    bool operator==(const PmrUpstreamResource& o) const {
        return _mr == o._mr;
    }

private:
    std::pmr::memory_resource* _mr;
};

/**
 * Fixed-size block allocator, backed by existing memory buffer passed at construction.
 *
 * All allocations must be for the same size and alignment (the first allocate call
 * establishes these values). The initial buffer is split into fixed-size blocks on first
 * use.
 *
 * When a block is freed it is returned to an intrusive free list and reclaimed on the
 * next allocation, avoiding any heap interaction and additional memory allocation.
 *
 * When all blocks in the initial buffer are in use, a new overflow chunk is allocated
 * from the upstream resource. Each overflow chunk holds `overflowChunkElementCount` blocks.
 */
template <typename Upstream = PmrUpstreamResource>
class [[MONGO_MOD_PRIVATE]] RecyclingFixedSizeBufferResource {
public:
    RecyclingFixedSizeBufferResource(void* buf,
                                     size_t sz,
                                     size_t overflowChunkElementCount,
                                     Upstream upstream = {})
        : _upstream{std::move(upstream)},
          _buf{buf},
          _bufSize{sz},
          _overflowChunkElementCount{overflowChunkElementCount} {}

    ~RecyclingFixedSizeBufferResource() {
        while (_chunks) {
            auto ch = std::exchange(_chunks, _chunks->next);
            _upstream.deallocate(ch->buf, ch->sz, ch->alignment);
            _upstream.deallocate(ch, sizeof(Chunk), alignof(Chunk));
        }
    }

    void* allocate(size_t sz, size_t al = alignof(std::max_align_t)) {
        // Precondition: sz must be a multiple of al. This allocator manages
        // fixed-size, fixed-alignment blocks; mismatched sizes corrupt the free list.

        if (_blockSize == 0) {
            _blockSize = sz;
            _blockAlignment = al;
            _addBufferToFreeList(_buf, _bufSize);
        }

        if (!_freeList) {
            _newChunk();
        }

        auto block = _freeList;
        _freeList = _freeList->next;
        return block;
    }

    void deallocate(void* p, size_t sz, size_t al) {
        // Precondition: sz must be a multiple of al. This allocator manages
        // fixed-size, fixed-alignment blocks; mismatched sizes corrupt the free list.

        if (!p) {
            return;
        }

        auto block = static_cast<FreeBlock*>(p);
        block->next = _freeList;
        _freeList = block;
    }

    bool operator==(const RecyclingFixedSizeBufferResource& o) const {
        return this == &o;
    }

private:
    struct FreeBlock {
        FreeBlock* next;
    };

    struct Chunk {
        Chunk* next = nullptr;
        void* buf = nullptr;
        size_t sz = 0;
        size_t alignment = 0;
    };

    void _addBufferToFreeList(void* buf, size_t bufSize) {
        void* current = buf;
        size_t available = bufSize;

        void* p = std::align(_blockAlignment, _blockSize, current, available);
        if (!p) {
            return;
        }

        auto* block = static_cast<std::byte*>(p);
        auto* end = static_cast<std::byte*>(buf) + bufSize;

        while (block + _blockSize <= end) {
            auto freeBlock = reinterpret_cast<FreeBlock*>(block);
            freeBlock->next = _freeList;
            _freeList = freeBlock;

            block += _blockSize;
        }
    }

    MONGO_COMPILER_NOINLINE MONGO_COMPILER_COLD_FUNCTION void _newChunk() {
        auto newChunk = static_cast<Chunk*>(_upstream.allocate(sizeof(Chunk), alignof(Chunk)));

        try {
            const size_t chunkSize = _overflowChunkElementCount * _blockSize;
            const size_t chunkAlignment = std::max(_blockAlignment, alignof(std::max_align_t));

            newChunk->buf = _upstream.allocate(chunkSize, chunkAlignment);
            newChunk->sz = chunkSize;
            newChunk->alignment = chunkAlignment;
        } catch (...) {
            _upstream.deallocate(newChunk, sizeof(Chunk), alignof(Chunk));
            throw;
        }

        newChunk->next = std::exchange(_chunks, newChunk);

        _addBufferToFreeList(newChunk->buf, newChunk->sz);
    }

    Chunk* _chunks = nullptr;

    MONGO_COMPILER_NO_UNIQUE_ADDRESS Upstream _upstream;

    void* const _buf;
    const size_t _bufSize;

    const size_t _overflowChunkElementCount;

    size_t _blockSize = 0;
    size_t _blockAlignment = 0;

    FreeBlock* _freeList = nullptr;
};

namespace detail {

/** Models the size and alignment of a std::list node for inline buffer sizing. */
template <typename T>
struct [[MONGO_MOD_PRIVATE]] ListNodeSizer {
    void* next;
    void* prev;
    T value;
};

/**
 * Adapts RecyclingFixedSizeBufferResource as a std::pmr::memory_resource so that
 * std::pmr::list can be driven by our custom free-list recycler via polymorphic_allocator.
 */
template <typename Upstream = PmrUpstreamResource>
class [[MONGO_MOD_PRIVATE]] RecyclingFixedSizeBufferPmrResource : public std::pmr::memory_resource {
public:
    RecyclingFixedSizeBufferPmrResource(void* buf,
                                        size_t sz,
                                        size_t overflowChunkElementCount,
                                        Upstream upstream = {})
        : _impl{buf, sz, overflowChunkElementCount, std::move(upstream)} {}

protected:
    void* do_allocate(size_t sz, size_t al) override {
        return _impl.allocate(sz, al);
    }

    void do_deallocate(void* p, size_t sz, size_t al) override {
        _impl.deallocate(p, sz, al);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    RecyclingFixedSizeBufferResource<Upstream> _impl;
};

/**
 * Memory base for RecyclingPmrInlineList. Declares members in initialization order:
 * _buf -> _resource (takes pointer to _buf).
 */
template <typename T, size_t inlineElementCount, size_t overflowChunkElementCount>
class [[MONGO_MOD_PRIVATE]] RecyclingPmrMemoryBase {
public:
    std::pmr::polymorphic_allocator<void> makeAllocator() {
        return std::pmr::polymorphic_allocator<void>{&_resource};
    }

private:
    static constexpr size_t kNodeSz = sizeof(ListNodeSizer<T>);
    static constexpr size_t kNodeAl = alignof(ListNodeSizer<T>);

    alignas(kNodeAl) std::array<std::byte, inlineElementCount * kNodeSz> _buf;
    RecyclingFixedSizeBufferPmrResource<> _resource{
        _buf.data(), _buf.size(), overflowChunkElementCount};
};

/**
 * Memory base for DefaultRecyclingPmrInlineList. Members are declared in initialization order:
 * _buf -> _mono (uses _buf) -> _opts -> _pool (uses _mono).
 */
template <typename T, size_t inlineElementCount, size_t overflowChunkElementCount>
class [[MONGO_MOD_PRIVATE]] DefaltRecyclingPmrMemoryBase {
public:
    std::pmr::polymorphic_allocator<void> makeAllocator() {
        return std::pmr::polymorphic_allocator<void>{&_pool};
    }

private:
    static constexpr size_t kNodeSz = sizeof(ListNodeSizer<T>);
    static constexpr size_t kNodeAl = alignof(ListNodeSizer<T>);

    alignas(kNodeAl) std::array<std::byte, inlineElementCount * kNodeSz> _buf;
    std::pmr::monotonic_buffer_resource _mono{
        _buf.data(), _buf.size(), std::pmr::new_delete_resource()};
    std::pmr::pool_options _opts{overflowChunkElementCount, kNodeSz};
    std::pmr::unsynchronized_pool_resource _pool{_opts, &_mono};
};

}  // namespace detail

/**
 * std::pmr::list<T> backed by RecyclingFixedSizeBufferResource via std::pmr virtual dispatch,
 * with an inline buffer sized exactly for inlineElementCount nodes.
 *
 * Not copyable or movable directly: the embedded buffer address is captured by the resource,
 * so relocating the object would corrupt internal pointers. The owning map handles copy/move
 * by constructing a fresh list and re-inserting elements.
 */
template <typename T,
          size_t inlineElementCount,
          size_t overflowChunkElementCount = inlineElementCount>
class [[MONGO_MOD_PRIVATE]] RecyclingPmrInlineList
    : private detail::RecyclingPmrMemoryBase<T, inlineElementCount, overflowChunkElementCount>,
      public std::pmr::list<T> {
    using MemBase =
        detail::RecyclingPmrMemoryBase<T, inlineElementCount, overflowChunkElementCount>;

public:
    RecyclingPmrInlineList() : std::pmr::list<T>{MemBase::makeAllocator()} {}

    RecyclingPmrInlineList(const RecyclingPmrInlineList&) = delete;
    RecyclingPmrInlineList& operator=(const RecyclingPmrInlineList&) = delete;
    RecyclingPmrInlineList(RecyclingPmrInlineList&&) = delete;
    RecyclingPmrInlineList& operator=(RecyclingPmrInlineList&&) = delete;
};

/**
 * std::pmr::list<T> backed by default unsyncrhonized_pool_resource for node recycling
 * over monotonic_buffer_resource, with an inline bufer sized exactly for inlineElement nodes.
 *
 * Not copyable or movable directly: the embedded buffer address is captured by the resource,
 * so relocating the object would corrupt internal pointers. The owning map handles copy/move
 * by constructing a fresh list and re-inserting elements.
 *
 * Is substantially slower than RecyclingPmrInlineList, due to using different recycling approach
 * based on bucketing same-sized buffers instead of linking them through free list. However
 * that lets it bypass the rule of making all of the element allocations same sized, which
 * can be occasionally broken during enabling test hardening.
 * For example in case of MSCV turning on Iterator debugging through _ITERATOR_DEBUG_LEVEL flag.
 */
template <typename T,
          size_t inlineElementCount,
          size_t overflowChunkElementCount = inlineElementCount>
class [[MONGO_MOD_PRIVATE]] DefaultRecyclingPmrInlineList
    : private detail::
          DefaltRecyclingPmrMemoryBase<T, inlineElementCount, overflowChunkElementCount>,
      public std::pmr::list<T> {
    using MemBase =
        detail::DefaltRecyclingPmrMemoryBase<T, inlineElementCount, overflowChunkElementCount>;

public:
    DefaultRecyclingPmrInlineList() : std::pmr::list<T>{MemBase::makeAllocator()} {}

    DefaultRecyclingPmrInlineList(const DefaultRecyclingPmrInlineList&) = delete;
    DefaultRecyclingPmrInlineList& operator=(const DefaultRecyclingPmrInlineList&) = delete;
    DefaultRecyclingPmrInlineList(DefaultRecyclingPmrInlineList&&) = delete;
    DefaultRecyclingPmrInlineList& operator=(DefaultRecyclingPmrInlineList&&) = delete;
};


// In case if test compiler hardening on iterators is turned on vie _ITERATOR_DEBUG_LEVEL flag,
// some of the assumptions about the allocation strategy might break, therefore falling back
// to default STL recycling allocator at a performance cost.
#if MONGO_MSVC_ITERATOR_DEBUG
template <typename T,
          size_t inlineElementCount,
          size_t overflowChunkElementCount = inlineElementCount>
using RecyclingList =
    DefaultRecyclingPmrInlineList<T, inlineElementCount, overflowChunkElementCount>;
#else
template <typename T,
          size_t inlineElementCount,
          size_t overflowChunkElementCount = inlineElementCount>
using RecyclingList = RecyclingPmrInlineList<T, inlineElementCount, overflowChunkElementCount>;
#endif

/**
 * This is a wrapper around list which provides find and insertion-like syntax, while
 * maintaining insertion order in the map.
 *
 * Asymptotics:
 *
 * - Insert: avg O(N)
 *   List provide amortized O(1) semantics for insertion. However the first insertion
 *   performs a find to ensure the element has not already been added.
 *
 * - Lookup by key: avg O(N)
 *   Lookups scan over entire entire list in search of the same key. The insertion
 *   order of list does not provide faster iteration.
 *
 * - Remove: O(1)
 *   Removing an element from the list is O(1), assuming the element location
 *   is known. If the location is unknown, it is avg O(N) due to lookup call.
 *
 * Memory:
 *
 * - O(N)
 *   Memory usage is linear in the number of elements, with additional overhead
 *   from the internal structure of the list.
 */
template <class Key, class Value, size_t InlineCapacity>
class [[MONGO_MOD_PRIVATE]] RecyclingListBasedMap {
public:
    struct Entry : std::pair<Key, Value> {
        using std::pair<Key, Value>::pair;

        const Key& key() const {
            return this->first;
        }
        Key& key() {
            return this->first;
        }

        const Value& value() const {
            return this->second;
        }
        Value& value() {
            return this->second;
        }
    };

    using List = RecyclingList<Entry, InlineCapacity>;
    using Iterator = typename List::iterator;
    using ConstIterator = typename List::const_iterator;

    RecyclingListBasedMap() = default;
    ~RecyclingListBasedMap() = default;

    RecyclingListBasedMap(const RecyclingListBasedMap& other) {
        for (const auto& entry : other._list) {
            _list.push_back(entry);
        }
    }

    RecyclingListBasedMap& operator=(const RecyclingListBasedMap& other) {
        if (this == &other) {
            return *this;
        }

        _list.clear();
        for (auto& entry : other._list) {
            _list.push_back(entry);
        }
        return *this;
    }

    RecyclingListBasedMap& operator=(RecyclingListBasedMap&& other) {
        if (this == &other) {
            return *this;
        }

        _list.clear();
        for (const auto& entry : other._list) {
            _list.push_back(std::move(entry));
        }
        other._list.clear();

        return *this;
    }

    RecyclingListBasedMap(RecyclingListBasedMap&& other) {
        for (auto& entry : other._list) {
            _list.push_back(std::move(entry));
        }
        other._list.clear();
    }

    bool empty() const noexcept {
        return _list.empty();
    }
    size_t size() const noexcept {
        return _list.size();
    }

    Iterator begin() noexcept {
        return _list.begin();
    }
    ConstIterator begin() const noexcept {
        return _list.begin();
    }
    ConstIterator cbegin() const noexcept {
        return _list.cbegin();
    }

    Iterator end() noexcept {
        return _list.end();
    }
    ConstIterator end() const noexcept {
        return _list.end();
    }
    ConstIterator cend() const noexcept {
        return _list.cend();
    }

    Iterator find(const Key& key) {
        Iterator it;
        for (it = _list.begin(); it != _list.end(); ++it) {
            if (it->key() == key)
                break;
        }

        return it;
    }

    ConstIterator find(const Key& key) const {
        ConstIterator it;
        for (it = _list.cbegin(); it != _list.cend(); ++it) {
            if (it->key() == key)
                break;
        }

        return it;
    }

    Iterator insert(const Key& key, const Value& value) {
        auto it = find(key);
        if (it != end()) {
            return it;
        }

        _list.emplace_back(key, value);
        return std::prev(_list.end());
    }

    template <class KeyArg>
    Iterator emplace(KeyArg&& key) {
        auto it = find(key);
        if (it != end())
            return it;

        _list.emplace_back(std::forward<KeyArg>(key), Value{});
        return std::prev(_list.end());
    }

    template <class KeyArg, class ValueArg>
    Iterator emplace(KeyArg&& key, ValueArg&& value) {
        auto it = find(key);
        if (it != end())
            return it;

        _list.emplace_back(std::forward<KeyArg>(key), std::forward<ValueArg>(value));
        return std::prev(_list.end());
    }

    bool erase(const Key& key) {
        auto it = find(key);
        if (it == _list.end())
            return false;

        _list.erase(it);
        return true;
    }

    Iterator erase(Iterator it) {
        auto next = std::next(it);
        _list.erase(it);
        return next;
    }

private:
    List _list;
};

}  // namespace mongo
