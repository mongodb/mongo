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
#include "mongo/db/op_observer/op_observer_noop.h"
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

key_string::Value makeKeyString(int64_t value, RecordId rid) {
    key_string::HeapBuilder ksBuilder(key_string::Version::kLatestVersion);
    ksBuilder.appendNumberLong(value);
    ksBuilder.appendRecordId(rid);
    return key_string::Value(ksBuilder.release());
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
            createIndex(indexBuildInfo.spec)->descriptor()->unique());
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
                                      getIndexEntry("a_1")->descriptor()->unique());

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

    interceptor->dropTemporaryTables(operationContext(), StorageEngine::Immediate{});

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
    ASSERT_NO_THROW(
        interceptor->dropTemporaryTables(operationContext(), StorageEngine::Immediate{}));

    ASSERT_FALSE(hasTable(*indexBuildInfo.skippedRecordsIdent));
}

TEST_F(IndexBuilderInterceptorTest, GetTableAfterDropReturnsNull) {
    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));

    LazyRecordStore lrs(operationContext(),
                        *indexBuildInfo.sideWritesIdent,
                        LazyRecordStore::CreateMode::immediate);

    ASSERT_TRUE(lrs.tableExists());

    lrs.drop(operationContext(), StorageEngine::Immediate{});

    ASSERT_FALSE(lrs.tableExists());
}

/**
 * Fake OpObserver that records onContainerInsert/onContainerDelete calls for verification
 * without generating real oplog entries.
 */
class ContainerOpCountingObserver : public OpObserverNoop {
public:
    void onContainerInsert(OperationContext*,
                           StringData ident,
                           std::span<const char>,
                           std::span<const char>) override {
        inserts.emplace_back(ident);
    }
    void onContainerInsert(OperationContext*,
                           StringData ident,
                           int64_t,
                           std::span<const char>) override {
        inserts.emplace_back(ident);
    }
    void onContainerDelete(OperationContext*, StringData ident, int64_t) override {
        deletes.emplace_back(ident);
    }
    void onContainerDelete(OperationContext*, StringData ident, std::span<const char>) override {
        deletes.emplace_back(ident);
    }

    size_t countInsertsFor(StringData ident, size_t from = 0) const {
        return std::count(inserts.begin() + from, inserts.end(), ident);
    }
    size_t countDeletesFor(StringData ident, size_t from = 0) const {
        return std::count(deletes.begin() + from, deletes.end(), ident);
    }

    std::vector<std::string> inserts;
    std::vector<std::string> deletes;
};

ContainerOpCountingObserver* installContainerOpObserver(OperationContext* opCtx) {
    auto observer = std::make_unique<ContainerOpCountingObserver>();
    auto* ptr = observer.get();
    opCtx->getServiceContext()->resetOpObserver_forTest(std::move(observer));
    return ptr;
}

TEST_F(IndexBuilderInterceptorTest, DrainSideWriteGeneratesNoContainerOpsWithoutPrimaryDriven) {
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::deferred);
    const auto entry = getIndexEntry("a_1");

    auto keyString = makeKeyString(10, RecordId{1});

    // Buffer a side write (insert) without PDIB feature flags.
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

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain without PDIB — should use regular record store ops, not container writes.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kNoTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // Verify the key was still inserted into the index.
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
    auto indexCursor = entry->accessMethod()->asSortedData()->newCursor(operationContext(), ru);
    ASSERT(indexCursor->seekForKeyString(ru, keyString.getView()));

    // No container operations should have been fired — non-PDIB builds don't use
    // container writes, so the OpObserver is never notified.
    ASSERT_EQ(observer->inserts.size(), insertsBefore);
    ASSERT_EQ(observer->deletes.size(), deletesBefore);

    // Verify side writes table is empty after draining.
    ASSERT_EQ(0, getSideWritesTableContents(indexBuildInfo).size());
}

