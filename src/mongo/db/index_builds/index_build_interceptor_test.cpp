/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/index_builds/index_build_interceptor.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/lazy_record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <span>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
// Projection of 'count' and 'sum' from 'HistogramData<int64_t>' returned by
// 'OtelMetricsCapturer::readInt64Histogram()'.
struct HistogramSnapshot {
    uint64_t count;
    int64_t sum;
};

// TODO SERVER-121249: Replace this local helper if SERVER-121249 introduces
// an OTel histogram test helper that supports empty histograms.
HistogramSnapshot readHistogramOrZero(otel::metrics::OtelMetricsCapturer& capturer,
                                      otel::metrics::MetricName metricName) {
    try {
        const auto histogram = capturer.readInt64Histogram(metricName);
        return {histogram.count, histogram.sum};
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::KeyNotFound) {
            return {0, 0};
        }
        throw;
    }
}

class IndexBuilderInterceptorTest : public CatalogTestFixture {
protected:
    const IndexCatalogEntry* createIndex(BSONObj spec) {
        WriteUnitOfWork wuow(operationContext());
        CollectionWriter writer{operationContext(), _coll.get()};

        auto* indexCatalog = writer.getWritableCollection(operationContext())->getIndexCatalog();
        uassertStatusOK(indexCatalog->createIndexOnEmptyCollection(
            operationContext(), writer.getWritableCollection(operationContext()), spec));
        wuow.commit();

        return indexCatalog->findIndexByName(
            operationContext(), spec.getStringField(IndexDescriptor::kIndexNameFieldName));
    }

    IndexBuildInfo buildIndexBuildInfo(BSONObj spec) {
        auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();
        return IndexBuildInfo(spec, *storageEngine, _nss.dbName());
    }
    std::unique_ptr<IndexBuildInterceptor> createIndexBuildInterceptor(
        const IndexBuildInfo& indexBuildInfo,
        LazyRecordStore::CreateMode createMode = LazyRecordStore::CreateMode::deferred) {
        WriteUnitOfWork wuow(operationContext());
        auto interceptor = std::make_unique<IndexBuildInterceptor>(
            operationContext(),
            indexBuildInfo,
            createMode,
            createIndex(indexBuildInfo.spec)->descriptor()->unique(),
            /*generateTableWrites=*/true);
        wuow.commit();
        return interceptor;
    }

    /**
     * Returns table from ident. Requires that the table exists.
     */
    std::unique_ptr<RecordStore> getTable(StringData ident) {
        auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
        return operationContext()
            ->getServiceContext()
            ->getStorageEngine()
            ->getEngine()
            ->getInternalRecordStore(ru, ident, KeyFormat::Long);
    }

    bool hasTable(StringData ident) {
        return operationContext()->getServiceContext()->getStorageEngine()->getEngine()->hasIdent(
            *shard_role_details::getRecoveryUnit(operationContext()), ident);
    }

    std::vector<BSONObj> getTableContents(std::unique_ptr<RecordStore> table) {
        std::vector<BSONObj> contents;
        auto cursor = table->getCursor(operationContext(),
                                       *shard_role_details::getRecoveryUnit(operationContext()));
        while (auto record = cursor->next()) {
            contents.push_back(record->data.toBson().getOwned());
        }
        return contents;
    }

    std::vector<BSONObj> getSideWritesTableContents(const IndexBuildInfo& info) {
        return getTableContents(getTable(*info.sideWritesIdent));
    }

    std::vector<BSONObj> getSkippedRecordsTableContents(const IndexBuildInfo& info) {
        return getTableContents(getTable(*info.skippedRecordsIdent));
    }

    std::vector<std::string> getDuplicateKeyTableContents(const IndexBuildInfo& info) {
        std::vector<std::string> contents;
        auto duplicateKeyTable = getTable(*info.constraintViolationsIdent);
        auto cursor = duplicateKeyTable->getCursor(
            operationContext(), *shard_role_details::getRecoveryUnit(operationContext()));
        while (auto record = cursor->next()) {
            contents.emplace_back(record->data.data(), record->data.size());
        }
        return contents;
    }

