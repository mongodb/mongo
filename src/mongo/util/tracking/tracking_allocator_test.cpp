// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    EXPECT_EQ(0, Context.allocated());

    {
        std::vector<int64_t, Allocator<int64_t>> vec(Context.makeAllocator<int64_t>());
        EXPECT_EQ(0, Context.allocated());

        vec.push_back(1);
        EXPECT_EQ(sizeof(int64_t), Context.allocated());

        vec.push_back(2);
        vec.push_back(3);

        // Different compilers allocate capacity in different ways. Use shrink_to_fit() to reduce
        // capacity to the size.
        vec.shrink_to_fit();
        EXPECT_EQ(3 * sizeof(int64_t), Context.allocated());

        vec.pop_back();
        vec.shrink_to_fit();
        EXPECT_EQ(2 * sizeof(int64_t), Context.allocated());

        vec.clear();
        vec.shrink_to_fit();
        EXPECT_EQ(0, Context.allocated());
    }

    EXPECT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, STLContainerCopy) {
#if _ITERATOR_DEBUG_LEVEL >= 2
    // If iterator debugging is on, skip this test as additional memory gets allocated.
    return;
#endif

    Context Context;
    EXPECT_EQ(0, Context.allocated());

    {
        std::vector<int64_t, Allocator<int64_t>> vec(Context.makeAllocator<int64_t>());
        EXPECT_EQ(0, Context.allocated());

        vec.push_back(1);
        EXPECT_EQ(sizeof(int64_t), Context.allocated());

        std::vector<int64_t, Allocator<int64_t>> vecCopy = vec;
        vecCopy.shrink_to_fit();
        EXPECT_EQ(2 * sizeof(int64_t), Context.allocated());

        vecCopy.push_back(2);
        vecCopy.shrink_to_fit();
        EXPECT_EQ(3 * sizeof(int64_t), Context.allocated());
    }

    EXPECT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, STLContainerMove) {
#if _ITERATOR_DEBUG_LEVEL >= 2
    // If iterator debugging is on, skip this test as additional memory gets allocated.
    return;
#endif

    Context Context;
    EXPECT_EQ(0, Context.allocated());

    {
        std::vector<int64_t, Allocator<int64_t>> vec(Context.makeAllocator<int64_t>());
        EXPECT_EQ(0, Context.allocated());

        vec.push_back(1);
        EXPECT_EQ(sizeof(int64_t), Context.allocated());

        std::vector<int64_t, Allocator<int64_t>> vecMove = std::move(vec);
        vecMove.shrink_to_fit();
        EXPECT_EQ(sizeof(int64_t), Context.allocated());

        vecMove.push_back(2);
        vecMove.shrink_to_fit();
        EXPECT_EQ(2 * sizeof(int64_t), Context.allocated());
    }

    EXPECT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, STLContainerNested) {
#if _ITERATOR_DEBUG_LEVEL >= 2
    // If iterator debugging is on, skip this test as additional memory gets allocated.
    return;
#endif

    Context Context;
    EXPECT_EQ(0, Context.allocated());

    {
        // The scoped_allocator_adaptor is necessary for multilevel containers to use the same
        // allocator.
        using Key = int64_t;
        using Value = std::vector<int64_t, Allocator<int64_t>>;
        map<Key, Value> map(make_map<Key, Value>(Context));

        map[1].emplace_back(1);
        EXPECT_GT(Context.allocated(), sizeof(std::pair<const Key, Value>));

        // Adding elements into the vector results in the top-level allocators allocation count
        // being increased.
        uint64_t prevAllocated = Context.allocated();
        for (int i = 0; i < 100; i++) {
            map[1].push_back(i);
        }
        EXPECT_GT(Context.allocated(), prevAllocated);

        prevAllocated = Context.allocated();
        map[1].clear();
        map[1].shrink_to_fit();
        EXPECT_LT(Context.allocated(), prevAllocated);
    }

    EXPECT_EQ(0, Context.allocated());

    {
        using Key = string;
        using Value = std::vector<string>;
        map<Key, Value> map(make_map<Key, Value>(Context));

        // Use a long string to avoid small-string optimization which would have no allocation.
        const string str = make_string(
            Context,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        map[str].emplace_back(str);
        EXPECT_GT(Context.allocated(), sizeof(std::pair<const Key, Value>) + 2 * str.capacity());
    }

    EXPECT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, ManagedObject) {
    Context Context;
    EXPECT_EQ(0, Context.allocated());

    {
        shared_ptr<MockClass> mockClass = tracking::make_shared<MockClass>(Context);
        EXPECT_GE(Context.allocated(), sizeof(MockClass));
    }

    EXPECT_EQ(0, Context.allocated());

    {
        unique_ptr<MockClass> mockClass = tracking::make_unique<MockClass>(Context);
        EXPECT_GE(Context.allocated(), sizeof(MockClass));
    }

    EXPECT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, TrackOwnedObjectNull) {
    Context Context;
    EXPECT_EQ(0, Context.allocated());

    {
        unique_ptr<MockClass> mockClass = tracking::unique_ptr<MockClass>(Context, nullptr);
        EXPECT_EQ(0, Context.allocated());
    }

    EXPECT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, TrackOwnedObjectMoveConstruction) {
    Context Context;
    EXPECT_EQ(0, Context.allocated());

    {
        unique_ptr<MockClass> mockClass1 = tracking::make_unique<MockClass>(Context);
        auto allocatedMemory = Context.allocated();
        EXPECT_GE(allocatedMemory, sizeof(MockClass));

        // Allocation doesn't increase by moving.
        unique_ptr<MockClass> mockClass2(std::move(mockClass1));
        EXPECT_EQ(Context.allocated(), allocatedMemory);
    }

    EXPECT_EQ(0, Context.allocated());
}

TEST_F(AllocatorTest, TrackOwnedObjectMoveAssignment) {
    Context Context1;
    Context Context2;
    Context Context3;
    EXPECT_EQ(0, Context1.allocated());
    EXPECT_EQ(0, Context2.allocated());
    EXPECT_EQ(0, Context3.allocated());

    {
        unique_ptr<MockClass> mockClass1 = tracking::make_unique<MockClass>(Context1);
        unique_ptr<MockClass> mockClass2 = tracking::make_unique<MockClass>(Context2);
        EXPECT_GE(Context1.allocated(), sizeof(MockClass));
        auto allocatedMemory2 = Context2.allocated();
        EXPECT_GE(allocatedMemory2, sizeof(MockClass));

        // mockClass1 deallocates the original object and takes over mockClass2's allocator.
        mockClass1 = std::move(mockClass2);
        EXPECT_EQ(0, Context1.allocated());
        // Allocation doesn't increase by moving.
        EXPECT_EQ(Context2.allocated(), allocatedMemory2);

        // mockClass1 deallocates the object from mockClass2 and takes over mockClass3's allocator.
        unique_ptr<MockClass> mockClass3 = tracking::make_unique<MockClass>(Context3);
        auto allocatedMemory3 = Context3.allocated();
        EXPECT_GE(allocatedMemory3, sizeof(MockClass));
        mockClass1 = std::move(mockClass3);
        EXPECT_EQ(0, Context1.allocated());
        EXPECT_EQ(0, Context2.allocated());
        // Allocation doesn't increase by moving.
        EXPECT_EQ(Context3.allocated(), allocatedMemory3);
    }

    EXPECT_EQ(0, Context1.allocated());
    EXPECT_EQ(0, Context2.allocated());
    EXPECT_EQ(0, Context3.allocated());

    {
        unique_ptr<MockClass> mockClass1 = tracking::unique_ptr<MockClass>(Context1, nullptr);
        unique_ptr<MockClass> mockClass2 = tracking::make_unique<MockClass>(Context2);
        EXPECT_EQ(0, Context1.allocated());
        EXPECT_GE(Context2.allocated(), sizeof(MockClass));

        // mockClass1 deallocates the original object and takes over mockClass2's allocator.
        mockClass1 = std::move(mockClass2);
        EXPECT_EQ(0, Context1.allocated());
        EXPECT_GE(Context2.allocated(), sizeof(MockClass));
    }

    EXPECT_EQ(0, Context1.allocated());
    EXPECT_EQ(0, Context2.allocated());
    EXPECT_EQ(0, Context3.allocated());
}

TEST_F(AllocatorTest, TrackOwnedObjectRelease) {
    Context Context;
    EXPECT_EQ(0, Context.allocated());

    {
        unique_ptr<MockClass> mockClass = tracking::make_unique<MockClass>(Context);
        EXPECT_GE(Context.allocated(), sizeof(MockClass));

        MockClass* rawMockClass = mockClass.release();
        EXPECT_GE(Context.allocated(), sizeof(MockClass));

        delete rawMockClass;
    }

    // The memory is no longer tracked after the pointer is released.
    EXPECT_GE(Context.allocated(), sizeof(MockClass));
}
}  // namespace mongo::tracking
