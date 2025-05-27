/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/util/inlined_storage.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
TEST(InlinedStorageTests, StoreAndAccessData) {
    // 1 Inlined Block.
    {
        InlinedStorage<size_t, 1> storage(1);
        storage[0] = 111;
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(1, storage.size());
    }

    // 1 Inlined Block + 1 Unused.
    {
        InlinedStorage<size_t, 2> storage(1);
        storage[0] = 111;
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(1, storage.size());
    }

    // 2 Inlined Blocks.
    {
        InlinedStorage<size_t, 2> storage(2);
        storage[0] = 111;
        storage[1] = 74;
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(74, storage[1]);
        ASSERT_EQ(2, storage.size());
    }

    // 2 Inlined Blocks + 1 Unused.
    {
        InlinedStorage<size_t, 3> storage(2);
        storage[0] = 111;
        storage[1] = 74;
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(74, storage[1]);
        ASSERT_EQ(2, storage.size());
    }

    // 3 Inlined Blocks.
    {
        InlinedStorage<size_t, 3> storage(3);
        storage[0] = 111;
        storage[1] = 74;
        storage[2] = 98;
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(74, storage[1]);
        ASSERT_EQ(98, storage[2]);
        ASSERT_EQ(3, storage.size());
    }

    // 2 Blocks on heap.
    {
        InlinedStorage<size_t, 1> storage(2);
        storage[0] = 111;
        storage[1] = 74;
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(74, storage[1]);
        ASSERT_EQ(2, storage.size());
    }

    // 3 Blocks on heap.
    {
        InlinedStorage<size_t, 1> storage(3);
        storage[0] = 111;
        storage[1] = 74;
        storage[2] = 99;
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(74, storage[1]);
        ASSERT_EQ(99, storage[2]);
        ASSERT_EQ(3, storage.size());
    }
}

TEST(InlinedStorageTests, EqualityOperator) {
    InlinedStorage<size_t, 5> s1{1, 2, 3, 4, 5, 6, 7};
    InlinedStorage<size_t, 5> s2{1, 2, 3, 4, 5, 6, 7};
    InlinedStorage<size_t, 5> s3{1, 2, 3, 4, 5, 7, 7};

    ASSERT_EQ(s1, s2);
    ASSERT_NE(s1, s3);
}

TEST(InlinedStorageTests, InitializerListConstructor) {
    // 1 Inlined Block.
    {
        InlinedStorage<size_t, 1> storage{111};
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(1, storage.size());
    }

    // 1 Inlined Block + 1 Unused.
    {
        InlinedStorage<size_t, 2> storage{111};
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(1, storage.size());
    }

    // 2 Inlined Blocks.
    {
        InlinedStorage<size_t, 2> storage{111, 74};
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(74, storage[1]);
        ASSERT_EQ(2, storage.size());
    }

    // 2 Inlined Blocks + 1 Unused.
    {
        InlinedStorage<size_t, 3> storage{111, 74};
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(74, storage[1]);
        ASSERT_EQ(2, storage.size());
    }

    // 3 Inlined Blocks.
    {
        InlinedStorage<size_t, 3> storage{111, 74, 98};
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(74, storage[1]);
        ASSERT_EQ(98, storage[2]);
        ASSERT_EQ(3, storage.size());
    }

    // 2 Blocks on heap.
    {
        InlinedStorage<size_t, 1> storage{111, 74};
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(74, storage[1]);
        ASSERT_EQ(2, storage.size());
    }

    // 3 Blocks on heap.
    {
        InlinedStorage<size_t, 1> storage{111, 74, 75};
        ASSERT_EQ(111, storage[0]);
        ASSERT_EQ(74, storage[1]);
        ASSERT_EQ(75, storage[2]);
        ASSERT_EQ(3, storage.size());
    }
}