TEST_F(IndexBuilderInterceptorTest, DrainInsertSideWriteGeneratesContainerOpsPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");

    auto keyString = makeKeyString(10, RecordId{1});

    // Buffer a side write (insert).
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

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain the side writes into the index.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kNoTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // Expect exactly one container insert (index) and one container delete (side writes table).
    ASSERT_EQ(observer->countInsertsFor(entry->getIdent(), insertsBefore), 1u);
    ASSERT_EQ(observer->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore), 1u);

    // Verify side writes table is empty and the key was inserted into the index.
    ASSERT_EQ(0, getSideWritesTableContents(indexBuildInfo).size());
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
    auto indexCursor = entry->accessMethod()->asSortedData()->newCursor(operationContext(), ru);
    ASSERT(indexCursor->seekForKeyString(ru, keyString.getView()));
}

TEST_F(IndexBuilderInterceptorTest, DrainDeleteSideWriteGeneratesContainerOpsPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");
    auto indexAccessMethod = entry->accessMethod()->asSortedData();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());

    auto keyString = makeKeyString(10, RecordId{1});
    KeyStringSet keySet;
    keySet.insert(keyString);

    // Pre-populate the index with the key so the delete side write has something to remove.
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

    // Buffer a side write (delete).
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

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain the side writes into the index.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kNoTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // No container inserts should be fired for a delete-only drain.
    ASSERT_EQ(observer->countInsertsFor(entry->getIdent(), insertsBefore), 0u);

    // Expect two container deletes: one for the index key, one for the side writes table.
    ASSERT_EQ(observer->countDeletesFor(entry->getIdent(), deletesBefore), 1u);
    ASSERT_EQ(observer->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore), 1u);

    // Verify side writes table is empty and the key was removed from the index.
    ASSERT_EQ(0, getSideWritesTableContents(indexBuildInfo).size());
    auto indexCursor = indexAccessMethod->newCursor(operationContext(), ru);
    ASSERT_FALSE(indexCursor->seekForKeyString(ru, keyString.getView()));
}

TEST_F(IndexBuilderInterceptorTest, DrainEmptySideWritesTableGeneratesNoContainerOpsPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain with no side writes buffered.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kNoTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // No container operations should be fired for an empty drain.
    ASSERT_EQ(observer->inserts.size(), insertsBefore);
    ASSERT_EQ(observer->deletes.size(), deletesBefore);
}

TEST_F(IndexBuilderInterceptorTest, DrainMultipleSideWritesGeneratesContainerOpsPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");

    // Buffer three insert side writes.
    const int kNumSideWrites = 3;
    for (int i = 0; i < kNumSideWrites; ++i) {
        auto keyString = makeKeyString(10 + i, RecordId{static_cast<int64_t>(i + 1)});

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

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain all side writes.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kNoTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // Each side write should produce one index insert and one side-table delete.
    ASSERT_EQ(observer->countInsertsFor(entry->getIdent(), insertsBefore),
              static_cast<size_t>(kNumSideWrites));
    ASSERT_EQ(observer->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore),
              static_cast<size_t>(kNumSideWrites));

    // Verify side writes table is empty and all keys are in the index.
    ASSERT_EQ(0, getSideWritesTableContents(indexBuildInfo).size());

    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
    auto indexCursor = entry->accessMethod()->asSortedData()->newCursor(operationContext(), ru);
    for (int i = 0; i < kNumSideWrites; ++i) {
        auto expected = makeKeyString(10 + i, RecordId{static_cast<int64_t>(i + 1)});
        ASSERT(indexCursor->seekForKeyString(ru, expected.getView()));
    }
}

