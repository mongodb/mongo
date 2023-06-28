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

#include <map>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>

#include <absl/container/node_hash_map.h>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {
/**
 * Asserts that 'obj' can be successfully inserted into 'set'.
 */
template <typename Set>
void assertInsertSucceeds(Set& set, BSONObj obj) {
    ASSERT(set.insert(obj).second) << "failed to insert object: " << obj.jsonString();
}

/**
 * Asserts that 'obj' fails to be inserted into 'set'.
 */
template <typename Set>
void assertInsertFails(Set& set, BSONObj obj) {
    ASSERT_FALSE(set.insert(obj).second)
        << "object was inserted successfully, but should have failed: " << obj.jsonString();
}

TEST(SimpleBSONObjContainerTest, SetIsDefaultConstructible) {
    SimpleBSONObjSet set;
    assertInsertSucceeds(set, BSON("x" << 1));
    ASSERT_EQ(set.size(), 1UL);
    assertInsertSucceeds(set, BSON("y" << 1));
    assertInsertFails(set, BSON("x" << 1));
    ASSERT_EQ(set.size(), 2UL);
}

TEST(SimpleBSONObjContainerTest, MultiSetIsDefaultConstructible) {
    SimpleBSONObjMultiSet multiset;
    multiset.insert(BSON("x" << 1));
    multiset.insert(BSON("x" << 1));
    multiset.insert(BSON("y" << 1));
    ASSERT_EQ(multiset.size(), 3UL);
}

TEST(SimpleBSONObjContainerTest, UnorderedSetIsDefaultConstructible) {
    SimpleBSONObjUnorderedSet uset;
    assertInsertSucceeds(uset, BSON("x" << 1));
    ASSERT_EQ(uset.size(), 1UL);
    assertInsertSucceeds(uset, BSON("y" << 1));
    assertInsertFails(uset, BSON("x" << 1));
    ASSERT_EQ(uset.size(), 2UL);
}

/**
 * Asserts that the key-value pair 'pair' can be successfully inserted into 'map'.
 */
template <typename Map, typename T>
void assertInsertSucceeds(Map& map, std::pair<BSONObj, T> pair) {
    ASSERT(map.insert(pair).second) << "failed to insert {key: " << pair.first.jsonString()
                                    << ", value: '" << pair.second << "'}";
}

/**
 * Asserts that the key-value pair 'pair' fails to be inserted into 'map'.
 */
template <typename Map, typename T>
void assertInsertFails(Map& map, std::pair<BSONObj, T> pair) {
    ASSERT_FALSE(map.insert(pair).second)
        << "key-value pair was inserted successfully, but should have failed: {key: "
        << pair.first.jsonString() << ", value: '" << pair.second << "'}";
}

TEST(SimpleBSONObjContainerTest, MapIsDefaultConstructible) {
    SimpleBSONObjMap<std::string> map;
    assertInsertSucceeds(map, std::make_pair(BSON("_id" << 0), "kyle"));
    ASSERT_EQ(map.size(), 1UL);
    assertInsertSucceeds(map, std::make_pair(BSON("_id" << 1), "jungsoo"));
    ASSERT_EQ(map.size(), 2UL);
    assertInsertFails(map, std::make_pair(BSON("_id" << 1), "erjon"));
    ASSERT_EQ(map.size(), 2UL);
}

TEST(SimpleBSONObjContainerTest, MultiMapIsDefaultConstructible) {
    SimpleBSONObjMultiMap<std::string> multimap;
    multimap.insert(std::make_pair(BSON("_id" << 0), "anica"));
    multimap.insert(std::make_pair(BSON("_id" << 0), "raj"));
    multimap.insert(std::make_pair(BSON("_id" << 1), "ian"));
    ASSERT_EQ(multimap.size(), 3UL);
}

TEST(SimpleBSONObjContainerTest, UnorderedMapIsDefaultConstructible) {
    SimpleBSONObjUnorderedMap<std::string> umap;
    assertInsertSucceeds(umap, std::make_pair(BSON("_id" << 0), "kyle"));
    ASSERT_EQ(umap.size(), 1UL);
    assertInsertSucceeds(umap, std::make_pair(BSON("_id" << 1), "jungsoo"));
    ASSERT_EQ(umap.size(), 2UL);
    assertInsertFails(umap, std::make_pair(BSON("_id" << 1), "erjon"));
    ASSERT_EQ(umap.size(), 2UL);
}

}  // namespace
}  // namespace mongo