    const IndexCatalogEntry* getIndexEntry(const std::string& indexName) {
        return _coll.get()->getIndexCatalog()->findIndexByName(operationContext(), indexName);
    }

    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(operationContext(), _nss, {}));
        _coll.emplace(operationContext(), _nss, MODE_X);
    }

    void tearDown() override {
        _coll.reset();
        CatalogTestFixture::tearDown();
    }

    // Reusable function which executes the test and can be run under different configurations (eg.
    // feature flags).
    void testSingleOpIsSavedToSideWritesTable(IndexBuildInterceptor::Op op,
                                              LazyRecordStore::CreateMode createMode);

    boost::optional<AutoGetCollection> _coll;

private:
    NamespaceString _nss = NamespaceString::createNamespaceString_forTest("testDB.interceptor");
};

void IndexBuilderInterceptorTest::testSingleOpIsSavedToSideWritesTable(
    IndexBuildInterceptor::Op op, LazyRecordStore::CreateMode createMode) {

    otel::metrics::OtelMetricsCapturer capturer;
    int64_t insertedBefore = 0;
    int64_t deletedBefore = 0;
    if (capturer.canReadMetrics()) {
        insertedBefore =
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildSideWritesInserted);
        deletedBefore =
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildSideWritesDeleted);
    }

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor = createIndexBuildInterceptor(indexBuildInfo, createMode);
    const auto entry = getIndexEntry("a_1");

    key_string::HeapBuilder ksBuilder(key_string::Version::kLatestVersion);
    ksBuilder.appendNumberLong(10);
    key_string::Value keyString(ksBuilder.release());

    WriteUnitOfWork wuow(operationContext());
    int64_t numKeys = 0;
    ASSERT_OK(interceptor->sideWrite(
        operationContext(), *_coll.get(), entry, {keyString}, {}, {}, op, &numKeys));
    ASSERT_EQ(1, numKeys);
    wuow.commit();

    BufBuilder bufBuilder;
    keyString.serialize(bufBuilder);
    BSONBinData serializedKeyString(bufBuilder.buf(), bufBuilder.len(), BinDataGeneral);

    auto sideWrites = getSideWritesTableContents(indexBuildInfo);
    ASSERT_EQ(1, sideWrites.size());
    ASSERT_BSONOBJ_EQ(BSON("op" << (op == IndexBuildInterceptor::Op::kInsert ? "i" : "d") << "key"
                                << serializedKeyString),
                      sideWrites[0]);

    if (capturer.canReadMetrics()) {
        EXPECT_EQ(
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildSideWritesInserted),
            insertedBefore + (op == IndexBuildInterceptor::Op::kInsert ? 1 : 0));
        EXPECT_EQ(
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildSideWritesDeleted),
            deletedBefore + (op == IndexBuildInterceptor::Op::kDelete ? 1 : 0));
    }
}

TEST_F(IndexBuilderInterceptorTest, SingleInsertIsSavedToSideWritesTable) {
    testSingleOpIsSavedToSideWritesTable(IndexBuildInterceptor::Op::kInsert,
                                         LazyRecordStore::CreateMode::deferred);
}

TEST_F(IndexBuilderInterceptorTest, SingleInsertIsSavedToSideWritesTablePrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    testSingleOpIsSavedToSideWritesTable(IndexBuildInterceptor::Op::kInsert,
                                         LazyRecordStore::CreateMode::immediate);
}

TEST_F(IndexBuilderInterceptorTest, SingleDeleteIsSavedToSideWritesTable) {
    testSingleOpIsSavedToSideWritesTable(IndexBuildInterceptor::Op::kDelete,
                                         LazyRecordStore::CreateMode::deferred);
}

TEST_F(IndexBuilderInterceptorTest, SingleDeleteIsSavedToSideWritesTablePrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    testSingleOpIsSavedToSideWritesTable(IndexBuildInterceptor::Op::kDelete,
                                         LazyRecordStore::CreateMode::immediate);
}

TEST_F(IndexBuilderInterceptorTest, SingleInsertIsSavedToSkippedRecordsIntRidTrackerTable) {
    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor = createIndexBuildInterceptor(indexBuildInfo);

    auto recordId = RecordId(1);
    BSONObjBuilder builder;
    recordId.serializeToken("recordId", &builder);

    WriteUnitOfWork wuow(operationContext());
    interceptor->getSkippedRecordTracker().record(operationContext(), *_coll.get(), recordId);
    wuow.commit();

    auto skippedRecordsTable = getSkippedRecordsTableContents(indexBuildInfo);
    ASSERT_EQ(1, skippedRecordsTable.size());
    ASSERT_BSONOBJ_EQ(builder.obj(), skippedRecordsTable[0]);
}

