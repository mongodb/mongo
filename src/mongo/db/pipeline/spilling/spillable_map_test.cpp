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

#include "mongo/db/pipeline/spilling/spillable_map.h"

#include "mongo/db/pipeline/spilling/spilling_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <absl/container/flat_hash_set.h>

namespace mongo {
namespace {

void assertSpilledBytes(long long spilledBytes, long long totalAddedSize) {
    // Spilled bytes are calculated as a sum of written BSONObj's and totalAddedSize is a sum of
    // Document::getApproximateSize(), so we compare them with 20% relative error.
    auto relativeError = totalAddedSize / 5;
    ASSERT_APPROX_EQUAL(spilledBytes, totalAddedSize, relativeError);
}

class SpillableDocumentHashMapTest : public SpillingTestFixture {
public:
    SpillableDocumentMap createSpillableMap(size_t maxMemory = 100 * 1024 * 1024) {
        _tracker.emplace(true /*allowDiskUse*/, maxMemory);
        return SpillableDocumentMap{_expCtx.get(), &_tracker.get()};
    }

    std::vector<Document> generateDocuments(int64_t docCount,
                                            int64_t payloadSize,
                                            int64_t startId = 0) {
        PseudoRandom random{docCount * 31 * 31 + payloadSize * 31 + startId};
        auto nextRandomAlpha = [&]() {
            return static_cast<char>('a' + random.nextInt32() % 26);
        };

        std::vector<Document> docs;
        docs.reserve(docCount);
        for (int64_t id = startId; id < startId + docCount; ++id) {
            std::string payload;
            payload.reserve(payloadSize);
            for (int64_t i = id; i < payloadSize; ++i) {
                payload.push_back(nextRandomAlpha());
            }
            docs.emplace_back(BSON("_id" << id << "payload" << payload));
        }
        return docs;
    }

    void verifyDocsInMap(const SpillableDocumentMap& map, const std::vector<Document>& docs) {
        verifyUsingIterator(map, docs);
        verifyUsingContains(map, docs);
    }

private:
    void verifyUsingIterator(const SpillableDocumentMap& map, const std::vector<Document>& docs) {
        auto unseenDocs = _expCtx->getValueComparator().makeFlatUnorderedValueSet();
        unseenDocs.insert(docs.begin(), docs.end());
        for (const auto& doc : map) {
            Value value{doc};
            auto it = unseenDocs.find(value);
            ASSERT_NE(it, unseenDocs.end())
                << "Document " << doc.toString() << " is found in map but not expected";
            unseenDocs.erase(it);
        }
        ASSERT_TRUE(unseenDocs.empty()) << "Documents not found in map but expected. Example: "
                                        << unseenDocs.begin()->toString();
    }

    void verifyUsingContains(const SpillableDocumentMap& map, const std::vector<Document>& docs) {
        ASSERT_EQ(docs.size(), map.size());
        for (const auto& doc : docs) {
            auto id = doc.getField("_id").coerceToLong();
            ASSERT_TRUE(map.contains(Value{id}));
            if (id != -id) {
                ASSERT_FALSE(map.contains(Value{-id}));
            }
        }
    }

