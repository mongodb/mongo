/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/util/aligned.h"

#include <utility>

#include "mongo/base/static_assert.h"
#include "mongo/unittest/unittest.h"

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
    static constexpr size_t a = stdx::hardware_destructive_interference_size;
    MONGO_STATIC_ASSERT(alignof(CacheExclusive<char>) == a);
    MONGO_STATIC_ASSERT(alignof(CacheExclusive<char>) == a);
}

}  // namespace
}  // namespace mongo