TEST_F(IndexBuilderInterceptorTest, SingleInsertIsSavedToSkippedRecordsTableIntRidPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);

    auto recordId = RecordId(1);
    BSONObjBuilder builder;
    recordId.serializeToken("recordId", &builder);

    WriteUnitOfWork wuow(operationContext());
    interceptor->getSkippedRecordTracker().record(operationContext(), *_coll.get(), recordId);
    wuow.commit();

    auto skippedRecordsTable = getSkippedRecordsTableContents(indexBuildInfo);
    ASSERT_EQ(1, skippedRecordsTable.size());
    ASSERT_BSONOBJ_EQ(builder.obj(), skippedRecordsTable[0]);
}

TEST_F(IndexBuilderInterceptorTest, SingleInsertIsSavedToskippedRecordsTableStringRid) {
    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor = createIndexBuildInterceptor(indexBuildInfo);

    auto recordId = RecordId("meow");
    BSONObjBuilder builder;
    recordId.serializeToken("recordId", &builder);

    WriteUnitOfWork wuow(operationContext());
    interceptor->getSkippedRecordTracker().record(operationContext(), *_coll.get(), recordId);
    wuow.commit();

    auto skippedRecordsTable = getSkippedRecordsTableContents(indexBuildInfo);
    ASSERT_EQ(1, skippedRecordsTable.size());
    ASSERT_BSONOBJ_EQ(builder.obj(), skippedRecordsTable[0]);
}

TEST_F(IndexBuilderInterceptorTest,
       SingleInsertIsSavedToskippedRecordsTableStringRidPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);

    auto recordId = RecordId("meow");
    BSONObjBuilder builder;
    recordId.serializeToken("recordId", &builder);

    WriteUnitOfWork wuow(operationContext());
    interceptor->getSkippedRecordTracker().record(operationContext(), *_coll.get(), recordId);
    wuow.commit();

    auto skippedRecordsTable = getSkippedRecordsTableContents(indexBuildInfo);
    ASSERT_EQ(1, skippedRecordsTable.size());
    ASSERT_BSONOBJ_EQ(builder.obj(), skippedRecordsTable[0]);
}

TEST_F(IndexBuilderInterceptorTest, SingleInsertIsSavedToDuplicateKeyTable) {
    auto indexBuildInfo =
        buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true}"));
    auto interceptor = createIndexBuildInterceptor(indexBuildInfo);
    const auto entry = getIndexEntry("a_1");

    key_string::HeapBuilder ksBuilder(key_string::Version::kLatestVersion);
    ksBuilder.appendNumberLong(10);
    key_string::Value keyString(ksBuilder.release());

    WriteUnitOfWork wuow(operationContext());
    ASSERT_OK(interceptor->recordDuplicateKey(operationContext(), *_coll.get(), entry, keyString));
    wuow.commit();

    key_string::View keyStringView(keyString);
    StackBufBuilder builder;
    keyStringView.serializeWithoutRecordId(builder);
    std::string ksWithoutRid(builder.buf(), builder.len());
    auto duplicates = getDuplicateKeyTableContents(indexBuildInfo);
    ASSERT_EQ(duplicates[0], ksWithoutRid);
}

TEST_F(IndexBuilderInterceptorTest, SingleInsertIsSavedToDuplicateKeyTablePrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto indexBuildInfo =
        buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");

    key_string::HeapBuilder ksBuilder(key_string::Version::kLatestVersion);
    ksBuilder.appendNumberLong(10);
    key_string::Value keyString(ksBuilder.release());

    WriteUnitOfWork wuow(operationContext());
    ASSERT_OK(interceptor->recordDuplicateKey(operationContext(), *_coll.get(), entry, keyString));
    wuow.commit();

    key_string::View keyStringView(keyString);
    StackBufBuilder builder;
    keyStringView.serializeWithoutRecordId(builder);
    std::string ksWithoutRid(builder.buf(), builder.len());
    auto duplicates = getDuplicateKeyTableContents(indexBuildInfo);
    ASSERT_EQ(duplicates[0], ksWithoutRid);
}