TEST(InlinedStorageTests, CopyConstructor) {
    // 1 Inlined Block.
    {
        InlinedStorage<size_t, 1> storage{111};

        InlinedStorage<size_t, 1> copy{storage};
        ASSERT_EQ(storage, copy);
    }

    // 1 Inlined Block + 1 Unused.
    {
        InlinedStorage<size_t, 2> storage{111};

        InlinedStorage<size_t, 2> copy{storage};
        ASSERT_EQ(storage, copy);
    }

    // 2 Inlined Blocks.
    {
        InlinedStorage<size_t, 2> storage{111, 74};

        InlinedStorage<size_t, 2> copy{storage};
        ASSERT_EQ(storage, copy);
    }

    // 2 Inlined Blocks + 1 Unused.
    {
        InlinedStorage<size_t, 3> storage{111, 74};

        InlinedStorage<size_t, 3> copy{storage};
        ASSERT_EQ(storage, copy);
    }

    // 3 Inlined Blocks.
    {
        InlinedStorage<size_t, 3> storage{111, 74, 98};

        InlinedStorage<size_t, 3> copy{storage};
        ASSERT_EQ(storage, copy);
    }

    // 2 Blocks on heap.
    {
        InlinedStorage<size_t, 1> storage{111, 74};

        InlinedStorage<size_t, 1> copy{storage};
        ASSERT_EQ(storage, copy);
    }

    // 3 Blocks on heap.
    {
        InlinedStorage<size_t, 1> storage{111, 74, 99};

        InlinedStorage<size_t, 1> copy{storage};
        ASSERT_EQ(storage, copy);
    }

    // 7 Blocks on heap.
    {
        InlinedStorage<size_t, 5> storage{1, 2, 3, 4, 5, 6, 7};

        InlinedStorage<size_t, 5> copy{storage};
        ASSERT_EQ(storage, copy);
    }
}

TEST(InlinedStorageTests, MoveConstructor) {
    // 1 Inlined Block.
    {
        InlinedStorage<size_t, 1> storage{111};
        InlinedStorage<size_t, 1> expected{111};
        InlinedStorage<size_t, 1> move{std::move(storage)};
        ASSERT_EQ(expected, move);
    }

    // 1 Inlined Block + 1 Unused.
    {
        InlinedStorage<size_t, 2> storage{111};
        InlinedStorage<size_t, 2> expected{111};
        InlinedStorage<size_t, 2> move{std::move(storage)};
        ASSERT_EQ(expected, move);
    }

    // 2 Inlined Blocks.
    {
        InlinedStorage<size_t, 2> storage{111, 74};
        InlinedStorage<size_t, 2> expected{111, 74};
        InlinedStorage<size_t, 2> move{std::move(storage)};
        ASSERT_EQ(expected, move);
    }

    // 2 Inlined Blocks + 1 Unused.
    {
        InlinedStorage<size_t, 3> storage{111, 74};
        InlinedStorage<size_t, 3> expected{111, 74};
        InlinedStorage<size_t, 3> move{std::move(storage)};
        ASSERT_EQ(expected, move);
    }

    // 3 Inlined Blocks.
    {
        InlinedStorage<size_t, 3> storage{111, 74, 98};
        InlinedStorage<size_t, 3> expected{111, 74, 98};
        InlinedStorage<size_t, 3> move{std::move(storage)};
        ASSERT_EQ(expected, move);
    }

    // 2 Blocks on heap.
    {
        InlinedStorage<size_t, 1> storage{111, 74};
        InlinedStorage<size_t, 1> expected{111, 74};
        InlinedStorage<size_t, 1> move{std::move(storage)};
        ASSERT_EQ(expected, move);
    }

    // 3 Blocks on heap.
    {
        InlinedStorage<size_t, 2> storage{111, 74, 99};
        InlinedStorage<size_t, 2> expected{111, 74, 99};
        InlinedStorage<size_t, 2> move{std::move(storage)};
        ASSERT_EQ(expected, move);
    }
}

