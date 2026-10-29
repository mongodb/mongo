// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/aligned.h"

#include "mongo/base/static_assert.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <utility>
#include <variant>

namespace mongo {
namespace {

struct MoveOnly {
    static constexpr int kEmpty = -1;

    MoveOnly() = default;
    explicit MoveOnly(int val) : val(val) {}

    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly& that) = delete;

    MoveOnly(MoveOnly&& that) : val{std::exchange(that.val, kEmpty)} {}
    MoveOnly& operator=(MoveOnly&& that) {
        val = std::exchange(that.val, kEmpty);
        return *this;
    }

    int val = 12345;
};

struct Final final {};
MONGO_STATIC_ASSERT(alignof(Aligned<Final, 1>) == 1);

MONGO_STATIC_ASSERT(alignof(Aligned<char, 1>) == 1);
MONGO_STATIC_ASSERT(alignof(Aligned<char, 2>) == 2);
MONGO_STATIC_ASSERT(alignof(Aligned<char, 4>) == 4);
MONGO_STATIC_ASSERT(alignof(Aligned<char, 8>) == 8);
MONGO_STATIC_ASSERT(alignof(Aligned<char, 16>) == 16);
MONGO_STATIC_ASSERT(alignof(Aligned<char, 32>) == 32);

MONGO_STATIC_ASSERT(alignof(Aligned<uint64_t, 8>) == 8);
MONGO_STATIC_ASSERT(alignof(Aligned<uint64_t, 16>) == 16);
MONGO_STATIC_ASSERT(alignof(Aligned<uint64_t, 32>) == 32);


TEST(Aligned, ConstructorForwarding) {
    struct NeedsArg {
        explicit NeedsArg(int v) : v(v) {}
        int v;
    };
    Aligned<NeedsArg, sizeof(NeedsArg)> a{123};
    ASSERT_EQ(a->v, 123);
};

TEST(Aligned, MoveConstruct) {
    Aligned<MoveOnly, sizeof(MoveOnly)> m1{1};
    Aligned<MoveOnly, sizeof(MoveOnly)> m2{std::move(m1)};
    ASSERT_EQ(m1->val, MoveOnly::kEmpty);  // NOLINT(bugprone-use-after-move)
    ASSERT_EQ(m2->val, 1);
}

TEST(Aligned, MoveAssign) {
    Aligned<MoveOnly, sizeof(MoveOnly)> m1{111};
    Aligned<MoveOnly, sizeof(MoveOnly)> m2{222};
    auto&& ret = (m2 = std::move(m1));
    ASSERT(&ret == &m2);
    ASSERT_EQ(m1->val, MoveOnly::kEmpty);  // NOLINT(bugprone-use-after-move)
    ASSERT_EQ(m2->val, 111);
}

TEST(Aligned, Swap) {
    Aligned<MoveOnly, sizeof(MoveOnly)> m1{111};
    Aligned<MoveOnly, sizeof(MoveOnly)> m2{222};
    using std::swap;
    swap(m1, m2);
    ASSERT_EQ(m1->val, 222);
    ASSERT_EQ(m2->val, 111);
}

TEST(CacheExclusive, IsAlignedToPlatformCacheLine) {
    static constexpr size_t a = std::hardware_destructive_interference_size;
    MONGO_STATIC_ASSERT(alignof(CacheExclusive<char>) == a);
    MONGO_STATIC_ASSERT(alignof(CacheExclusive<char>) == a);
}

}  // namespace
}  // namespace mongo