TEST_F(IndexBuilderInterceptorTest, SingleInsertIsDrainedIntoIndexPrimaryDriven) {
    otel::metrics::OtelMetricsCapturer capturer;
    int64_t drainedBefore = 0;
    HistogramSnapshot drainDurationBefore{0, 0};
    HistogramSnapshot drainBytesBefore{0, 0};
    if (capturer.canReadMetrics()) {
        drainedBefore =
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildSideWritesDrained);
        drainDurationBefore = readHistogramOrZero(
            capturer, otel::metrics::MetricNames::kIndexBuildSideWritesDrainDuration);
        drainBytesBefore = readHistogramOrZero(
            capturer, otel::metrics::MetricNames::kIndexBuildSideWritesDrainBytes);
    }

    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");

    key_string::HeapBuilder ksBuilder(key_string::Version::kLatestVersion);
    ksBuilder.appendNumberLong(10);
    ksBuilder.appendRecordId(RecordId{1});
    key_string::Value keyString(ksBuilder.release());

    // Set up by inserting into side write table.
    {
        WriteUnitOfWork wuow(operationContext());
        int64_t numKeys = 0;
        ASSERT_OK(interceptor->sideWrite(operationContext(),
                                         *_coll.get(),
                                         entry,
                                         {keyString},
                                         {},
                                         {},
                                         IndexBuildInterceptor::Op::kInsert,
                                         &numKeys));
        ASSERT_EQ(1, numKeys);
        wuow.commit();
    }

    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kNoTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
    auto indexCursor = entry->accessMethod()->asSortedData()->newCursor(operationContext(), ru);

    // Check that the key was inserted into the index.
    ASSERT(indexCursor->seekForKeyString(ru, keyString.getView()));
    ASSERT_FALSE(indexCursor->nextKeyString(ru));

    // Check that the side write table is empty since the side write was removed.
    auto sideWrites = getSideWritesTableContents(indexBuildInfo);
    ASSERT_EQ(0, sideWrites.size());

    if (capturer.canReadMetrics()) {
        EXPECT_EQ(
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildSideWritesDrained),
            drainedBefore + 1);
        const auto drainDurationAfter = capturer.readInt64Histogram(
            otel::metrics::MetricNames::kIndexBuildSideWritesDrainDuration);
        EXPECT_EQ(drainDurationAfter.count, drainDurationBefore.count + 1);
        EXPECT_GE(drainDurationAfter.sum, drainDurationBefore.sum);
        const auto drainBytesAfter = capturer.readInt64Histogram(
            otel::metrics::MetricNames::kIndexBuildSideWritesDrainBytes);
        EXPECT_EQ(drainBytesAfter.count, drainBytesBefore.count + 1);
        EXPECT_GE(drainBytesAfter.sum, drainBytesBefore.sum);
    }
}

TEST_F(IndexBuilderInterceptorTest, SingleDeleteIsDrainedIntoIndexPrimaryDriven) {
    otel::metrics::OtelMetricsCapturer capturer;
    int64_t drainedBefore = 0;
    HistogramSnapshot drainDurationBefore{0, 0};
    HistogramSnapshot drainBytesBefore{0, 0};
    if (capturer.canReadMetrics()) {
        drainedBefore =
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildSideWritesDrained);
        drainDurationBefore = readHistogramOrZero(
            capturer, otel::metrics::MetricNames::kIndexBuildSideWritesDrainDuration);
        drainBytesBefore = readHistogramOrZero(
            capturer, otel::metrics::MetricNames::kIndexBuildSideWritesDrainBytes);
    }

    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");
    auto indexAccessMethod = entry->accessMethod()->asSortedData();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());

    key_string::HeapBuilder ksBuilder(key_string::Version::kLatestVersion);
    ksBuilder.appendNumberLong(10);
    ksBuilder.appendRecordId(RecordId{1});
    key_string::Value keyString(ksBuilder.release());
    KeyStringSet keySet;
    keySet.insert(keyString);

    // Set up by inserting into index.
    {
        WriteUnitOfWork wuow(operationContext());
        int64_t numInserted = 0;
        ASSERT_OK(indexAccessMethod->insertKeys(operationContext(),
                                                ru,
                                                *_coll.get(),
                                                entry,
                                                keySet,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                {},
                                                &numInserted));
        ASSERT_EQ(numInserted, 1);
        wuow.commit();
    }

    // Write key removal to side write table.
    {
        WriteUnitOfWork wuow(operationContext());
        int64_t numKeys = 0;
        ASSERT_OK(interceptor->sideWrite(operationContext(),
                                         *_coll.get(),
                                         entry,
                                         {keyString},
                                         {},
                                         {},
                                         IndexBuildInterceptor::Op::kDelete,
                                         &numKeys));
        ASSERT_EQ(1, numKeys);
        wuow.commit();
    }

    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kNoTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // Check that the index is now empty since the key was removed.
    auto indexCursor = indexAccessMethod->newCursor(operationContext(), ru);
    ASSERT_FALSE(indexCursor->nextKeyString(ru));

    // Check that the side write table is empty since the side write was removed.
    auto sideWrites = getSideWritesTableContents(indexBuildInfo);
    ASSERT_EQ(0, sideWrites.size());

    if (capturer.canReadMetrics()) {
        EXPECT_EQ(
            capturer.readInt64Counter(otel::metrics::MetricNames::kIndexBuildSideWritesDrained),
            drainedBefore + 1);
        const auto drainDurationAfter = capturer.readInt64Histogram(
            otel::metrics::MetricNames::kIndexBuildSideWritesDrainDuration);
        EXPECT_EQ(drainDurationAfter.count, drainDurationBefore.count + 1);
        EXPECT_GE(drainDurationAfter.sum, drainDurationBefore.sum);
        const auto drainBytesAfter = capturer.readInt64Histogram(
            otel::metrics::MetricNames::kIndexBuildSideWritesDrainBytes);
        EXPECT_EQ(drainBytesAfter.count, drainBytesBefore.count + 1);
        EXPECT_GE(drainBytesAfter.sum, drainBytesBefore.sum);
    }
}