TEST(InlinedStorageTests, CopyOperator) {
    // Same size, inlined to inlined.
    {
        InlinedStorage<size_t, 2> s1{111, 74};
        InlinedStorage<size_t, 2> s2{0, 1};
        s2 = s1;
        ASSERT_EQ(s1, s2);
    }

    // Smaller to larger, inlined to inlined.
    {
        InlinedStorage<size_t, 2> s1{111};
        InlinedStorage<size_t, 2> s2{0, 1};
        s2 = s1;
        ASSERT_EQ(s1, s2);
    }

    // Larger to smaller, inlined to inlined.
    {
        InlinedStorage<size_t, 2> s1{111};
        InlinedStorage<size_t, 2> s2{0, 1};
        s1 = s2;
        ASSERT_EQ(s2, s1);
    }

    // Same size, on heap to on heap.
    {
        InlinedStorage<size_t, 2> s1{111, 112, 113};
        InlinedStorage<size_t, 2> s2{0, 1, 2};

        auto oldBufferPointer = s2.data();

        s2 = s1;
        ASSERT_EQ(oldBufferPointer, s2.data());  // The buffer's address is not changed
        ASSERT_EQ(s1, s2);
    }

    // Smaller to larger, on heap to on heap.
    {
        InlinedStorage<size_t, 2> s1{111, 112, 113};
        InlinedStorage<size_t, 2> s2{0, 1, 2, 3, 4};

        s2 = s1;
        ASSERT_EQ(s1, s2);
    }

    // Larger to smaller, on heap to on heap.
    {
        InlinedStorage<size_t, 2> s1{111, 112, 113};
        InlinedStorage<size_t, 2> s2{0, 1, 2, 3, 4};

        s1 = s2;
        ASSERT_EQ(s2, s1);
    }

    // On heap to inlined.
    {
        InlinedStorage<size_t, 2> s1{111, 112, 113};
        InlinedStorage<size_t, 2> s2{0};

        s2 = s1;
        ASSERT_EQ(s1, s2);
    }

    // Inlined to on heap.
    {
        InlinedStorage<size_t, 2> s1{111};
        InlinedStorage<size_t, 2> s2{0, 1, 2, 3, 4};

        s2 = s1;
        ASSERT_EQ(s1, s2);
    }
}

TEST(InlinedStorageTests, MoveOperator) {
    // Same size, inlined to inlined.
    {
        InlinedStorage<size_t, 2> s1{111, 74};
        InlinedStorage<size_t, 2> expected{111, 74};
        InlinedStorage<size_t, 2> s2{0, 1};
        s2 = std::move(s1);
        ASSERT_EQ(expected, s2);
    }

    // Smaller to larger, inlined to inlined.
    {
        InlinedStorage<size_t, 2> s1{111};
        InlinedStorage<size_t, 2> expected{111};
        InlinedStorage<size_t, 2> s2{0, 1};
        s2 = std::move(s1);
        ASSERT_EQ(expected, s2);
    }

    // Larger to smaller, inlined to inlined.
    {
        InlinedStorage<size_t, 2> s1{111};
        InlinedStorage<size_t, 2> expected{111};
        InlinedStorage<size_t, 2> s2{0, 1};
        s2 = std::move(s1);
        ASSERT_EQ(expected, s2);
    }

    // Same size, on heap to on heap.
    {
        InlinedStorage<size_t, 2> s1{111, 112, 113};
        InlinedStorage<size_t, 2> expected{111, 112, 113};
        InlinedStorage<size_t, 2> s2{0, 1, 2};

        auto oldBufferPointer = s1.data();

        s2 = std::move(s1);
        ASSERT_EQ(oldBufferPointer, s2.data());
        ASSERT_EQ(expected, s2);
    }

    // Smaller to larger, on heap to on heap.
    {
        InlinedStorage<size_t, 2> s1{111, 112, 113};
        InlinedStorage<size_t, 2> expected{111, 112, 113};
        InlinedStorage<size_t, 2> s2{0, 1, 2, 3, 4};

        s2 = std::move(s1);
        ASSERT_EQ(expected, s2);
    }

    // Larger to smaller, on heap to on heap.
    {
        InlinedStorage<size_t, 2> s1{111, 112, 113};
        InlinedStorage<size_t, 2> expected{111, 112, 113};
        InlinedStorage<size_t, 2> s2{0, 1, 2, 3, 4};

        s2 = std::move(s1);
        ASSERT_EQ(expected, s2);
    }

    // On heap to inlined.
    {
        InlinedStorage<size_t, 2> s1{111, 112, 113};
        InlinedStorage<size_t, 2> expected{111, 112, 113};
        InlinedStorage<size_t, 2> s2{0};

        auto oldBufferPointer = s1.data();

        s2 = std::move(s1);
        ASSERT_EQ(oldBufferPointer, s2.data());
        ASSERT_EQ(expected, s2);
    }

    // Inlined to on heap.
    {
        InlinedStorage<size_t, 2> s1{111};
        InlinedStorage<size_t, 2> expected{111};
        InlinedStorage<size_t, 2> s2{0, 1, 2, 3, 4};

        s2 = std::move(s1);
        ASSERT_EQ(expected, s2);
    }
}

