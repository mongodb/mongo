// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/fast_list_based_map.h"

#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <array>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <absl/container/node_hash_map.h>


namespace mongo {

struct TestStruct {
    int id;
    std::string value;

    void initNew(int newId, const std::string& newValue) {
        id = newId;
        value = newValue;
    }
};

typedef class RecyclingListBasedMap<ResourceId, TestStruct, 8> TestRecyclingListBasedMap;


TEST(RecyclingListBasedMap, Empty) {
    TestRecyclingListBasedMap map;
    ASSERT(map.empty());

    TestRecyclingListBasedMap::Iterator it = map.begin();
    ASSERT(it == map.end());
}

TEST(RecyclingListBasedMap, NotEmpty) {
    TestRecyclingListBasedMap map;

    map.emplace(ResourceId(RESOURCE_COLLECTION, 1))->value().initNew(101, "Item101");
    map.emplace(ResourceId(RESOURCE_COLLECTION, 2))->value().initNew(102, "Item102");
    ASSERT(!map.empty());

    TestRecyclingListBasedMap::Iterator it = map.begin();
    ASSERT(it != map.end());

    ASSERT(it->value().id == 101);
    ASSERT(it->value().value == "Item101");

    it = std::next(it);
    ASSERT(it != map.end());

    ASSERT(it->value().id == 102);
    ASSERT(it->value().value == "Item102");

    // We are at the last element
    it = std::next(it);
    ASSERT(it == map.end());
}

TEST(RecyclingListBasedMap, FindNonExisting) {
    TestRecyclingListBasedMap map;

    ASSERT(map.find(ResourceId(RESOURCE_COLLECTION, 0)) == map.end());
}

TEST(RecyclingListBasedMap, FindAndRemove) {
    TestRecyclingListBasedMap map;

    for (int i = 0; i < 6; i++) {
        map.emplace(ResourceId(RESOURCE_COLLECTION, i))
            ->value()
            .initNew(i, "Item" + std::to_string(i));
    }

    for (int i = 0; i < 6; i++) {
        ASSERT(map.find(ResourceId(RESOURCE_COLLECTION, i)) != map.end());

        ASSERT_EQUALS(i, map.find(ResourceId(RESOURCE_COLLECTION, i))->value().id);

        ASSERT_EQUALS("Item" + std::to_string(i),
                      map.find(ResourceId(RESOURCE_COLLECTION, i))->value().value);
    }

    // Remove a middle entry
    map.erase(ResourceId(RESOURCE_COLLECTION, 2));
    ASSERT(map.find(ResourceId(RESOURCE_COLLECTION, 2)) == map.end());

    // Remove entry after first
    map.erase(ResourceId(RESOURCE_COLLECTION, 1));
    ASSERT(map.find(ResourceId(RESOURCE_COLLECTION, 1)) == map.end());

    // Remove entry before last
    map.erase(ResourceId(RESOURCE_COLLECTION, 4));
    ASSERT(map.find(ResourceId(RESOURCE_COLLECTION, 4)) == map.end());

    // Remove first entry
    map.erase(ResourceId(RESOURCE_COLLECTION, 0));
    ASSERT(map.find(ResourceId(RESOURCE_COLLECTION, 0)) == map.end());

    // Remove last entry
    map.erase(ResourceId(RESOURCE_COLLECTION, 5));
    ASSERT(map.find(ResourceId(RESOURCE_COLLECTION, 5)) == map.end());

    // Remove final entry
    map.erase(ResourceId(RESOURCE_COLLECTION, 3));
    ASSERT(map.find(ResourceId(RESOURCE_COLLECTION, 3)) == map.end());
}

TEST(RecyclingListBasedMap, RemoveAll) {
    TestRecyclingListBasedMap map;
    stdx::unordered_map<ResourceId, TestStruct> checkMap;

    for (int i = 1; i <= 6; i++) {
        map.emplace(ResourceId(RESOURCE_COLLECTION, i))
            ->value()
            .initNew(i, "Item" + std::to_string(i));

        checkMap[ResourceId(RESOURCE_COLLECTION, i)].initNew(i, "Item" + std::to_string(i));
    }

    TestRecyclingListBasedMap::Iterator it = map.begin();
    while (it != map.end()) {
        ASSERT_EQUALS(it->value().id, checkMap[it->key()].id);
        ASSERT_EQUALS("Item" + std::to_string(it->value().id), checkMap[it->key()].value);

        checkMap.erase(it->key());
        it = map.erase(it);
    }

    ASSERT(map.empty());
    ASSERT(checkMap.empty());
}

TEST(RecyclingListBasedMap, PreserveIterationOrder) {
    TestRecyclingListBasedMap map;

    for (int i = 0; i < 100; i++) {
        map.emplace(ResourceId(RESOURCE_COLLECTION, i))
            ->value()
            .initNew(i, "Item" + std::to_string(i));
    }

    auto it = map.begin();
    for (int i = 0; i < 100; i++) {
        ASSERT(it->value().id == i);
        it = std::next(it);
    }

    // deleting elements from the middle
    it = map.begin();
    for (int i = 0; i < 25; i++) {
        it = std::next(it);
    }
    for (int i = 0; i < 50; i++) {
        it = map.erase(it);
    }

    // adding 50 more elements
    for (int i = 100; i < 150; i++) {
        map.emplace(ResourceId(RESOURCE_COLLECTION, i))
            ->value()
            .initNew(i, "Item" + std::to_string(i));
    }

    // these 50 elements should be in the end
    it = map.begin();
    for (int i = 0; i < 25; i++) {
        ASSERT(it->value().id == i);
        it = std::next(it);
    }
    for (int i = 75; i < 100; i++) {
        ASSERT(it->value().id == i);
        it = std::next(it);
    }
    for (int i = 100; i < 150; i++) {
        ASSERT(it->value().id == i);
        it = std::next(it);
    }
}

}  // namespace mongo