TEST_F(IndexBuilderInterceptorTest, DeferredTableCreation) {
    auto indexBuildInfo =
        buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::deferred);
    const auto entry = getIndexEntry("a_1");

    // The side writes table should exist immediately but the others should not
    ASSERT_TRUE(hasTable(*indexBuildInfo.sideWritesIdent));
    ASSERT_FALSE(hasTable(*indexBuildInfo.skippedRecordsIdent));
    ASSERT_FALSE(hasTable(*indexBuildInfo.constraintViolationsIdent));

    {
        WriteUnitOfWork wuow(operationContext());
        interceptor->getSkippedRecordTracker().record(
            operationContext(), *_coll.get(), RecordId(1));
        wuow.commit();
    }

    // The skipped records table should now exist, but the duplicate key table still doesn't
    ASSERT_TRUE(hasTable(*indexBuildInfo.sideWritesIdent));
    ASSERT_TRUE(hasTable(*indexBuildInfo.skippedRecordsIdent));
    ASSERT_FALSE(hasTable(*indexBuildInfo.constraintViolationsIdent));

    {
        key_string::HeapBuilder ksBuilder(key_string::Version::kLatestVersion);
        ksBuilder.appendNumberLong(10);

        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(interceptor->recordDuplicateKey(
            operationContext(), *_coll.get(), entry, ksBuilder.release()));
        wuow.commit();
    }

    // All three should now exist
    ASSERT_TRUE(hasTable(*indexBuildInfo.sideWritesIdent));
    ASSERT_TRUE(hasTable(*indexBuildInfo.skippedRecordsIdent));
    ASSERT_TRUE(hasTable(*indexBuildInfo.constraintViolationsIdent));
}

TEST_F(IndexBuilderInterceptorTest, ImmediateTableCreation) {
    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    ASSERT_TRUE(hasTable(*indexBuildInfo.sideWritesIdent));
    ASSERT_TRUE(hasTable(*indexBuildInfo.skippedRecordsIdent));
    // Index isn't unique so no duplicate key table
    ASSERT_FALSE(indexBuildInfo.constraintViolationsIdent);
}

TEST_F(IndexBuilderInterceptorTest, ImmediateTableCreationUnique) {
    auto indexBuildInfo =
        buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    ASSERT_TRUE(hasTable(*indexBuildInfo.sideWritesIdent));
    ASSERT_TRUE(hasTable(*indexBuildInfo.skippedRecordsIdent));
    ASSERT_TRUE(hasTable(*indexBuildInfo.constraintViolationsIdent));
}

using IndexBuilderInterceptorTestDeathTest = IndexBuilderInterceptorTest;
DEATH_TEST_F(IndexBuilderInterceptorTestDeathTest,
             OpenExistingRequiresThatTablesAlreadyExist,
             "Metadata format version check failed") {
    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::openExisting);
}

