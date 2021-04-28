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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_mongod_context_fixture.h"
#include "mongo/db/pipeline/window_function/spillable_cache.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class MongoProcessInterfaceForTest : public StubMongoProcessInterface {
public:
    std::unique_ptr<TemporaryRecordStore> createTemporaryRecordStore(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const override {
        expCtx->opCtx->recoveryUnit()->abandonSnapshot();
        expCtx->opCtx->recoveryUnit()->setPrepareConflictBehavior(
            PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
        return expCtx->opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(
            expCtx->opCtx);
    }

    void writeRecordsToRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   RecordStore* rs,
                                   std::vector<Record>* records,
                                   const std::vector<Timestamp>& ts) const override {

        writeConflictRetry(expCtx->opCtx, "MPI::writeRecordsToRecordStore", expCtx->ns.ns(), [&] {
            AutoGetCollection autoColl(expCtx->opCtx, expCtx->ns, MODE_IX);
            WriteUnitOfWork wuow(expCtx->opCtx);
            auto writeResult = rs->insertRecords(expCtx->opCtx, records, ts);
            tassert(5643014,
                    str::stream() << "Failed to write to disk because " << writeResult.reason(),
                    writeResult.isOK());
            wuow.commit();
        });
    }

    Document readRecordFromRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       RecordStore* rs,
                                       RecordId rID) const override {
        RecordData possibleRecord;
        AutoGetCollection autoColl(expCtx->opCtx, expCtx->ns, MODE_IX);
        auto foundDoc = rs->findRecord(expCtx->opCtx, RecordId(rID), &possibleRecord);
        tassert(5643001, str::stream() << "Could not find document id " << rID, foundDoc);
        return Document(possibleRecord.toBson());
    }

    void deleteRecordFromRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     RecordStore* rs,
                                     RecordId rID) const override {
        AutoGetCollection autoColl(expCtx->opCtx, expCtx->ns, MODE_IX);
        WriteUnitOfWork wuow(expCtx->opCtx);
        rs->deleteRecord(expCtx->opCtx, rID);
        wuow.commit();
    }

    void truncateRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             RecordStore* rs) const override {
        AutoGetCollection autoColl(expCtx->opCtx, expCtx->ns, MODE_IX);
        WriteUnitOfWork wuow(expCtx->opCtx);
        auto status = rs->truncate(expCtx->opCtx);
        tassert(5643015, "Unable to clear record store", status.isOK());
        wuow.commit();
    }
    void deleteTemporaryRecordStore(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    std::unique_ptr<TemporaryRecordStore> rs) const override {
        AutoGetCollection autoColl(expCtx->opCtx, expCtx->ns, MODE_IX);
        rs->finalizeTemporaryTable(expCtx->opCtx,
                                   TemporaryRecordStore::FinalizationAction::kDelete);
    }
};

class SpillableCacheTest : public AggregationMongoDContextFixture {
public:
    SpillableCacheTest() : AggregationMongoDContextFixture() {
        getExpCtx()->mongoProcessInterface = std::make_shared<MongoProcessInterfaceForTest>();
        _expCtx = getExpCtx();
    }

    std::unique_ptr<SpillableCache> createSpillableCache(size_t maxMem) {
        _tracker = std::make_unique<MemoryUsageTracker>(false, maxMem);
        auto cache = std::make_unique<SpillableCache>(_expCtx.get(), _tracker.get());
        return cache;
    }

    void buildAndLoadDocumentSet(int numDocs, SpillableCache* cache) {
        for (int i = _lastIndex; i < _lastIndex + numDocs; ++i) {
            _docSet.emplace_back(Document{{"val", i}});
            cache->addDocument(_docSet.back());
        }
    }

    void verifyDocsInCache(int start, int end, SpillableCache* cache) {
        for (int i = start; i < end; ++i) {
            ASSERT_DOCUMENT_EQ(cache->getDocumentById(i), _docSet[i]);
        }
    }

    boost::intrusive_ptr<ExpressionContext> _expCtx;
    std::unique_ptr<MemoryUsageTracker> _tracker;

    // Docs are ~200 each.
    std::vector<Document> _docSet;
    int _lastIndex = 0;
};

TEST_F(SpillableCacheTest, CanReadAndWriteDocumentsInMem) {
    _expCtx->allowDiskUse = false;
    auto cache = createSpillableCache(1000);
    buildAndLoadDocumentSet(2, cache.get());
    verifyDocsInCache(0, 2, cache.get());
}

TEST_F(SpillableCacheTest, LoadingFailsIfCantSpillToDisk) {
    _expCtx->allowDiskUse = false;
    auto cache = createSpillableCache(1);
    ASSERT_THROWS_CODE(buildAndLoadDocumentSet(1, cache.get()), AssertionException, 5643011);
}

TEST_F(SpillableCacheTest, CanReadAndWriteDocumentsToDisk) {
    _expCtx->allowDiskUse = true;
    auto cache = createSpillableCache(1);
    buildAndLoadDocumentSet(3, cache.get());
    verifyDocsInCache(0, 3, cache.get());
    _expCtx->allowDiskUse = false;
}

TEST_F(SpillableCacheTest, CanReturnDocumentsFromCacheAndDisk) {
    _expCtx->allowDiskUse = true;
    // Docs are ~200 each.
    auto cache = createSpillableCache(250);
    buildAndLoadDocumentSet(3, cache.get());
    verifyDocsInCache(0, 3, cache.get());
    _expCtx->allowDiskUse = false;
    cache->finalize();
}

DEATH_TEST_F(SpillableCacheTest, RemovesDocumentsWhenExpired, "Requested expired document") {
    _expCtx->allowDiskUse = false;
    auto cache = createSpillableCache(1000);
    buildAndLoadDocumentSet(4, cache.get());
    cache->freeUpTo(0);
    verifyDocsInCache(1, 4, cache.get());
    ASSERT_THROWS_CODE(cache->getDocumentById(0), AssertionException, 5643005);
}

TEST_F(SpillableCacheTest, ReturnsCorrectDocumentsIfSomeHaveBeenRemovedMemOnly) {
    _expCtx->allowDiskUse = false;
    auto cache = createSpillableCache(5000);
    buildAndLoadDocumentSet(10, cache.get());
    cache->freeUpTo(4);
    verifyDocsInCache(5, 10, cache.get());
}

TEST_F(SpillableCacheTest, ReturnsCorrectDocumentsIfSomeHaveBeenRemovedMixed) {
    _expCtx->allowDiskUse = true;
    auto cache = createSpillableCache(1000);
    buildAndLoadDocumentSet(10, cache.get());
    // Only mark documents on disk as freed.
    cache->freeUpTo(4);
    // Force spilling again.
    buildAndLoadDocumentSet(5, cache.get());
    verifyDocsInCache(5, 15, cache.get());
    cache->finalize();
    _expCtx->allowDiskUse = false;
}

TEST_F(SpillableCacheTest, CanInsertLargeDocuments) {
    _expCtx->allowDiskUse = true;
    // 19 MB
    auto cache = createSpillableCache(19 * 1024 * 1024);
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
    _expCtx->allowDiskUse = false;
}

}  // namespace
}  // namespace mongo