TEST_F(IndexBuilderInterceptorTest,
       DrainMixedInsertDeleteSideWritesGeneratesCorrectContainerOpsPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");
    auto indexAccessMethod = entry->accessMethod()->asSortedData();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());

    // Build two key strings: one for a key to insert, one for a key to delete.
    auto insertKeyString = makeKeyString(20, RecordId{2});
    auto deleteKeyString = makeKeyString(30, RecordId{3});

    // Pre-populate the index with the key to be deleted.
    {
        KeyStringSet keySet;
        keySet.insert(deleteKeyString);
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

    // Buffer an insert side write.
    {
        WriteUnitOfWork wuow(operationContext());
        int64_t numKeys = 0;
        ASSERT_OK(interceptor->sideWrite(operationContext(),
                                         *_coll.get(),
                                         entry,
                                         {insertKeyString},
                                         {},
                                         {},
                                         IndexBuildInterceptor::Op::kInsert,
                                         &numKeys));
        ASSERT_EQ(1, numKeys);
        wuow.commit();
    }

    // Buffer a delete side write.
    {
        WriteUnitOfWork wuow(operationContext());
        int64_t numKeys = 0;
        ASSERT_OK(interceptor->sideWrite(operationContext(),
                                         *_coll.get(),
                                         entry,
                                         {deleteKeyString},
                                         {},
                                         {},
                                         IndexBuildInterceptor::Op::kDelete,
                                         &numKeys));
        ASSERT_EQ(1, numKeys);
        wuow.commit();
    }

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain all side writes.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kNoTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    std::string indexIdent = entry->getIdent();

    // One insert side write → one index insert; one delete side write → one index delete.
    ASSERT_EQ(observer->countInsertsFor(indexIdent, insertsBefore), 1u);
    ASSERT_EQ(observer->countDeletesFor(indexIdent, deletesBefore), 1u);
    // Two side writes cleaned up → two side-table deletes.
    ASSERT_EQ(observer->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore), 2u);

    // Verify the insert key is now in the index and the delete key is gone.
    auto indexCursor = indexAccessMethod->newCursor(operationContext(), ru);
    ASSERT(indexCursor->seekForKeyString(ru, insertKeyString.getView()));
    indexCursor = indexAccessMethod->newCursor(operationContext(), ru);
    ASSERT_FALSE(indexCursor->seekForKeyString(ru, deleteKeyString.getView()));
}

TEST_F(IndexBuilderInterceptorTest, DrainMultipleBatchesGeneratesCorrectContainerOpsPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    // Force each side write to drain in its own WriteUnitOfWork batch.
    RAIIServerParameterControllerForTest batchSize("maxIndexBuildDrainBatchSize", 1);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo = buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");

    // Buffer three insert side writes.
    const int kNumSideWrites = 3;
    std::vector<key_string::Value> keyStrings;
    for (int i = 0; i < kNumSideWrites; ++i) {
        keyStrings.emplace_back(makeKeyString(10 + i, RecordId{static_cast<int64_t>(i + 1)}));

        WriteUnitOfWork wuow(operationContext());
        int64_t numKeys = 0;
        ASSERT_OK(interceptor->sideWrite(operationContext(),
                                         *_coll.get(),
                                         entry,
                                         {keyStrings.back()},
                                         {},
                                         {},
                                         IndexBuildInterceptor::Op::kInsert,
                                         &numKeys));
        ASSERT_EQ(1, numKeys);
        wuow.commit();
    }

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain all side writes — with batch size 1 this should produce multiple WUOWs.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kNoTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // Each side write should produce one index insert and one side-table delete.
    ASSERT_EQ(observer->countInsertsFor(entry->getIdent(), insertsBefore),
              static_cast<size_t>(kNumSideWrites));
    ASSERT_EQ(observer->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore),
              static_cast<size_t>(kNumSideWrites));

    // Verify side writes table is empty and all keys are in the index.
    ASSERT_EQ(0, getSideWritesTableContents(indexBuildInfo).size());

    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
    auto indexCursor = entry->accessMethod()->asSortedData()->newCursor(operationContext(), ru);
    for (int i = 0; i < kNumSideWrites; ++i) {
        ASSERT(indexCursor->seekForKeyString(ru, keyStrings[i].getView()));
    }
}