TEST_F(IndexBuilderInterceptorTest, OpenExistingPreservesExistingData) {
    auto indexBuildInfo =
        buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true}"));

    {
        auto interceptor =
            createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::deferred);
        auto entry = getIndexEntry("a_1");
        key_string::HeapBuilder ksBuilder(key_string::Version::kLatestVersion);
        ksBuilder.appendNumberLong(10);
        auto keyString = key_string::Value(ksBuilder.release());
        RecordId recordId(1);

        WriteUnitOfWork wuow(operationContext());
        int64_t numKeys = 0;
        ASSERT_OK(interceptor->sideWrite(operationContext(),
                                         *_coll.get(),
                                         entry,
                                         {keyString},
                                         {},
                                         {},
                                         IndexBuildInterceptor::Op::kInsert,
                                         &numKeys));
        interceptor->getSkippedRecordTracker().record(operationContext(), *_coll.get(), recordId);
        ASSERT_OK(
            interceptor->recordDuplicateKey(operationContext(), *_coll.get(), entry, keyString));
        wuow.commit();
    }

    // Creating a new interceptor in openExisting mode should preserve the existing data.
    IndexBuildInterceptor interceptor(operationContext(),
                                      indexBuildInfo,
                                      LazyRecordStore::CreateMode::openExisting,
                                      getIndexEntry("a_1")->descriptor()->unique(),
                                      /*generateTableWrites=*/true);

    ASSERT_EQ(1, getSideWritesTableContents(indexBuildInfo).size());
    ASSERT_EQ(1, getSkippedRecordsTableContents(indexBuildInfo).size());
    ASSERT_EQ(1, getDuplicateKeyTableContents(indexBuildInfo).size());
}

TEST_F(IndexBuilderInterceptorTest, DropTemporaryTables) {
    auto indexBuildInfo =
        buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);

    ASSERT_TRUE(hasTable(*indexBuildInfo.sideWritesIdent));
    ASSERT_TRUE(hasTable(*indexBuildInfo.skippedRecordsIdent));
    ASSERT_TRUE(hasTable(*indexBuildInfo.constraintViolationsIdent));

    interceptor->dropTemporaryTables(operationContext(), Timestamp::min());

    auto storageEngine = operationContext()->getServiceContext()->getStorageEngine();

    // Idents still exist in WiredTiger because the drop is deferred to the reaper.
    ASSERT_TRUE(hasTable(*indexBuildInfo.sideWritesIdent));
    ASSERT_TRUE(hasTable(*indexBuildInfo.skippedRecordsIdent));
    ASSERT_TRUE(hasTable(*indexBuildInfo.constraintViolationsIdent));

    // Force reaping
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(),
                                                            *indexBuildInfo.sideWritesIdent));
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(operationContext(),
                                                            *indexBuildInfo.skippedRecordsIdent));
    ASSERT_OK(storageEngine->immediatelyCompletePendingDrop(
        operationContext(), *indexBuildInfo.constraintViolationsIdent));

    // Table must now be dropped
    ASSERT_FALSE(hasTable(*indexBuildInfo.sideWritesIdent));
    ASSERT_FALSE(hasTable(*indexBuildInfo.skippedRecordsIdent));
    ASSERT_FALSE(hasTable(*indexBuildInfo.constraintViolationsIdent));
}

TEST_F(IndexBuilderInterceptorTest, DropTemporaryTablesOnDeferredTableIsNoOp) {
    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::deferred);

    ASSERT_FALSE(hasTable(*indexBuildInfo.skippedRecordsIdent));

    // Dropping should not crash even though some tables were never created.
    ASSERT_NO_THROW(interceptor->dropTemporaryTables(operationContext(), Timestamp::min()));

    ASSERT_FALSE(hasTable(*indexBuildInfo.skippedRecordsIdent));
}

TEST_F(IndexBuilderInterceptorTest, GetTableAfterDropReturnsNull) {
    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));

    LazyRecordStore lrs(operationContext(),
                        *indexBuildInfo.sideWritesIdent,
                        LazyRecordStore::CreateMode::immediate);

    ASSERT_TRUE(lrs.tableExists());

    lrs.drop(operationContext(), Timestamp::min());

    ASSERT_FALSE(lrs.tableExists());
}

}  // namespace
}  // namespace mongo