    boost::optional<MemoryUsageTracker> _tracker;
};

TEST_F(SpillableDocumentHashMapTest, CanReadAndWriteDocumentsInMemory) {
    _expCtx->setAllowDiskUse(false);
    auto map = createSpillableMap();
    std::vector<Document> docs = generateDocuments(1024, 1024);
    for (const auto& doc : docs) {
        map.add(doc);
    }
    verifyDocsInMap(map, docs);
}

TEST_F(SpillableDocumentHashMapTest, AddingFailsIfCantSpillToDisk) {
    _expCtx->setAllowDiskUse(false);
    auto map = createSpillableMap(100 * 1024);
    std::vector<Document> docs = generateDocuments(1024, 1024);
    auto insert = [&]() {
        for (const auto& doc : docs) {
            map.add(doc);
        }
    };
    ASSERT_THROWS_CODE(
        insert(), AssertionException, ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

TEST_F(SpillableDocumentHashMapTest, CanReadAndWriteDocumentsOnDisk) {
    _expCtx->setAllowDiskUse(true);
    auto map = createSpillableMap(100 * 1024);
    std::vector<Document> docs = generateDocuments(1024, 1024);
    for (const auto& doc : docs) {
        map.add(doc);
    }
    verifyDocsInMap(map, docs);
}

TEST_F(SpillableDocumentHashMapTest, CanSpillManually) {
    _expCtx->setAllowDiskUse(true);
    auto map = createSpillableMap(100 * 1024);
    std::vector<Document> docs = generateDocuments(1024, 1024);
    for (size_t i = 0; i < docs.size(); ++i) {
        map.add(docs[i]);
        if (i % 4 == 0) {
            map.spillToDisk();
        }
    }
    verifyDocsInMap(map, docs);
}


TEST_F(SpillableDocumentHashMapTest, CanMixReadsWritesAndSpills) {
    _expCtx->setAllowDiskUse(true);
    auto map = createSpillableMap(100 * 1024);
    std::vector<Document> docs = generateDocuments(1024, 1024);
    std::vector<Document> docsInMap;
    for (size_t i = 0; i < docs.size(); ++i) {
        docsInMap.push_back(docs[i]);
        map.add(docs[i]);
        if (i % 7 == 0) {
            map.spillToDisk();
        }
        if (i % 11 == 0) {
            verifyDocsInMap(map, docsInMap);
        }
    }
    verifyDocsInMap(map, docs);
}

TEST_F(SpillableDocumentHashMapTest, CanAddAfterClear) {
    _expCtx->setAllowDiskUse(true);
    auto map = createSpillableMap(100 * 1024);
    std::vector<Document> docs = generateDocuments(1024, 1024);
    for (int loop = 0; loop < 2; ++loop) {
        ASSERT_TRUE(map.empty()) << "loop index " << loop;

        for (size_t i = 0; i < docs.size(); ++i) {
            map.add(docs[i]);
        }
        verifyDocsInMap(map, docs);

        map.clear();
    }
}

TEST_F(SpillableDocumentHashMapTest, ReportsMemoryUsageAndSpillingStats) {
    _expCtx->setAllowDiskUse(true);
    auto map = createSpillableMap(100 * 1024);
    size_t totalAddedDocSize = 0;
    for (const auto& doc : generateDocuments(10, 1024)) {
        map.add(doc);
        totalAddedDocSize += doc.getApproximateSize();
        ASSERT_GTE(map.getApproximateSize(), totalAddedDocSize);
    }

    ASSERT_EQ(map.getSpillingStats().getSpills(), 0);
    ASSERT_EQ(map.getSpillingStats().getSpilledBytes(), 0);
    ASSERT_EQ(map.getSpillingStats().getSpilledDataStorageSize(), 0);

    map.spillToDisk();
    ASSERT_LT(map.getApproximateSize(), totalAddedDocSize);

    ASSERT_EQ(map.getSpillingStats().getSpills(), 1);
    assertSpilledBytes(map.getSpillingStats().getSpilledBytes(), totalAddedDocSize);

    totalAddedDocSize = 0;
    for (const auto& doc : generateDocuments(10, 1024, 10)) {
        map.add(doc);
        totalAddedDocSize += doc.getApproximateSize();
        ASSERT_GTE(map.getApproximateSize(), totalAddedDocSize);
    }
}

TEST_F(SpillableDocumentHashMapTest, CanSpillIterator) {
    _expCtx->setAllowDiskUse(true);
    auto map = createSpillableMap(100 * 1024);
    absl::flat_hash_set<long long> generatedIds;
    for (const auto& doc : generateDocuments(20, 1024)) {
        map.add(doc);
        auto id = doc.getField("_id").coerceToLong();
        generatedIds.insert(id);
        if (map.size() == 10) {
            map.spillToDisk();
        }
    }

    size_t initialSize = map.getApproximateSize();

    absl::flat_hash_set<long long> iteratedIds;
    auto it = map.begin();
    // First iterator reads from memory without affecting approximate size
    for (int i = 0; i < 10; ++i) {
        iteratedIds.insert(it->getField("_id").coerceToLong());
        ASSERT_EQ(map.getApproximateSize(), initialSize);
        ++it;
    }

    // Now iterator should have a buffer of documents from disk
    size_t sizeWithFullBuffer = map.getApproximateSize();
    ASSERT_GT(sizeWithFullBuffer, initialSize);

    // Spilling should free up memory
    it.releaseMemory();
    ASSERT_LT(map.getApproximateSize(), sizeWithFullBuffer);

    // Spilling should not invalidate the iterator
    for (int i = 0; i < 10; ++i) {
        iteratedIds.insert(it->getField("_id").coerceToLong());
        ++it;
        if (i % 3 == 0) {
            it.releaseMemory();
        }
    }

    ASSERT_EQ(iteratedIds, generatedIds);
}

TEST_F(SpillableDocumentHashMapTest, EraseIfInMemoryFreesMemory) {
    _expCtx->setAllowDiskUse(true);
    auto map = createSpillableMap(100 * 1024 * 1024);
    // Generate and spill 10 documents.
    for (const auto& doc : generateDocuments(10, 1024)) {
        map.add(doc);
    }
    map.spillToDisk();

    // Generate 10 more documents to be in memory.
    for (const auto& doc : generateDocuments(10, 1024, 10)) {
        map.add(doc);
    }

    auto it = map.begin();
    // When iterating through in-memory part, we should be able to free memory.
    for (size_t i = 0; i < 9; ++i) {
        size_t sizeBeforeErase = map.getApproximateSize();
        Document erasedDoc = *it;
        map.eraseIfInMemoryAndAdvance(it);
        size_t sizeAfterErase = map.getApproximateSize();
        ASSERT_LTE(sizeAfterErase, sizeBeforeErase - erasedDoc.getApproximateSize());
    }

    // When memory is exhausted, iterator will buffer data from disk, which will cause
    // getApproximateSize to increase instead.
    map.eraseIfInMemoryAndAdvance(it);

    // Iterating through disk should also work.
    for (size_t i = 0; i < 10; ++i) {
        map.eraseIfInMemoryAndAdvance(it);
    }

    ASSERT_EQ(it, map.end());
}

TEST_F(SpillableDocumentHashMapTest, IteratorCanResumeAfterSwitchingOpCtx) {
    _expCtx->setAllowDiskUse(true);
    auto map = createSpillableMap(10 * 1024);
    const std::vector<Document> docs = generateDocuments(30, 1024);
    for (const auto& doc : docs) {
        map.add(doc);
    }

    auto unseenDocs = _expCtx->getValueComparator().makeFlatUnorderedValueSet();
    unseenDocs.insert(docs.begin(), docs.end());

    auto it = map.begin();
    for (size_t i = 0; i < docs.size(); ++i) {
        if (i % 5 == 4) {
            _opCtx.reset();
            _opCtx = makeOperationContext();
            _expCtx->setOperationContext(_opCtx.get());
        }

        Value value{*it};
        auto unseenIt = unseenDocs.find(value);
        ASSERT_NE(unseenIt, unseenDocs.end())
            << "Document " << it->toString() << " is found in map but not expected";
        unseenDocs.erase(unseenIt);
        ++it;
    }
    ASSERT_TRUE(it == map.end());
    ASSERT_TRUE(unseenDocs.empty())
        << "Documents not found in map but expected. Example: " << unseenDocs.begin()->toString();
}

}  // namespace
}  // namespace mongo