TEST_F(IndexBuilderInterceptorTest,
       DrainDuplicateInsertOnUniqueIndexRecordsConstraintViolationPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observerPtr = installContainerOpObserver(operationContext());

    auto indexBuildInfo =
        buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");
    auto indexAccessMethod = entry->accessMethod()->asSortedData();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());

    // Build two key strings with the same index key value (10) but different RecordIds.
    auto existingKeyString = makeKeyString(10, RecordId{1});
    auto dupKeyString = makeKeyString(10, RecordId{2});

    // Pre-populate the index with the existing key.
    {
        KeyStringSet keySet;
        keySet.insert(existingKeyString);
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

    // Buffer a side write that inserts a duplicate of the existing key.
    {
        WriteUnitOfWork wuow(operationContext());
        int64_t numKeys = 0;
        ASSERT_OK(interceptor->sideWrite(operationContext(),
                                         *_coll.get(),
                                         entry,
                                         {dupKeyString},
                                         {},
                                         {},
                                         IndexBuildInterceptor::Op::kInsert,
                                         &numKeys));
        ASSERT_EQ(1, numKeys);
        wuow.commit();
    }

    // Record observer state before drain so we can isolate drain-phase calls.
    const size_t insertsBefore = observerPtr->inserts.size();
    const size_t deletesBefore = observerPtr->deletes.size();

    // Drain with TrackDuplicates::kTrack — duplicates should be recorded to the constraint
    // violations table rather than causing a failure.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // Verify the drain produced the expected container operations:
    //   - 2 inserts: one into the index, one into the constraint violations table
    //   - 1 delete: removing the consumed side write
    ASSERT_EQ(observerPtr->countInsertsFor(entry->getIdent(), insertsBefore), 1u);
    ASSERT_EQ(
        observerPtr->countInsertsFor(*indexBuildInfo.constraintViolationsIdent, insertsBefore), 1u);
    ASSERT_EQ(observerPtr->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore), 1u);

    // Verify the duplicate was recorded in the constraint violations table.
    ASSERT_EQ(1, getDuplicateKeyTableContents(indexBuildInfo).size());

    // Verify the side writes table is empty after draining.
    ASSERT_EQ(0, getSideWritesTableContents(indexBuildInfo).size());

    // Verify both keys (original and duplicate) are in the index.
    auto indexCursor = indexAccessMethod->newCursor(operationContext(), ru);
    ASSERT(indexCursor->seekForKeyString(ru, existingKeyString.getView()));
    ASSERT(indexCursor->seekForKeyString(ru, dupKeyString.getView()));
}

TEST_F(IndexBuilderInterceptorTest,
       DrainInsertOnUniqueIndexWithNoDuplicateGeneratesNoConstraintViolationPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo =
        buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");

    auto keyString = makeKeyString(10, RecordId{1});

    // Buffer a side write (insert) — no pre-existing key in the index, so no duplicate.
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

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain with kTrack on a unique index — but since there's no duplicate, the constraint
    // violations table should not be written to.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // Same as non-unique: one CI for the index, one CD for the side writes table.
    ASSERT_EQ(observer->countInsertsFor(entry->getIdent(), insertsBefore), 1u);
    ASSERT_EQ(observer->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore), 1u);

    // No writes to the constraint violations table.
    ASSERT_EQ(observer->countInsertsFor(*indexBuildInfo.constraintViolationsIdent, insertsBefore),
              0u);

    ASSERT_EQ(0, getDuplicateKeyTableContents(indexBuildInfo).size());
    ASSERT_EQ(0, getSideWritesTableContents(indexBuildInfo).size());
}

TEST_F(IndexBuilderInterceptorTest, DrainDeleteOnUniqueIndexGeneratesContainerOpsPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo =
        buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");
    auto indexAccessMethod = entry->accessMethod()->asSortedData();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());

    auto keyString = makeKeyString(10, RecordId{1});
    KeyStringSet keySet;
    keySet.insert(keyString);

    // Pre-populate the index with the key so the delete side write has something to remove.
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

    // Buffer a side write (delete).
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

    const size_t deletesBefore = observer->deletes.size();

    // Drain with kTrack on a unique index — delete path is unaffected by uniqueness.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // Expect two container deletes: one for the index key, one for the side writes table.
    ASSERT_EQ(observer->countDeletesFor(entry->getIdent(), deletesBefore), 1u);
    ASSERT_EQ(observer->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore), 1u);

    ASSERT_EQ(0, getSideWritesTableContents(indexBuildInfo).size());
}

