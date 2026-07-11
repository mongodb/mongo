// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/inline_memory.h"

#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/demangle.h"

#include <list>
#include <utility>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::inline_memory {
namespace {

class MonotonicBufferResourceTest : public unittest::Test {
public:
    template <typename T>
    T sorted(const T& seq) {
        auto tmp = seq;
        std::sort(tmp.begin(), tmp.end());
        return tmp;
    }

    template <typename Upstream = NewDeleteResource>
    struct MockResource {
        MockResource(std::function<void(void*, size_t, size_t)> onAlloc,
                     std::function<void(void*, size_t, size_t)> onDealloc)
            : onAlloc{std::move(onAlloc)}, onDealloc{std::move(onDealloc)} {}

        void* allocate(size_t sz, size_t al) {
            auto p = upstream.allocate(sz, al);
            if (onAlloc)
                onAlloc(p, sz, al);
            return p;
        }

        void deallocate(void* p, size_t sz, size_t al) {
            if (onDealloc)
                onDealloc(p, sz, al);
            return upstream.deallocate(p, sz, al);
        }

        std::function<void(void*, size_t, size_t)> onAlloc;
        std::function<void(void*, size_t, size_t)> onDealloc;
        MONGO_COMPILER_NO_UNIQUE_ADDRESS Upstream upstream;
    };

    std::vector<std::tuple<void*, size_t, size_t>> upstreamAllocations;
    std::vector<std::tuple<void*, size_t, size_t>> upstreamDeallocations;
    MockResource<> mockResource{
        [&](void* p, size_t sz, size_t al) { upstreamAllocations.push_back({p, sz, al}); },
        [&](void* p, size_t sz, size_t al) {
            upstreamDeallocations.push_back({p, sz, al});
        }};
};

TEST_F(MonotonicBufferResourceTest, UpstreamAllocationsReduced) {
    // Probe to find out when the upstream allocations start.
    size_t mbrAllocations = 0;
    constexpr size_t bufSize = 1024;
    std::array<std::byte, bufSize> buf;
    {
        MonotonicBufferResource mbr(buf.data(), buf.size(), mockResource);
        for (; upstreamAllocations.empty(); ++mbrAllocations) {
            void* p = mbr.allocate(1, 1);
            if (upstreamAllocations.empty()) {
                ASSERT(p >= buf.data() && p < buf.data() + buf.size())
                    << "Initial allocations should be within buffer\n";
            }
            mbr.deallocate(p, 1, 1);
        }
    }
    ASSERT_EQ(sorted(upstreamAllocations), sorted(upstreamDeallocations));
    ASSERT_EQ(mbrAllocations, bufSize + 1) << "1-byte allocations should be packed";
}

/**
 * Observe the exponential growth of chunks requested from upstream.
 * Burn allocations until we get enough upstream to verify the growth pattern.
 */
TEST_F(MonotonicBufferResourceTest, ChunkGrowth) {
    size_t growthNum;
    size_t growthDen;
    {
        MonotonicBufferResource mbr(nullptr, 0, mockResource);
        growthNum = decltype(mbr)::ChunkGrowth::num;
        growthDen = decltype(mbr)::ChunkGrowth::den;
        while (upstreamAllocations.size() < 10) {
            for (size_t prev = upstreamAllocations.size(); upstreamAllocations.size() == prev;) {
                mbr.deallocate(mbr.allocate(1, 1), 1, 1);
            }
        }
    }
    ASSERT_EQ(sorted(upstreamAllocations), sorted(upstreamDeallocations));
    size_t prevSize = 0;

    size_t inferredChunkSize = 0;
    for (size_t i = 0; i != upstreamAllocations.size(); ++i) {
        auto&& [p, sz, al] = upstreamAllocations[i];
        if (i % 2 == 0) {
            // First and every even-numbered allocation following should be a Chunk allocation.
            if (!inferredChunkSize)
                inferredChunkSize = sz;
            ASSERT_EQ(sz, inferredChunkSize) << "Expected a Chunk allocation";
            continue;
        }
        if (prevSize) {
            ASSERT_EQ(sz, prevSize * growthNum / growthDen);
        }
        prevSize = sz;
    }
}

template <size_t N, typename F>
void templateForEachSize(F f) {
    [&]<size_t... I>(std::index_sequence<I...>) {
        (f(std::integral_constant<size_t, I>{}), ...);
    }(std::make_index_sequence<N>{});
}

template <typename Sequence>
void testFillSequence(size_t n) {
    Sequence seq;
    size_t i = 0;
    for (; i != n; ++i)
        seq.emplace_back(i);
    i = 0;
    for (const auto& el : seq)
        ASSERT_EQ(el, i++);
}

TEST(InlineListTest, ExceedCapacity) {
    templateForEachSize<10>(
        []<size_t I>(std::integral_constant<size_t, I>) { testFillSequence<List<int, I>>(2 * I); });
}

TEST(InlineVectorTest, ExceedCapacity) {
    templateForEachSize<10>([]<size_t I>(std::integral_constant<size_t, I>) {
        testFillSequence<Vector<int, I>>(2 * I);
    });
}

/**
 * A `std::list<T, A>` will rebind its `A` to allocate list nodes.
 * Used by `ListNodeSizeEstimation` below.
 *
 * Catches the first such rebind in the `record` pointer. Subsequent
 * rebinds are possible but ignored as they are not important for
 * performance tuning.
 */
template <typename T>
class RebindCanary : public std::allocator<T> {
public:
    explicit RebindCanary(size_t* record) : record{record} {}

    template <typename U>
    RebindCanary(const RebindCanary<U>& src) {
        LOGV2(8856200,
              "RebindCanary",
              "fromTypename"_attr = demangleName(typeid(U)),
              "fromSize"_attr = sizeof(U),
              "fromAlign"_attr = alignof(U),
              "toTypename"_attr = demangleName(typeid(T)),
              "toSize"_attr = sizeof(T),
              "toAlign"_attr = alignof(T),
              "ignoring"_attr = !src.record);
        if (src.record)
            *std::exchange(src.record, {}) = sizeof(T);
    }

    mutable size_t* record{};
};

/**
 * The size of inline_memory::detail::FakeListNode<T> should match the
 * reality of std::list node allocation.
 *
 * `std::list<T,A>` gets an allocator of `T`, but really needs an allocator of
 * `_Node<T>`.  We need to know what the size of those `_Node<T>` are in order
 * to make optimal buffers for `List` to use.
 *
 * This is detectable using an instrumented rebind conversion constructor.
 */
TEST(InlineListTest, ListNodeSizeEstimation) {
    size_t rebindSize = 0;
    RebindCanary<int> alloc{&rebindSize};
    std::list<int, RebindCanary<int>> list{alloc};
    ASSERT_EQ(rebindSize, sizeof(detail::FakeListNode<int>));
}

}  // namespace
}  // namespace mongo::inline_memory
