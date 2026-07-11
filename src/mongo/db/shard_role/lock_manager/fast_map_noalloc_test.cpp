// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/fast_map_noalloc.h"

#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <absl/container/node_hash_map.h>


namespace mongo {

struct TestStruct {
    void initNew(int newId, const std::string& newValue) {
        id = newId;
        value = newValue;
    }

    int id;
    std::string value;
};

typedef class FastMapNoAlloc<ResourceId, TestStruct> TestFastMapNoAlloc;


TEST(FastMapNoAlloc, Empty) {
    TestFastMapNoAlloc map;
    ASSERT(map.empty());

    TestFastMapNoAlloc::Iterator it = map.begin();
    ASSERT(it.finished());
}

TEST(FastMapNoAlloc, NotEmpty) {
    TestFastMapNoAlloc map;

    map.insert(ResourceId(RESOURCE_COLLECTION, 1))->initNew(101, "Item101");
    map.insert(ResourceId(RESOURCE_COLLECTION, 2))->initNew(102, "Item102");
    ASSERT(!map.empty());

    TestFastMapNoAlloc::Iterator it = map.begin();
    ASSERT(!it.finished());
    ASSERT(!!it);

    ASSERT(it->id == 102);
    ASSERT(it->value == "Item102");

    it.next();
    ASSERT(!it.finished());
    ASSERT(!!it);

    ASSERT(it->id == 101);
    ASSERT(it->value == "Item101");

    // We are at the last element
    it.next();
    ASSERT(it.finished());
    ASSERT(!it);
}

TEST(FastMapNoAlloc, FindNonExisting) {
    TestFastMapNoAlloc map;

    ASSERT(!map.find(ResourceId(RESOURCE_COLLECTION, 0)));
}

TEST(FastMapNoAlloc, FindAndRemove) {
    TestFastMapNoAlloc map;

    for (int i = 0; i < 6; i++) {
        map.insert(ResourceId(RESOURCE_COLLECTION, i))->initNew(i, "Item" + std::to_string(i));
    }

    for (int i = 0; i < 6; i++) {
        ASSERT(!map.find(ResourceId(RESOURCE_COLLECTION, i)).finished());

        ASSERT_EQUALS(i, map.find(ResourceId(RESOURCE_COLLECTION, i))->id);

        ASSERT_EQUALS("Item" + std::to_string(i),
                      map.find(ResourceId(RESOURCE_COLLECTION, i))->value);
    }

    // Remove a middle entry
    map.find(ResourceId(RESOURCE_COLLECTION, 2)).remove();
    ASSERT(!map.find(ResourceId(RESOURCE_COLLECTION, 2)));

    // Remove entry after first
    map.find(ResourceId(RESOURCE_COLLECTION, 1)).remove();
    ASSERT(!map.find(ResourceId(RESOURCE_COLLECTION, 1)));

    // Remove entry before last
    map.find(ResourceId(RESOURCE_COLLECTION, 4)).remove();
    ASSERT(!map.find(ResourceId(RESOURCE_COLLECTION, 4)));

    // Remove first entry
    map.find(ResourceId(RESOURCE_COLLECTION, 0)).remove();
    ASSERT(!map.find(ResourceId(RESOURCE_COLLECTION, 0)));

    // Remove last entry
    map.find(ResourceId(RESOURCE_COLLECTION, 5)).remove();
    ASSERT(!map.find(ResourceId(RESOURCE_COLLECTION, 5)));

    // Remove final entry
    map.find(ResourceId(RESOURCE_COLLECTION, 3)).remove();
    ASSERT(!map.find(ResourceId(RESOURCE_COLLECTION, 3)));
}

TEST(FastMapNoAlloc, RemoveAll) {
    TestFastMapNoAlloc map;
    stdx::unordered_map<ResourceId, TestStruct> checkMap;

    for (int i = 1; i <= 6; i++) {
        map.insert(ResourceId(RESOURCE_COLLECTION, i))->initNew(i, "Item" + std::to_string(i));

        checkMap[ResourceId(RESOURCE_COLLECTION, i)].initNew(i, "Item" + std::to_string(i));
    }

    TestFastMapNoAlloc::Iterator it = map.begin();
    while (!it.finished()) {
        ASSERT_EQUALS(it->id, checkMap[it.key()].id);
        ASSERT_EQUALS("Item" + std::to_string(it->id), checkMap[it.key()].value);

        checkMap.erase(it.key());
        it.remove();
    }

    ASSERT(map.empty());
    ASSERT(checkMap.empty());
}

}  // namespace mongo