TEST_F(IndexBuilderInterceptorTest,
       DrainDuplicateInsertOnUniqueIndexWithNoTrackGeneratesNoConstraintViolationPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo =
        buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");
    auto indexAccessMethod = entry->accessMethod()->asSortedData();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());

    // Build two key strings with the same index key value but different RecordIds.
    auto existingKeyString = makeKeyString(10, RecordId{1});
    auto dupKeyString = makeKeyString(10, RecordId{2});

    // Pre-populate the index with the existing key.
    {
        KeyStringSet keySet;
        keySet.insert(existingKeyString);
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

    // Buffer a side write that inserts a duplicate of the existing key.
    {
        WriteUnitOfWork wuow(operationContext());
        int64_t numKeys = 0;
        ASSERT_OK(interceptor->sideWrite(operationContext(),
                                         *_coll.get(),
                                         entry,
                                         {dupKeyString},
                                         {},
                                         {},
                                         IndexBuildInterceptor::Op::kInsert,
                                         &numKeys));
        ASSERT_EQ(1, numKeys);
        wuow.commit();
    }

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain with kNoTrack — duplicate is silently swallowed, not recorded to constraint
    // violations table.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kNoTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // The duplicate key is still inserted into the index, but not recorded as a violation.
    ASSERT_EQ(observer->countInsertsFor(entry->getIdent(), insertsBefore), 1u);
    ASSERT_EQ(observer->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore), 1u);

    // No writes to the constraint violations table.
    ASSERT_EQ(observer->countInsertsFor(*indexBuildInfo.constraintViolationsIdent, insertsBefore),
              0u);

    ASSERT_EQ(0, getDuplicateKeyTableContents(indexBuildInfo).size());
    ASSERT_EQ(0, getSideWritesTableContents(indexBuildInfo).size());
}

TEST_F(IndexBuilderInterceptorTest,
       DrainDuplicateInsertOnPrepareUniqueIndexReturnsErrorPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo = buildIndexBuildInfo(
        fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true, prepareUnique: true}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");
    auto indexAccessMethod = entry->accessMethod()->asSortedData();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());

    // Build two key strings with the same index key value but different RecordIds.
    auto existingKeyString = makeKeyString(10, RecordId{1});
    auto dupKeyString = makeKeyString(10, RecordId{2});

    // Pre-populate the index with the existing key.
    {
        KeyStringSet keySet;
        keySet.insert(existingKeyString);
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

    // Buffer a side write that inserts a duplicate of the existing key.
    {
        WriteUnitOfWork wuow(operationContext());
        int64_t numKeys = 0;
        ASSERT_OK(interceptor->sideWrite(operationContext(),
                                         *_coll.get(),
                                         entry,
                                         {dupKeyString},
                                         {},
                                         {},
                                         IndexBuildInterceptor::Op::kInsert,
                                         &numKeys));
        ASSERT_EQ(1, numKeys);
        wuow.commit();
    }

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain with kTrack on a prepareUnique index — duplicate should cause a DuplicateKey error
    // rather than being recorded to the constraint violations table, because prepareUnique
    // forces the error path even when options.dupsAllowed is true.
    auto status =
        interceptor->drainWritesIntoIndex(operationContext(),
                                          *_coll.get(),
                                          entry,
                                          InsertDeleteOptions{.dupsAllowed = true},
                                          IndexBuildInterceptor::TrackDuplicates::kTrack,
                                          IndexBuildInterceptor::DrainYieldPolicy::kNoYield);
    ASSERT_EQ(status.code(), ErrorCodes::DuplicateKey);

    // No container ops should have been generated for the failed drain — the error is returned
    // before the container_write::insert() call.
    ASSERT_EQ(observer->countInsertsFor(entry->getIdent(), insertsBefore), 0u);
    ASSERT_EQ(observer->countInsertsFor(*indexBuildInfo.constraintViolationsIdent, insertsBefore),
              0u);
    ASSERT_EQ(observer->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore), 0u);
}

