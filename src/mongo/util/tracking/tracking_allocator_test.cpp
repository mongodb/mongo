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

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tracking/allocator.h"
#include "mongo/util/tracking/map.h"
#include "mongo/util/tracking/memory.h"
#include "mongo/util/tracking/string.h"

#include <map>
#include <vector>

namespace mongo::tracking {

class AllocatorTest : public unittest::Test {
protected:
    class MockClass {
    private:
        int64_t u;
        int64_t v;
        int64_t w;
        int64_t x;
        int64_t y;
        int64_t z;
    };
};

TEST_F(AllocatorTest, STLContainerSimple) {
#if _ITERATOR_DEBUG_LEVEL >= 2
    // If iterator debugging is on, skip this test as additional memory gets allocated.
    return;
#endif

    Context Context;
    ASSERT_EQ(0, Context.allocated());

    {
        std::vector<int64_t, Allocator<int64_t>> vec(Context.makeAllocator<int64_t>());
        ASSERT_EQ(0, Context.allocated());

        vec.push_back(1);
        ASSERT_EQ(sizeof(int64_t), Context.allocated());

        vec.push_back(2);
        vec.push_back(3);

        // Different compilers allocate capacity in different ways. Use shrink_to_fit() to reduce
        // capacity to the size.
        vec.shrink_to_fit();
        ASSERT_EQ(3 * sizeof(int64_t), Context.allocated());

        vec.pop_back();
        vec.shrink_to_fit();
        ASSERT_EQ(2 * sizeof(int64_t), Context.allocated());

        vec.clear();
        vec.shrink_to_fit();
        ASSERT_EQ(0, Context.allocated());
    }

    ASSERT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, STLContainerCopy) {
#if _ITERATOR_DEBUG_LEVEL >= 2
    // If iterator debugging is on, skip this test as additional memory gets allocated.
    return;
#endif

    Context Context;
    ASSERT_EQ(0, Context.allocated());

    {
        std::vector<int64_t, Allocator<int64_t>> vec(Context.makeAllocator<int64_t>());
        ASSERT_EQ(0, Context.allocated());

        vec.push_back(1);
        ASSERT_EQ(sizeof(int64_t), Context.allocated());

        std::vector<int64_t, Allocator<int64_t>> vecCopy = vec;
        vecCopy.shrink_to_fit();
        ASSERT_EQ(2 * sizeof(int64_t), Context.allocated());

        vecCopy.push_back(2);
        vecCopy.shrink_to_fit();
        ASSERT_EQ(3 * sizeof(int64_t), Context.allocated());
    }

    ASSERT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, STLContainerMove) {
#if _ITERATOR_DEBUG_LEVEL >= 2
    // If iterator debugging is on, skip this test as additional memory gets allocated.
    return;
#endif

    Context Context;
    ASSERT_EQ(0, Context.allocated());

    {
        std::vector<int64_t, Allocator<int64_t>> vec(Context.makeAllocator<int64_t>());
        ASSERT_EQ(0, Context.allocated());

        vec.push_back(1);
        ASSERT_EQ(sizeof(int64_t), Context.allocated());

        std::vector<int64_t, Allocator<int64_t>> vecMove = std::move(vec);
        vecMove.shrink_to_fit();
        ASSERT_EQ(sizeof(int64_t), Context.allocated());

        vecMove.push_back(2);
        vecMove.shrink_to_fit();
        ASSERT_EQ(2 * sizeof(int64_t), Context.allocated());
    }

    ASSERT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, STLContainerNested) {
#if _ITERATOR_DEBUG_LEVEL >= 2
    // If iterator debugging is on, skip this test as additional memory gets allocated.
    return;
#endif

    Context Context;
    ASSERT_EQ(0, Context.allocated());

    {
        // The scoped_allocator_adaptor is necessary for multilevel containers to use the same
        // allocator.
        using Key = int64_t;
        using Value = std::vector<int64_t, Allocator<int64_t>>;
        map<Key, Value> map(make_map<Key, Value>(Context));

        map[1].emplace_back(1);
        ASSERT_GT(Context.allocated(), sizeof(std::pair<const Key, Value>));

        // Adding elements into the vector results in the top-level allocators allocation count
        // being increased.
        uint64_t prevAllocated = Context.allocated();
        for (int i = 0; i < 100; i++) {
            map[1].push_back(i);
        }
        ASSERT_GT(Context.allocated(), prevAllocated);

        prevAllocated = Context.allocated();
        map[1].clear();
        map[1].shrink_to_fit();
        ASSERT_LT(Context.allocated(), prevAllocated);
    }

    ASSERT_EQ(0, Context.allocated());

    {
        using Key = string;
        using Value = std::vector<string>;
        map<Key, Value> map(make_map<Key, Value>(Context));

        // Use a long string to avoid small-string optimization which would have no allocation.
        const string str = make_string(
            Context,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        map[str].emplace_back(str);
        ASSERT_GT(Context.allocated(), sizeof(std::pair<const Key, Value>) + 2 * str.capacity());
    }

    ASSERT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, ManagedObject) {
    Context Context;
    ASSERT_EQ(0, Context.allocated());

    {
        shared_ptr<MockClass> mockClass = tracking::make_shared<MockClass>(Context);
        ASSERT_GTE(Context.allocated(), sizeof(MockClass));
    }

    ASSERT_EQ(0, Context.allocated());

    {
        unique_ptr<MockClass> mockClass = tracking::make_unique<MockClass>(Context);
        ASSERT_GTE(Context.allocated(), sizeof(MockClass));
    }

    ASSERT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, TrackOwnedObjectNull) {
    Context Context;
    ASSERT_EQ(0, Context.allocated());

    {
        unique_ptr<MockClass> mockClass = tracking::unique_ptr<MockClass>(Context, nullptr);
        ASSERT_EQ(0, Context.allocated());
    }

    ASSERT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, TrackOwnedObjectMoveConstruction) {
    Context Context;
    ASSERT_EQ(0, Context.allocated());

    {
        unique_ptr<MockClass> mockClass1 = tracking::make_unique<MockClass>(Context);
        auto allocatedMemory = Context.allocated();
        ASSERT_GTE(allocatedMemory, sizeof(MockClass));

        // Allocation doesn't increase by moving.
        unique_ptr<MockClass> mockClass2(std::move(mockClass1));
        ASSERT_EQ(Context.allocated(), allocatedMemory);
    }

    ASSERT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, TrackOwnedObjectMoveAssignment) {
    Context Context1;
    Context Context2;
    Context Context3;
    ASSERT_EQ(0, Context1.allocated());
    ASSERT_EQ(0, Context2.allocated());
    ASSERT_EQ(0, Context3.allocated());

    {
        unique_ptr<MockClass> mockClass1 = tracking::make_unique<MockClass>(Context1);
        unique_ptr<MockClass> mockClass2 = tracking::make_unique<MockClass>(Context2);
        ASSERT_GTE(Context1.allocated(), sizeof(MockClass));
        auto allocatedMemory2 = Context2.allocated();
        ASSERT_GTE(allocatedMemory2, sizeof(MockClass));

        // mockClass1 deallocates the original object and takes over mockClass2's allocator.
        mockClass1 = std::move(mockClass2);
        ASSERT_EQ(0, Context1.allocated());
        // Allocation doesn't increase by moving.
        ASSERT_EQ(Context2.allocated(), allocatedMemory2);

        // mockClass1 deallocates the object from mockClass2 and takes over mockClass3's allocator.
        unique_ptr<MockClass> mockClass3 = tracking::make_unique<MockClass>(Context3);
        auto allocatedMemory3 = Context3.allocated();
        ASSERT_GTE(allocatedMemory3, sizeof(MockClass));
        mockClass1 = std::move(mockClass3);
        ASSERT_EQ(0, Context1.allocated());
        ASSERT_EQ(0, Context2.allocated());
        // Allocation doesn't increase by moving.
        ASSERT_EQ(Context3.allocated(), allocatedMemory3);
    }

    ASSERT_EQ(0, Context1.allocated());
    ASSERT_EQ(0, Context2.allocated());
    ASSERT_EQ(0, Context3.allocated());

    {
        unique_ptr<MockClass> mockClass1 = tracking::unique_ptr<MockClass>(Context1, nullptr);
        unique_ptr<MockClass> mockClass2 = tracking::make_unique<MockClass>(Context2);
        ASSERT_EQ(0, Context1.allocated());
        ASSERT_GTE(Context2.allocated(), sizeof(MockClass));

        // mockClass1 deallocates the original object and takes over mockClass2's allocator.
        mockClass1 = std::move(mockClass2);
        ASSERT_EQ(0, Context1.allocated());
        ASSERT_GTE(Context2.allocated(), sizeof(MockClass));
    }

    ASSERT_EQ(0, Context1.allocated());
    ASSERT_EQ(0, Context2.allocated());
    ASSERT_EQ(0, Context3.allocated());
}

TEST_F(AllocatorTest, TrackOwnedObjectRelease) {
    Context Context;
    ASSERT_EQ(0, Context.allocated());

    {
        unique_ptr<MockClass> mockClass = tracking::make_unique<MockClass>(Context);
        ASSERT_GTE(Context.allocated(), sizeof(MockClass));

        MockClass* rawMockClass = mockClass.release();
        ASSERT_GTE(Context.allocated(), sizeof(MockClass));

        delete rawMockClass;
    }

    // The memory is no longer tracked after the pointer is released.
    ASSERT_GTE(Context.allocated(), sizeof(MockClass));
}
}  // namespace mongo::tracking
