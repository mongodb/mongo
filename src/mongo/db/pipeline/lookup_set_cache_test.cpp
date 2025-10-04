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

#include "mongo/db/pipeline/lookup_set_cache.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <vector>

namespace mongo {

bool vectorContains(const std::vector<Document>* vector, const Document& expectedDoc) {
    ASSERT_TRUE(vector);
    DocumentComparator comparator;
    return std::find_if(
               vector->begin(), vector->end(), [&comparator, &expectedDoc](const Document& doc) {
                   return comparator.evaluate(expectedDoc == doc);
               }) != vector->end();
}

Document intToDoc(int value) {
    return Document{{"n", value}};
}

const ValueComparator defaultComparator{nullptr};

TEST(LookupSetCacheTest, InsertAndRetrieveWorksCorrectly) {
    LookupSetCache cache(defaultComparator);
    cache.insert(Value(0), intToDoc(1));
    cache.insert(Value(0), intToDoc(2));
    cache.insert(Value(0), intToDoc(3));
    cache.insert(Value(1), intToDoc(4));
    cache.insert(Value(1), intToDoc(5));

    ASSERT(cache[Value(0)]);
    ASSERT_TRUE(vectorContains(cache[Value(0)], intToDoc(1)));
    ASSERT_TRUE(vectorContains(cache[Value(0)], intToDoc(2)));
    ASSERT_TRUE(vectorContains(cache[Value(0)], intToDoc(3)));
    ASSERT_FALSE(vectorContains(cache[Value(0)], intToDoc(4)));
    ASSERT_FALSE(vectorContains(cache[Value(0)], intToDoc(5)));
}

TEST(LookupSetCacheTest, CacheDoesEvictInExpectedOrder) {
    LookupSetCache cache(defaultComparator);

    cache.insert(Value(0), intToDoc(0));
    cache.insert(Value(1), intToDoc(0));
    cache.insert(Value(2), intToDoc(0));
    cache.insert(Value(3), intToDoc(0));

    // Cache ordering is {1: ..., 3: ..., 2: ..., 0: ...}.
    cache.evictOne();
    ASSERT_FALSE(cache[Value(0)]);
    cache.evictOne();
    ASSERT_FALSE(cache[Value(2)]);
    cache.evictOne();
    ASSERT_FALSE(cache[Value(3)]);
    cache.evictOne();
    ASSERT_FALSE(cache[Value(1)]);
}

TEST(LookupSetCacheTest, ReadDoesMoveKeyToFrontOfCache) {
    LookupSetCache cache(defaultComparator);

    cache.insert(Value(0), intToDoc(0));
    cache.insert(Value(1), intToDoc(0));
    // Cache ordering is now {1: [1], 0: [0]}.

    ASSERT_TRUE(cache[Value(0)]);
    // Cache ordering is now {0: [0], 1: [1]}.

    cache.evictOne();
    ASSERT_TRUE(cache[Value(0)]);
    ASSERT_FALSE(cache[Value(1)]);
}

TEST(LookupSetCacheTest, InsertDoesPutKeyInMiddle) {
    LookupSetCache cache(defaultComparator);

    cache.insert(Value(0), intToDoc(0));
    cache.insert(Value(1), intToDoc(0));
    cache.insert(Value(2), intToDoc(0));
    cache.insert(Value(3), intToDoc(0));

    cache.evictUntilSize(1);

    ASSERT_TRUE(cache[Value(1)]);
}

TEST(LookupSetCacheTest, EvictDoesRespectMemoryUsage) {
    LookupSetCache cache(defaultComparator);

    cache.insert(Value(0), intToDoc(0));
    cache.insert(Value(1), intToDoc(0));

    // One size_t for the key, one for the value.
    cache.evictDownTo(Value(1).getApproximateSize() + intToDoc(0).getApproximateSize());

    ASSERT_TRUE(cache[Value(1)]);
    ASSERT_FALSE(cache[Value(0)]);
}

TEST(LookupSetCacheTest, ComplexAccessPatternDoesBehaveCorrectly) {
    LookupSetCache cache(defaultComparator);

    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            cache.insert(Value(j), intToDoc(i));
        }
    }

    // Cache ordering is now {0: ..., 3: ..., 4: ..., 2: ..., 1: ...}
    cache.evictOne();
    ASSERT_FALSE(cache[Value(0)]);

    ASSERT_TRUE(cache[Value(1)]);
    ASSERT_TRUE(vectorContains(cache[Value(1)], intToDoc(0)));
    ASSERT_TRUE(vectorContains(cache[Value(1)], intToDoc(1)));
    ASSERT_TRUE(vectorContains(cache[Value(1)], intToDoc(2)));
    ASSERT_TRUE(vectorContains(cache[Value(1)], intToDoc(3)));
    ASSERT_TRUE(vectorContains(cache[Value(1)], intToDoc(4)));

    cache.evictUntilSize(2);
    // Cache ordering is now {1: ..., 3: ...}

    ASSERT_TRUE(cache[Value(1)]);
    ASSERT_TRUE(cache[Value(3)]);
    // Cache ordering is now {3: ..., 1: ...}

    cache.evictOne();
    ASSERT_FALSE(cache[Value(1)]);

    cache.insert(Value(5), intToDoc(0));
    cache.evictDownTo(Value(5).getApproximateSize() + intToDoc(0).getApproximateSize());

    ASSERT_EQ(cache.size(), 1U);
    ASSERT_TRUE(cache[Value(5)]);
}