namespace mongo {
namespace {

class RecyclingBufferResourceTest : public unittest::Test {
protected:
    struct MockResource {
        MockResource(std::function<void(void*, size_t, size_t)> onAlloc,
                     std::function<void(void*, size_t, size_t)> onDealloc)
            : onAlloc{std::move(onAlloc)}, onDealloc{std::move(onDealloc)} {}

        void* allocate(size_t sz, size_t al) {
            auto p = _upstream.allocate(sz, al);
            if (onAlloc)
                onAlloc(p, sz, al);
            return p;
        }

        void deallocate(void* p, size_t sz, size_t al) {
            if (onDealloc)
                onDealloc(p, sz, al);
            _upstream.deallocate(p, sz, al);
        }

        std::function<void(void*, size_t, size_t)> onAlloc;
        std::function<void(void*, size_t, size_t)> onDealloc;
        PmrUpstreamResource _upstream;
    };

    template <typename T>
    T sorted(const T& seq) {
        auto tmp = seq;
        std::sort(tmp.begin(), tmp.end());
        return tmp;
    }

    std::vector<std::tuple<void*, size_t, size_t>> upstreamAllocations;
    std::vector<std::tuple<void*, size_t, size_t>> upstreamDeallocations;
    MockResource mockResource{
        [&](void* p, size_t sz, size_t al) { upstreamAllocations.push_back({p, sz, al}); },
        [&](void* p, size_t sz, size_t al) {
            upstreamDeallocations.push_back({p, sz, al});
        }};
};

TEST_F(RecyclingBufferResourceTest, UpstreamAllocationsReduced) {
    constexpr size_t kBlockSize = sizeof(void*);
    constexpr size_t kBlockAlign = alignof(void*);
    constexpr size_t kBlocks = 1024;
    constexpr size_t bufSize = kBlocks * kBlockSize;
    std::array<std::byte, bufSize> buf;
    {
        RecyclingFixedSizeBufferResource mbr(buf.data(), buf.size(), 10, mockResource);
        std::vector<void*> ptrs;
        ptrs.reserve(kBlocks + 1);

        for (size_t i = 0; i != kBlocks; ++i) {
            void* p = mbr.allocate(kBlockSize, kBlockAlign);
            ASSERT_TRUE(upstreamAllocations.empty())
                << "No allocations from upstream while within initial buffer";
            ASSERT(p >= buf.data() && p < buf.data() + buf.size())
                << "Initial allocations should be within buffer\n";
            ptrs.push_back(p);
        }

        void* overflow = mbr.allocate(kBlockSize, kBlockAlign);
        ptrs.push_back(overflow);
        ASSERT_FALSE(upstreamAllocations.empty())
            << "Allocation from upstream should be used upon exhaustion of initial buffer";

        for (auto p : ptrs)
            mbr.deallocate(p, kBlockSize, kBlockAlign);
    }
    ASSERT_EQ(sorted(upstreamAllocations), sorted(upstreamDeallocations));
}

TEST_F(RecyclingBufferResourceTest, ChunkGrowth) {
    constexpr size_t kBlockSize = sizeof(void*);
    constexpr size_t kBlockAlign = alignof(void*);
    constexpr size_t kOverflowElementCount = 2;
    // Making 10 allocations on the buffer, causing overflow of the overflow storage
    // to be increased 5 times
    {
        RecyclingFixedSizeBufferResource mbr(nullptr, 0, kOverflowElementCount, mockResource);
        std::vector<void*> ptrs;
        ptrs.reserve(10);

        while (upstreamAllocations.size() < 10) {
            ptrs.push_back(mbr.allocate(kBlockSize, kBlockAlign));
        }

        for (auto p : ptrs)
            mbr.deallocate(p, kBlockSize, kBlockAlign);
    }
    ASSERT_EQ(sorted(upstreamAllocations), sorted(upstreamDeallocations));

    // While iterating over all allocations being done, we're ensuring their sizes
    size_t inferredChunkSize = 0;
    for (size_t i = 0; i != upstreamAllocations.size(); ++i) {
        auto&& [p, sz, al] = upstreamAllocations[i];
        // Every odd allocation (since amount of elements in overflow storage is 2)
        // should lead to overflow storage increase
        if (i % 2 == 0) {
            if (!inferredChunkSize)
                inferredChunkSize = sz;
            ASSERT_EQ(sz, inferredChunkSize) << "Expected a Chunk allocation";
            continue;
        }
        // The extra-memory allocated should be always fixed-sized and equal to
        // overflow element number of blocks
        ASSERT_EQ(sz, kOverflowElementCount * kBlockSize) << "Every chunk should be fixed-sized";
    }
}

}  // namespace
}  // namespace mongo
