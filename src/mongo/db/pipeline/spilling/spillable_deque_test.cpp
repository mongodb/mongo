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

#include "mongo/db/pipeline/spilling/spillable_deque.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/spilling/spilling_test_fixture.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
static constexpr bool isSingleElementSortKey = true;
static const Value sortKeyVal = Value(1.0);

class SpillableDequeTest : public SpillingTestFixture {
public:
    std::unique_ptr<SpillableDeque> createSpillableDeque(size_t maxMem) {
        _tracker = std::make_unique<MemoryUsageTracker>(false, maxMem);
        auto cache = std::make_unique<SpillableDeque>(_expCtx.get(), _tracker.get());
        return cache;
    }

    void buildAndLoadDocumentSet(int numDocs, SpillableDeque* cache) {
        for (int i = _lastIndex; i < _lastIndex + numDocs; ++i) {
            // Make sure we can load metadata to/from the store.
            _docSet.emplace_back(addMetadata(Document{{"val", i}}));
            cache->addDocument(_docSet.back());
        }
    }

    void verifyDocsInCache(int start, int end, SpillableDeque* cache) {
        for (int i = start; i < end; ++i) {
            auto foundDoc = cache->getDocumentById(i);
            ASSERT_DOCUMENT_EQ(foundDoc, _docSet[i]);
            assertMetaIsStillPresent(foundDoc);
        }
    }

    Document addMetadata(Document&& mockInput) const {
        MutableDocument doc(std::move(mockInput));
        doc.metadata().setSortKey(sortKeyVal, isSingleElementSortKey);
        std::cout << doc.peek().memUsageForSorter() << std::endl;
        return doc.freeze();
    }

    void assertMetaIsStillPresent(const Document& foundDoc) const {
        ASSERT_VALUE_EQ(foundDoc.metadata().getSortKey(), sortKeyVal);
    }

    std::unique_ptr<MemoryUsageTracker> _tracker;

    // Docs are ~500 each.
    std::vector<Document> _docSet;
    int _lastIndex = 0;
};

TEST_F(SpillableDequeTest, CanReadAndWriteDocumentsInMem) {
    _expCtx->setAllowDiskUse(false);
    auto cache = createSpillableDeque(2000);
    buildAndLoadDocumentSet(2, cache.get());
    verifyDocsInCache(0, 2, cache.get());
}

TEST_F(SpillableDequeTest, LoadingFailsIfCantSpillToDisk) {
    _expCtx->setAllowDiskUse(false);
    auto cache = createSpillableDeque(1);
    ASSERT_THROWS_CODE(buildAndLoadDocumentSet(1, cache.get()), AssertionException, 5643011);
}

TEST_F(SpillableDequeTest, CanReadAndWriteDocumentsToDisk) {
    _expCtx->setAllowDiskUse(true);
    auto cache = createSpillableDeque(1);
    buildAndLoadDocumentSet(3, cache.get());
    verifyDocsInCache(0, 3, cache.get());
    _expCtx->setAllowDiskUse(false);
}

TEST_F(SpillableDequeTest, CanReturnDocumentsFromCacheAndDisk) {
    _expCtx->setAllowDiskUse(true);
    // Docs are ~500 each.
    auto cache = createSpillableDeque(700);
    buildAndLoadDocumentSet(3, cache.get());
    verifyDocsInCache(0, 3, cache.get());
    _expCtx->setAllowDiskUse(false);
    cache->finalize();
}

DEATH_TEST_F(SpillableDequeTest, RemovesDocumentsWhenExpired, "Requested expired document") {
    _expCtx->setAllowDiskUse(false);
    auto cache = createSpillableDeque(2000);
    buildAndLoadDocumentSet(4, cache.get());
    cache->freeUpTo(0);
    verifyDocsInCache(1, 4, cache.get());
    ASSERT_THROWS_CODE(cache->getDocumentById(0), AssertionException, 5643005);
}

TEST_F(SpillableDequeTest, ReturnsCorrectDocumentsIfSomeHaveBeenRemovedMemOnly) {
    _expCtx->setAllowDiskUse(false);
    auto cache = createSpillableDeque(5000);
    buildAndLoadDocumentSet(10, cache.get());
    cache->freeUpTo(4);
    verifyDocsInCache(5, 10, cache.get());
}

TEST_F(SpillableDequeTest, ReturnsCorrectDocumentsIfSomeHaveBeenRemovedMixed) {
    _expCtx->setAllowDiskUse(true);
    auto cache = createSpillableDeque(2000);
    buildAndLoadDocumentSet(10, cache.get());
    // Only mark documents on disk as freed.
    cache->freeUpTo(4);
    // Force spilling again.
    buildAndLoadDocumentSet(5, cache.get());
    verifyDocsInCache(5, 15, cache.get());
    cache->finalize();
    _expCtx->setAllowDiskUse(false);
}

TEST_F(SpillableDequeTest, CanInsertLargeDocuments) {
    _expCtx->setAllowDiskUse(true);
    // 19 MB
    auto cache = createSpillableDeque(19 * 1024 * 1024);
    // 1 MB string
    auto str = std::string(1024 * 1024, 'x');
    for (int i = 0; i < 20; ++i) {
        cache->addDocument(Document(BSON("_id" << i << "longStr" << str)));
    }
    // We've loaded 20 MB, so we must have spilled successfully. Check that we can get them back.
    for (int i = 0; i < 20; ++i) {
        auto doc = cache->getDocumentById(i);
        ASSERT_EQ(doc["longStr"].getString(), str);
        ASSERT_EQ(doc["_id"].getInt(), i);
    }
    cache->finalize();
    _expCtx->setAllowDiskUse(false);
}

}  // namespace
}  // namespace mongo
