/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/lock_manager/fast_map_noalloc.h"

#include "mongo/base/string_data.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
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