TEST_F(IndexBuilderInterceptorTest,
       DrainSameKeyAndRecordIdInsertOnUniqueIndexIsIdempotentPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo =
        buildIndexBuildInfo(fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");
    auto indexAccessMethod = entry->accessMethod()->asSortedData();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());

    // Use the exact same key value AND RecordId for both the pre-populated key and the side write.
    auto keyString = makeKeyString(10, RecordId{1});

    // Pre-populate the index with the key.
    {
        KeyStringSet keySet;
        keySet.insert(keyString);
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

    // Buffer a side write that re-inserts the exact same (key, RecordId) pair.
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

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain with kTrack — the container_write::insert returns KeyExists because the exact
    // (key, RecordId) already exists, which is converted to OK. The onDuplicateKey callback
    // is NOT invoked, so no constraint violation is recorded. This is an idempotent re-insert,
    // not a true duplicate.
    ASSERT_OK(interceptor->drainWritesIntoIndex(operationContext(),
                                                *_coll.get(),
                                                entry,
                                                InsertDeleteOptions{.dupsAllowed = true},
                                                IndexBuildInterceptor::TrackDuplicates::kTrack,
                                                IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // The container_write::insert returns KeyExists (the exact key+RecordId is already present),
    // which is silently converted to OK. No actual index insert op is generated because the key
    // was already there. No constraint violation is recorded — this is the key distinction from
    // the different-RecordId duplicate case.
    ASSERT_EQ(observer->countInsertsFor(entry->getIdent(), insertsBefore), 0u);
    ASSERT_EQ(observer->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore), 1u);

    ASSERT_EQ(observer->countInsertsFor(*indexBuildInfo.constraintViolationsIdent, insertsBefore),
              0u);

    ASSERT_EQ(0, getDuplicateKeyTableContents(indexBuildInfo).size());
    ASSERT_EQ(0, getSideWritesTableContents(indexBuildInfo).size());
}

TEST_F(IndexBuilderInterceptorTest,
       DrainDuplicateInsertOnPrepareUniqueIndexWithNoTrackReturnsErrorPrimaryDriven) {
    // TODO (SERVER-116165): Remove.
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);
    RAIIServerParameterControllerForTest ffPDIB("featureFlagPrimaryDrivenIndexBuilds", true);
    auto* observer = installContainerOpObserver(operationContext());

    auto indexBuildInfo = buildIndexBuildInfo(
        fromjson("{v: 2, name: 'a_1', key: {a: 1}, unique: true, prepareUnique: true}"));
    auto interceptor =
        createIndexBuildInterceptor(indexBuildInfo, LazyRecordStore::CreateMode::immediate);
    const auto entry = getIndexEntry("a_1");
    auto indexAccessMethod = entry->accessMethod()->asSortedData();
    auto& ru = *shard_role_details::getRecoveryUnit(operationContext());

    auto existingKeyString = makeKeyString(10, RecordId{1});
    auto dupKeyString = makeKeyString(10, RecordId{2});

    // Pre-populate the index with the existing key.
    {
        KeyStringSet keySet;
        keySet.insert(existingKeyString);
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

    // Buffer a side write that inserts a duplicate of the existing key.
    {
        WriteUnitOfWork wuow(operationContext());
        int64_t numKeys = 0;
        ASSERT_OK(interceptor->sideWrite(operationContext(),
                                         *_coll.get(),
                                         entry,
                                         {dupKeyString},
                                         {},
                                         {},
                                         IndexBuildInterceptor::Op::kInsert,
                                         &numKeys));
        ASSERT_EQ(1, numKeys);
        wuow.commit();
    }

    const size_t insertsBefore = observer->inserts.size();
    const size_t deletesBefore = observer->deletes.size();

    // Drain with kNoTrack on a prepareUnique index — prepareUnique forces the DuplicateKey error
    // path regardless of the TrackDuplicates mode.
    auto status =
        interceptor->drainWritesIntoIndex(operationContext(),
                                          *_coll.get(),
                                          entry,
                                          InsertDeleteOptions{.dupsAllowed = true},
                                          IndexBuildInterceptor::TrackDuplicates::kNoTrack,
                                          IndexBuildInterceptor::DrainYieldPolicy::kNoYield);
    ASSERT_EQ(status.code(), ErrorCodes::DuplicateKey);

    // No container ops — error fires before any writes.
    ASSERT_EQ(observer->countInsertsFor(entry->getIdent(), insertsBefore), 0u);
    ASSERT_EQ(observer->countInsertsFor(*indexBuildInfo.constraintViolationsIdent, insertsBefore),
              0u);
    ASSERT_EQ(observer->countDeletesFor(*indexBuildInfo.sideWritesIdent, deletesBefore), 0u);
}

}  // namespace
}  // namespace mongo