TEST(LookupSetCacheTest, CacheKeysRespectCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    ValueComparator comparator{&collator};
    LookupSetCache cache(comparator);

    cache.insert(Value("foo"_sd), intToDoc(1));
    cache.insert(Value("FOO"_sd), intToDoc(2));
    cache.insert(Value("FOOz"_sd), intToDoc(3));

    {
        auto fooResult = cache[Value("FoO"_sd)];
        ASSERT_TRUE(fooResult);
        ASSERT_EQ(2U, fooResult->size());
    }

    {
        auto foozResult = cache[Value("fooZ"_sd)];
        ASSERT_TRUE(foozResult);
        ASSERT_EQ(1U, foozResult->size());
    }
}

// Cache values shouldn't respect collation, since they are distinct documents from the
// foreign collection.
TEST(LookupSetCacheTest, CachedValuesDontRespectCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    ValueComparator comparator{&collator};
    LookupSetCache cache(comparator);

    cache.insert(Value("foo"_sd), Document{{"foo", "bar"_sd}});
    cache.insert(Value("foo"_sd), Document{{"foo", "BAR"_sd}});

    auto fooResult = cache[Value("foo"_sd)];
    ASSERT_TRUE(fooResult);
    ASSERT_EQ(2U, fooResult->size());
}

TEST(LookupSetCacheTest, DocumentWithStorageCachePopulated) {
    LookupSetCache cache(defaultComparator);

    // Use a BSONObj backed Document so that the Document's hot storage isn't populated at
    // initialization.
    BSONObj input = BSON("a" << 1);
    const auto doc1 = Document(input);
    const auto sizeOfDoc1Before = doc1.getCurrentApproximateSize();
    auto key = Value("foo"_sd);

    // Insert a cache entry and verify that both the key and the document are accounted for in the
    // cache size.
    cache.insert(key, doc1);
    ASSERT_EQ(cache.getMemoryUsage(), sizeOfDoc1Before + key.getApproximateSize());


    // A second document with the same key should require additional memory usage to store the
    // document, but not the key.
    auto prevCacheSize = cache.getMemoryUsage();
    const auto doc2 = Document({{"a", 2}});
    cache.insert(key, doc2);
    ASSERT_EQ(cache.getMemoryUsage(), prevCacheSize + doc2.getCurrentApproximateSize());

    // Calling serializeForSorter() should grow the overall document size. Verify that growing the
    // size of the 'Document' object does not have impact on the size stored in 'cache'.
    prevCacheSize = cache.getMemoryUsage();
    BufBuilder builder;
    doc1.serializeForSorter(builder);
    ASSERT_LT(sizeOfDoc1Before, doc1.getCurrentApproximateSize());
    ASSERT_EQ(prevCacheSize, cache.getMemoryUsage());

    cache.evictOne();
    ASSERT_EQ(cache.getMemoryUsage(), 0);
}

}  // namespace mongo