TEST(InlinedStorageTests, Resize) {
    // Same size, inlined
    {
        InlinedStorage<size_t, 2> s{111};
        InlinedStorage<size_t, 2> expected{111};
        s.resize(1);
        ASSERT_EQ(expected, s);
    }

    // Same size, on heap
    {
        InlinedStorage<size_t, 2> s{111, 112, 113};
        InlinedStorage<size_t, 2> expected{111, 112, 113};
        s.resize(3);
        ASSERT_EQ(expected, s);
    }

    // Shrink, inlined
    {
        InlinedStorage<size_t, 2> s{111, 112};
        InlinedStorage<size_t, 2> expected{111};
        s.resize(1);
        ASSERT_EQ(expected, s);
    }

    // Grow, inlined
    {
        InlinedStorage<size_t, 2> s{111};
        InlinedStorage<size_t, 2> expected{111, 0};
        s.resize(2);
        ASSERT_EQ(expected, s);
    }

    // Shrink, on heap
    {
        InlinedStorage<size_t, 2> s{111, 112, 113, 114};
        InlinedStorage<size_t, 2> expected{111, 112, 113};
        s.resize(3);
        ASSERT_EQ(expected, s);
    }

    // Grow, on heap
    {
        InlinedStorage<size_t, 2> s{111, 112, 113};
        InlinedStorage<size_t, 2> expected{111, 112, 113, 0};
        s.resize(4);
        ASSERT_EQ(expected, s);
    }

    // Shrink, on heap -> inlined
    {
        InlinedStorage<size_t, 2> s{111, 112, 113};
        InlinedStorage<size_t, 2> expected{111};
        s.resize(1);
        ASSERT_EQ(expected, s);
    }

    // Grow, inlined -> on heap
    {
        InlinedStorage<size_t, 2> s{111};
        InlinedStorage<size_t, 2> expected{111, 0, 0};
        s.resize(3);
        ASSERT_EQ(expected, s);
    }
}

TEST(InlinedStorageTests, Less) {
    using Storage = InlinedStorage<size_t, 2>;

    ASSERT_LT((Storage{111}), (Storage{111, 112}));
    ASSERT_LT((Storage{111}), (Storage{112}));
    ASSERT_LT((Storage{111}), (Storage{112}));
    ASSERT_LT((Storage{111}), (Storage({111, 1})));
}

template <typename Storage>
void assertMovedFrom(Storage& movedFrom) {
    // Moved from object is empty.
    ASSERT_EQ(0, movedFrom.size());

    // It still can be assigned.
    movedFrom = Storage{111, 112};
    ASSERT_EQ(2, movedFrom.size());
    ASSERT_EQ(111, movedFrom[0]);
    ASSERT_EQ(112, movedFrom[1]);
}

TEST(InlinedStorageTests, MovedFromObjects) {
    using Storage = InlinedStorage<size_t, 1>;

    Storage inlined{111};
    Storage heaped{111, 112};

    auto newInlined = std::move(inlined);
    auto newHeaped = std::move(heaped);

    ASSERT_EQ(1, newInlined.size());
    ASSERT_EQ(2, newHeaped.size());

    assertMovedFrom(inlined);
    assertMovedFrom(heaped);
}

}  // namespace mongo
