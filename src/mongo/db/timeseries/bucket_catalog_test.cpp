/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {
constexpr StringData kNumSchemaChanges = "numBucketsClosedDueToSchemaChange"_sd;
constexpr StringData kNumBucketsReopened = "numBucketsReopened"_sd;
constexpr StringData kNumArchivedDueToTimeForward = "numBucketsArchivedDueToTimeForward"_sd;
constexpr StringData kNumArchivedDueToTimeBackward = "numBucketsArchivedDueToTimeBackward"_sd;
constexpr StringData kNumArchivedDueToMemoryThreshold = "numBucketsArchivedDueToMemoryThreshold"_sd;
constexpr StringData kNumArchivedDueToReopening = "numBucketsArchivedDueToReopening"_sd;
constexpr StringData kNumClosedDueToTimeForward = "numBucketsClosedDueToTimeForward"_sd;
constexpr StringData kNumClosedDueToTimeBackward = "numBucketsClosedDueToTimeBackward"_sd;
constexpr StringData kNumClosedDueToMemoryThreshold = "numBucketsClosedDueToMemoryThreshold"_sd;

class BucketCatalogTest : public CatalogTestFixture {
protected:
    class Task {
        AtomicWord<bool> _running{false};
        stdx::packaged_task<void()> _task;
        stdx::future<void> _future;
        stdx::thread _taskThread;

    public:
        Task(std::function<void()>&& fn);
        ~Task();

        const stdx::future<void>& future();
    };

    void setUp() override;

    std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
    _makeOperationContext();

    virtual BSONObj _makeTimeseriesOptionsForCreate() const;

    TimeseriesOptions _getTimeseriesOptions(const NamespaceString& ns) const;
    const CollatorInterface* _getCollator(const NamespaceString& ns) const;

    void _commit(const std::shared_ptr<BucketCatalog::WriteBatch>& batch,
                 uint16_t numPreviouslyCommittedMeasurements,
                 size_t expectedBatchSize = 1);
    void _insertOneAndCommit(const NamespaceString& ns,
                             uint16_t numPreviouslyCommittedMeasurements);

    long long _getExecutionStat(const NamespaceString& ns, StringData stat);

    // Check that each group of objects has compatible schema with itself, but that inserting the
    // first object in new group closes the existing bucket and opens a new one
    void _testMeasurementSchema(
        const std::initializer_list<std::initializer_list<BSONObj>>& groups);

    OperationContext* _opCtx;
    BucketCatalog* _bucketCatalog;

    StringData _timeField = "time";
    StringData _metaField = "meta";

    NamespaceString _ns1{"bucket_catalog_test_1", "t_1"};
    NamespaceString _ns2{"bucket_catalog_test_1", "t_2"};
    NamespaceString _ns3{"bucket_catalog_test_2", "t_1"};
};

class BucketCatalogWithoutMetadataTest : public BucketCatalogTest {
protected:
    BSONObj _makeTimeseriesOptionsForCreate() const override;
};

BucketCatalogTest::Task::Task(std::function<void()>&& fn)
    : _task{[this, fn = std::move(fn)]() {
          _running.store(true);
          fn();
      }},
      _future{_task.get_future()},
      _taskThread{std::move(_task)} {
    while (!_running.load()) {
        stdx::this_thread::yield();
    }
}
BucketCatalogTest::Task::~Task() {
    _taskThread.join();
}

const stdx::future<void>& BucketCatalogTest::Task::future() {
    return _future;
}

void BucketCatalogTest::setUp() {
    CatalogTestFixture::setUp();

    _opCtx = operationContext();
    _bucketCatalog = &BucketCatalog::get(_opCtx);

    for (const auto& ns : {_ns1, _ns2, _ns3}) {
        ASSERT_OK(createCollection(
            _opCtx,
            ns.dbName(),
            BSON("create" << ns.coll() << "timeseries" << _makeTimeseriesOptionsForCreate())));
    }
}

std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
BucketCatalogTest::_makeOperationContext() {
    auto client = getServiceContext()->makeClient("BucketCatalogTest");
    auto opCtx = client->makeOperationContext();
    return {std::move(client), std::move(opCtx)};
}

BSONObj BucketCatalogTest::_makeTimeseriesOptionsForCreate() const {
    return BSON("timeField" << _timeField << "metaField" << _metaField);
}

BSONObj BucketCatalogWithoutMetadataTest::_makeTimeseriesOptionsForCreate() const {
    return BSON("timeField" << _timeField);
}

TimeseriesOptions BucketCatalogTest::_getTimeseriesOptions(const NamespaceString& ns) const {
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    return *autoColl->getTimeseriesOptions();
}

const CollatorInterface* BucketCatalogTest::_getCollator(const NamespaceString& ns) const {
    AutoGetCollection autoColl(_opCtx, ns.makeTimeseriesBucketsNamespace(), MODE_IS);
    return autoColl->getDefaultCollator();
}

void BucketCatalogTest::_commit(const std::shared_ptr<BucketCatalog::WriteBatch>& batch,
                                uint16_t numPreviouslyCommittedMeasurements,
                                size_t expectedBatchSize) {
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), expectedBatchSize);
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements(), numPreviouslyCommittedMeasurements);

    _bucketCatalog->finish(batch, {});
}

void BucketCatalogTest::_insertOneAndCommit(const NamespaceString& ns,
                                            uint16_t numPreviouslyCommittedMeasurements) {
    auto result = _bucketCatalog->insert(_opCtx,
                                         ns,
                                         _getCollator(ns),
                                         _getTimeseriesOptions(ns),
                                         BSON(_timeField << Date_t::now()),
                                         BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    auto& batch = result.getValue().batch;
    _commit(batch, numPreviouslyCommittedMeasurements);
}

long long BucketCatalogTest::_getExecutionStat(const NamespaceString& ns, StringData stat) {
    BSONObjBuilder builder;
    _bucketCatalog->appendExecutionStats(ns, &builder);
    return builder.obj().getIntField(stat);
}

void BucketCatalogTest::_testMeasurementSchema(
    const std::initializer_list<std::initializer_list<BSONObj>>& groups) {
    // Make sure we start and end with a clean slate.
    _bucketCatalog->clear(_ns1);
    ScopeGuard guard([this]() { _bucketCatalog->clear(_ns1); });

    bool firstGroup = true;
    for (const auto& group : groups) {
        bool firstMember = true;
        for (const auto& doc : group) {
            BSONObjBuilder timestampedDoc;
            timestampedDoc.append(_timeField, Date_t::now());
            timestampedDoc.appendElements(doc);

            auto pre = _getExecutionStat(_ns1, kNumSchemaChanges);
            auto result = _bucketCatalog
                              ->insert(_opCtx,
                                       _ns1,
                                       _getCollator(_ns1),
                                       _getTimeseriesOptions(_ns1),
                                       timestampedDoc.obj(),
                                       BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                              .getValue();
            auto post = _getExecutionStat(_ns1, kNumSchemaChanges);

            if (firstMember) {
                if (firstGroup) {
                    // We don't expect to close a bucket if we are on the first group.
                    ASSERT_EQ(pre, post) << "expected " << doc << " to be compatible";
                    firstGroup = false;
                } else {
                    // Otherwise we expect that we are in fact closing a bucket because we have
                    // an incompatible schema change.
                    ASSERT_EQ(pre + 1, post) << "expected " << doc << " to be incompatible";
                }
                firstMember = false;
            } else {
                // Should have compatible schema, no expected bucket closure.
                ASSERT_EQ(pre, post) << "expected " << doc << " to be compatible";
            }
        }
    }
}

TEST_F(BucketCatalogTest, InsertIntoSameBucket) {
    // The first insert should be able to take commit rights
    auto result1 =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    auto batch1 = result1.getValue().batch;
    ASSERT(batch1->claimCommitRights());

    // A subsequent insert into the same bucket should land in the same batch, but not be able to
    // claim commit rights
    auto result2 =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    auto batch2 = result2.getValue().batch;
    ASSERT_EQ(batch1, batch2);
    ASSERT(!batch2->claimCommitRights());

    // The batch hasn't actually been committed yet.
    ASSERT(!batch1->finished());

    ASSERT_OK(_bucketCatalog->prepareCommit(batch1));

    // Still not finished.
    ASSERT(!batch1->finished());

    // The batch should contain both documents since they belong in the same bucket and happened
    // in the same commit epoch. Nothing else has been committed in this bucket yet.
    ASSERT_EQ(batch1->measurements().size(), 2);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements(), 0);

    // Once the commit has occurred, the waiter should be notified.
    _bucketCatalog->finish(batch1, {});
    ASSERT(batch2->finished());
    auto result3 = batch2->getResult();
    ASSERT_OK(result3.getStatus());
}

TEST_F(BucketCatalogTest, GetMetadataReturnsEmptyDocOnMissingBucket) {
    auto batch = _bucketCatalog
                     ->insert(_opCtx,
                              _ns1,
                              _getCollator(_ns1),
                              _getTimeseriesOptions(_ns1),
                              BSON(_timeField << Date_t::now()),
                              BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                     .getValue()
                     .batch;
    ASSERT(batch->claimCommitRights());
    auto bucket = batch->bucket();
    _bucketCatalog->abort(batch, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT_BSONOBJ_EQ(BSONObj(), _bucketCatalog->getMetadata(bucket));
}

TEST_F(BucketCatalogTest, InsertIntoDifferentBuckets) {
    auto result1 =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now() << _metaField << "123"),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    auto result2 =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now() << _metaField << BSONObj()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    auto result3 =
        _bucketCatalog->insert(_opCtx,
                               _ns2,
                               _getCollator(_ns2),
                               _getTimeseriesOptions(_ns2),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);

    // Inserts should all be into three distinct buckets (and therefore batches).
    ASSERT_NE(result1.getValue().batch, result2.getValue().batch);
    ASSERT_NE(result1.getValue().batch, result3.getValue().batch);
    ASSERT_NE(result2.getValue().batch, result3.getValue().batch);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(BSON(_metaField << "123"),
                      _bucketCatalog->getMetadata(result1.getValue().batch->bucket()));
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSONObj()),
                      _bucketCatalog->getMetadata(result2.getValue().batch->bucket()));
    ASSERT(_bucketCatalog->getMetadata(result3.getValue().batch->bucket()).isEmpty());

    // Committing one bucket should only return the one document in that bucket and should not
    // affect the other bucket.
    for (const auto& batch :
         {result1.getValue().batch, result2.getValue().batch, result3.getValue().batch}) {
        _commit(batch, 0);
    }
}

TEST_F(BucketCatalogTest, InsertIntoSameBucketArray) {
    auto result1 = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField << BSON_ARRAY(BSON("a" << 0 << "b" << 1))),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    auto result2 = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField << BSON_ARRAY(BSON("b" << 1 << "a" << 0))),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);

    ASSERT_EQ(result1.getValue().batch, result2.getValue().batch);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSON_ARRAY(BSON("a" << 0 << "b" << 1))),
                      _bucketCatalog->getMetadata(result1.getValue().batch->bucket()));
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSON_ARRAY(BSON("a" << 0 << "b" << 1))),
                      _bucketCatalog->getMetadata(result2.getValue().batch->bucket()));
}

TEST_F(BucketCatalogTest, InsertIntoSameBucketObjArray) {
    auto result1 = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField
                        << BSONObj(BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1)
                                                          << BSON("f" << 1 << "g" << 0))))),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    auto result2 = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField
                        << BSONObj(BSON("c" << BSON_ARRAY(BSON("b" << 1 << "a" << 0)
                                                          << BSON("g" << 0 << "f" << 1))))),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);

    ASSERT_EQ(result1.getValue().batch, result2.getValue().batch);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << BSONObj(BSON(
                 "c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1) << BSON("f" << 1 << "g" << 0))))),
        _bucketCatalog->getMetadata(result1.getValue().batch->bucket()));
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << BSONObj(BSON(
                 "c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1) << BSON("f" << 1 << "g" << 0))))),
        _bucketCatalog->getMetadata(result2.getValue().batch->bucket()));
}


TEST_F(BucketCatalogTest, InsertIntoSameBucketNestedArray) {
    auto result1 = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField
                        << BSONObj(BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1)
                                                          << BSON_ARRAY("123"
                                                                        << "456"))))),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    auto result2 = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField
                        << BSONObj(BSON("c" << BSON_ARRAY(BSON("b" << 1 << "a" << 0)
                                                          << BSON_ARRAY("123"
                                                                        << "456"))))),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);

    ASSERT_EQ(result1.getValue().batch, result2.getValue().batch);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSONObj(BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1)
                                                                        << BSON_ARRAY("123"
                                                                                      << "456"))))),
                      _bucketCatalog->getMetadata(result1.getValue().batch->bucket()));
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSONObj(BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1)
                                                                        << BSON_ARRAY("123"
                                                                                      << "456"))))),
                      _bucketCatalog->getMetadata(result2.getValue().batch->bucket()));
}

TEST_F(BucketCatalogTest, InsertNullAndMissingMetaFieldIntoDifferentBuckets) {
    auto result1 =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now() << _metaField << BSONNULL),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    auto result2 =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);

    // Inserts should all be into three distinct buckets (and therefore batches).
    ASSERT_NE(result1.getValue().batch, result2.getValue().batch);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSONNULL),
                      _bucketCatalog->getMetadata(result1.getValue().batch->bucket()));
    ASSERT(_bucketCatalog->getMetadata(result2.getValue().batch->bucket()).isEmpty());

    // Committing one bucket should only return the one document in that bucket and should not
    // affect the other bucket.
    for (const auto& batch : {result1.getValue().batch, result2.getValue().batch}) {
        _commit(batch, 0);
    }
}

TEST_F(BucketCatalogTest, NumCommittedMeasurementsAccumulates) {
    // The numCommittedMeasurements returned when committing should accumulate as more entries in
    // the bucket are committed.
    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns1, 1);
}

TEST_F(BucketCatalogTest, ClearNamespaceBuckets) {
    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 0);

    _bucketCatalog->clear(_ns1);

    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 1);
}

TEST_F(BucketCatalogTest, ClearDatabaseBuckets) {
    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 0);
    _insertOneAndCommit(_ns3, 0);

    _bucketCatalog->clear(_ns1.db());

    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 0);
    _insertOneAndCommit(_ns3, 1);
}

TEST_F(BucketCatalogTest, InsertBetweenPrepareAndFinish) {
    auto batch1 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                      .getValue()
                      .batch;
    ASSERT(batch1->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch1));
    ASSERT_EQ(batch1->measurements().size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements(), 0);

    // Insert before finish so there's a second batch live at the same time.
    auto batch2 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                      .getValue()
                      .batch;
    ASSERT_NE(batch1, batch2);

    _bucketCatalog->finish(batch1, {});
    ASSERT(batch1->finished());

    // Verify the second batch still commits one doc, and that the first batch only commited one.
    _commit(batch2, 1);
}

DEATH_TEST_F(BucketCatalogTest, CannotCommitWithoutRights, "invariant") {
    auto result = _bucketCatalog->insert(_opCtx,
                                         _ns1,
                                         _getCollator(_ns1),
                                         _getTimeseriesOptions(_ns1),
                                         BSON(_timeField << Date_t::now()),
                                         BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    auto& batch = result.getValue().batch;
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));

    // BucketCatalog::prepareCommit uses dassert, so it will only invariant in debug mode. Ensure we
    // die here in non-debug mode as well.
    invariant(kDebugBuild);
}

TEST_F(BucketCatalogWithoutMetadataTest, GetMetadataReturnsEmptyDoc) {
    auto batch = _bucketCatalog
                     ->insert(_opCtx,
                              _ns1,
                              _getCollator(_ns1),
                              _getTimeseriesOptions(_ns1),
                              BSON(_timeField << Date_t::now()),
                              BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                     .getValue()
                     .batch;

    ASSERT_BSONOBJ_EQ(BSONObj(), _bucketCatalog->getMetadata(batch->bucket()));

    _commit(batch, 0);
}

TEST_F(BucketCatalogWithoutMetadataTest, CommitReturnsNewFields) {
    // Creating a new bucket should return all fields from the initial measurement.
    auto result = _bucketCatalog->insert(_opCtx,
                                         _ns1,
                                         _getCollator(_ns1),
                                         _getTimeseriesOptions(_ns1),
                                         BSON(_timeField << Date_t::now() << "a" << 0),
                                         BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT(result.isOK());
    auto batch = result.getValue().batch;
    auto oldId = batch->bucket().id;
    _commit(batch, 0);
    ASSERT_EQ(2U, batch->newFieldNamesToBeInserted().size()) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted().count(_timeField)) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted().count("a")) << batch->toBSON();

    // Inserting a new measurement with the same fields should return an empty set of new fields.

    result = _bucketCatalog->insert(_opCtx,
                                    _ns1,
                                    _getCollator(_ns1),
                                    _getTimeseriesOptions(_ns1),
                                    BSON(_timeField << Date_t::now() << "a" << 1),
                                    BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT(result.isOK());
    batch = result.getValue().batch;
    _commit(batch, 1);
    ASSERT_EQ(0U, batch->newFieldNamesToBeInserted().size()) << batch->toBSON();

    // Insert a new measurement with the a new field.
    result = _bucketCatalog->insert(_opCtx,
                                    _ns1,
                                    _getCollator(_ns1),
                                    _getTimeseriesOptions(_ns1),
                                    BSON(_timeField << Date_t::now() << "a" << 2 << "b" << 2),
                                    BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT(result.isOK());
    batch = result.getValue().batch;
    _commit(batch, 2);
    ASSERT_EQ(1U, batch->newFieldNamesToBeInserted().size()) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted().count("b")) << batch->toBSON();

    // Fill up the bucket.
    for (auto i = 3; i < gTimeseriesBucketMaxCount; ++i) {
        result = _bucketCatalog->insert(_opCtx,
                                        _ns1,
                                        _getCollator(_ns1),
                                        _getTimeseriesOptions(_ns1),
                                        BSON(_timeField << Date_t::now() << "a" << i),
                                        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
        ASSERT(result.isOK());
        batch = result.getValue().batch;
        _commit(batch, i);
        ASSERT_EQ(0U, batch->newFieldNamesToBeInserted().size()) << i << ":" << batch->toBSON();
    }

    // When a bucket overflows, committing to the new overflow bucket should return the fields of
    // the first measurement as new fields.
    auto result2 = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << "a" << gTimeseriesBucketMaxCount),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    auto& batch2 = result2.getValue().batch;
    ASSERT_NE(oldId, batch2->bucket().id);
    _commit(batch2, 0);
    ASSERT_EQ(2U, batch2->newFieldNamesToBeInserted().size()) << batch2->toBSON();
    ASSERT(batch2->newFieldNamesToBeInserted().count(_timeField)) << batch2->toBSON();
    ASSERT(batch2->newFieldNamesToBeInserted().count("a")) << batch2->toBSON();
}

TEST_F(BucketCatalogTest, AbortBatchOnBucketWithPreparedCommit) {
    auto batch1 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                      .getValue()
                      .batch;
    ASSERT(batch1->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch1));
    ASSERT_EQ(batch1->measurements().size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements(), 0);

    // Insert before finish so there's a second batch live at the same time.
    auto batch2 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                      .getValue()
                      .batch;
    ASSERT_NE(batch1, batch2);

    ASSERT(batch2->claimCommitRights());
    _bucketCatalog->abort(batch2, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(batch2->finished());
    ASSERT_EQ(batch2->getResult().getStatus(), ErrorCodes::TimeseriesBucketCleared);

    _bucketCatalog->finish(batch1, {});
    ASSERT(batch1->finished());
    ASSERT_OK(batch1->getResult().getStatus());
}

TEST_F(BucketCatalogTest, ClearNamespaceWithConcurrentWrites) {
    auto batch = _bucketCatalog
                     ->insert(_opCtx,
                              _ns1,
                              _getCollator(_ns1),
                              _getTimeseriesOptions(_ns1),
                              BSON(_timeField << Date_t::now()),
                              BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                     .getValue()
                     .batch;
    ASSERT(batch->claimCommitRights());

    _bucketCatalog->clear(_ns1);

    ASSERT_NOT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT(batch->finished());
    ASSERT_EQ(batch->getResult().getStatus(), ErrorCodes::TimeseriesBucketCleared);

    batch = _bucketCatalog
                ->insert(_opCtx,
                         _ns1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         BSON(_timeField << Date_t::now()),
                         BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                .getValue()
                .batch;
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements(), 0);

    _bucketCatalog->clear(_ns1);

    // Even though bucket has been cleared, finish should still report success. Basically, in this
    // case we know that the write succeeded, so it must have happened before the namespace drop
    // operation got the collection lock. So the write did actually happen, but is has since been
    // removed, and that's fine for our purposes. The finish just records the result to the batch
    // and updates some statistics.
    _bucketCatalog->finish(batch, {});
    ASSERT(batch->finished());
    ASSERT_OK(batch->getResult().getStatus());
}


TEST_F(BucketCatalogTest, ClearBucketWithPreparedBatchThrowsConflict) {
    auto batch = _bucketCatalog
                     ->insert(_opCtx,
                              _ns1,
                              _getCollator(_ns1),
                              _getTimeseriesOptions(_ns1),
                              BSON(_timeField << Date_t::now()),
                              BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                     .getValue()
                     .batch;
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements(), 0);

    ASSERT_THROWS(_bucketCatalog->clear(batch->bucket().id), WriteConflictException);

    _bucketCatalog->abort(batch, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(batch->finished());
    ASSERT_EQ(batch->getResult().getStatus(), ErrorCodes::TimeseriesBucketCleared);
}

TEST_F(BucketCatalogTest, PrepareCommitOnClearedBatchWithAlreadyPreparedBatch) {
    auto batch1 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                      .getValue()
                      .batch;
    ASSERT(batch1->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch1));
    ASSERT_EQ(batch1->measurements().size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements(), 0);

    // Insert before clear so there's a second batch live at the same time.
    auto batch2 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                      .getValue()
                      .batch;
    ASSERT_NE(batch1, batch2);
    ASSERT_EQ(batch1->bucket().id, batch2->bucket().id);

    // Now clear the bucket. Since there's a prepared batch it should conflict.
    ASSERT_THROWS(_bucketCatalog->clear(batch1->bucket().id), WriteConflictException);

    // Now try to prepare the second batch. Ensure it aborts the batch.
    ASSERT(batch2->claimCommitRights());
    ASSERT_NOT_OK(_bucketCatalog->prepareCommit(batch2));
    ASSERT(batch2->finished());
    ASSERT_EQ(batch2->getResult().getStatus(), ErrorCodes::TimeseriesBucketCleared);

    // Make sure we didn't clear the bucket state when we aborted the second batch.
    ASSERT_THROWS(_bucketCatalog->clear(batch1->bucket().id), WriteConflictException);

    // Make sure a subsequent insert, which opens a new bucket, doesn't corrupt the old bucket
    // state and prevent us from finishing the first batch.
    auto batch3 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                      .getValue()
                      .batch;
    ASSERT_NE(batch1, batch3);
    ASSERT_NE(batch2, batch3);
    ASSERT_NE(batch1->bucket().id, batch3->bucket().id);
    // Clean up this batch
    ASSERT(batch3->claimCommitRights());
    _bucketCatalog->abort(batch3, {ErrorCodes::TimeseriesBucketCleared, ""});

    // Make sure we can finish the cleanly prepared batch.
    _bucketCatalog->finish(batch1, {});
    ASSERT(batch1->finished());
    ASSERT_OK(batch1->getResult().getStatus());
}

TEST_F(BucketCatalogTest, PrepareCommitOnAlreadyAbortedBatch) {
    auto batch = _bucketCatalog
                     ->insert(_opCtx,
                              _ns1,
                              _getCollator(_ns1),
                              _getTimeseriesOptions(_ns1),
                              BSON(_timeField << Date_t::now()),
                              BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                     .getValue()
                     .batch;
    ASSERT(batch->claimCommitRights());

    _bucketCatalog->abort(batch, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(batch->finished());
    ASSERT_EQ(batch->getResult().getStatus(), ErrorCodes::TimeseriesBucketCleared);

    ASSERT_NOT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT(batch->finished());
    ASSERT_EQ(batch->getResult().getStatus(), ErrorCodes::TimeseriesBucketCleared);
}

TEST_F(BucketCatalogTest, CombiningWithInsertsFromOtherClients) {
    auto batch1 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow)
                      .getValue()
                      .batch;

    auto batch2 = _bucketCatalog
                      ->insert(_makeOperationContext().second.get(),
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow)
                      .getValue()
                      .batch;

    auto batch3 = _bucketCatalog
                      ->insert(_makeOperationContext().second.get(),
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                      .getValue()
                      .batch;

    auto batch4 = _bucketCatalog
                      ->insert(_makeOperationContext().second.get(),
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                      .getValue()
                      .batch;

    ASSERT_NE(batch1, batch2);
    ASSERT_NE(batch1, batch3);
    ASSERT_NE(batch2, batch3);
    ASSERT_EQ(batch3, batch4);

    _commit(batch1, 0);
    _commit(batch2, 1);
    _commit(batch3, 2, 2);
}

TEST_F(BucketCatalogTest, CannotConcurrentlyCommitBatchesForSameBucket) {
    auto batch1 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow)
                      .getValue()
                      .batch;

    auto batch2 = _bucketCatalog
                      ->insert(_makeOperationContext().second.get(),
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow)
                      .getValue()
                      .batch;

    ASSERT(batch1->claimCommitRights());
    ASSERT(batch2->claimCommitRights());

    // Batch 2 will not be able to commit until batch 1 has finished.
    ASSERT_OK(_bucketCatalog->prepareCommit(batch1));
    auto task = Task{[&]() { ASSERT_OK(_bucketCatalog->prepareCommit(batch2)); }};
    // Add a little extra wait to make sure prepareCommit actually gets to the blocking point.
    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(10));
    ASSERT(task.future().valid());
    ASSERT(stdx::future_status::timeout == task.future().wait_for(stdx::chrono::microseconds(1)))
        << "prepareCommit finished before expected";

    _bucketCatalog->finish(batch1, {});
    task.future().wait();
    _bucketCatalog->finish(batch2, {});
}

TEST_F(BucketCatalogTest, DuplicateNewFieldNamesAcrossConcurrentBatches) {
    auto batch1 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow)
                      .getValue()
                      .batch;

    auto batch2 = _bucketCatalog
                      ->insert(_makeOperationContext().second.get(),
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow)
                      .getValue()
                      .batch;

    // Batch 2 is the first batch to commit the time field.
    ASSERT(batch2->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch2));
    ASSERT_EQ(batch2->newFieldNamesToBeInserted().size(), 1);
    ASSERT_EQ(batch2->newFieldNamesToBeInserted().begin()->first, _timeField);
    _bucketCatalog->finish(batch2, {});

    // Batch 1 was the first batch to insert the time field, but by commit time it was already
    // committed by batch 2.
    ASSERT(batch1->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch1));
    ASSERT(batch1->newFieldNamesToBeInserted().empty());
    _bucketCatalog->finish(batch1, {});
}

TEST_F(BucketCatalogTest, SchemaChanges) {
    std::vector<BSONObj> docs = {
        ::mongo::fromjson(R"({a: 1})"),                                // 0
        ::mongo::fromjson(R"({a: true})"),                             // 1
        ::mongo::fromjson(R"({a: {}})"),                               // 2
        ::mongo::fromjson(R"({a: {b: 1}})"),                           // 3
        ::mongo::fromjson(R"({a: {b: true}})"),                        // 4
        ::mongo::fromjson(R"({a: {c: true}})"),                        // 5
        ::mongo::fromjson(R"({a: {d: true}})"),                        // 6
        ::mongo::fromjson(R"({a: {e: true}})"),                        // 7
        ::mongo::fromjson(R"({a: {f: true}})"),                        // 8
        ::mongo::fromjson(R"({a: {d: 1.0}})"),                         // 9
        ::mongo::fromjson(R"({b: 1.0})"),                              // 10
        ::mongo::fromjson(R"({c: {}})"),                               // 11
        ::mongo::fromjson(R"({a: 1.0, b: 2.0, c: 3.0})"),              // 12
        ::mongo::fromjson(R"({c: 1.0, b: 3.0, a: 2.0})"),              // 13
        ::mongo::fromjson(R"({b: 1.0, a: 3.0, c: 2.0})"),              // 14
        ::mongo::fromjson(R"({a: {b: [1.0, 2.0]}})"),                  // 15
        ::mongo::fromjson(R"({a: {b: [true, false]}})"),               // 16
        ::mongo::fromjson(R"({a: {b: [false, true, false, true]}})"),  // 17
        ::mongo::fromjson(R"({a: {b: [{a: true}, {b: false}]}})"),     // 18
        ::mongo::fromjson(R"({a: {b: [{b: true}, {a: false}]}})"),     // 19
        ::mongo::fromjson(R"({a: {b: [{a: 1.0}, {b: 2.0}]}})"),        // 20
        ::mongo::fromjson(R"({a: {b: [{}, {}, true, false]}})"),       // 21
    };

    _testMeasurementSchema({{docs[0]}, {docs[1]}, {docs[2], docs[3]}, {docs[4]}});
    _testMeasurementSchema({{docs[0]}, {docs[1]}, {docs[2], docs[4]}, {docs[3]}});
    _testMeasurementSchema({{docs[4], docs[5], docs[6], docs[7], docs[8]}, {docs[9]}});
    _testMeasurementSchema({{docs[4], docs[5], docs[6], docs[7], docs[8], docs[10]}});
    _testMeasurementSchema({{docs[10], docs[11]}, {docs[12]}});
    _testMeasurementSchema({{docs[12], docs[13], docs[14]}, {docs[15]}, {docs[16], docs[17]}});
    _testMeasurementSchema({{docs[18], docs[19]}, {docs[20], docs[21]}});
}

TEST_F(BucketCatalogTest, ReopenMalformedBucket) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagTimeseriesScalabilityImprovements",
                                                     true};

    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    {
        // Missing _id field.
        BSONObj missingIdObj = bucketDoc.removeField("_id");
        ASSERT_NOT_OK(_bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), missingIdObj));

        // Bad _id type.
        BSONObj badIdObj = bucketDoc.addFields(BSON("_id" << 123));
        ASSERT_NOT_OK(_bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), badIdObj));
    }

    {
        // Missing control field.
        BSONObj missingControlObj = bucketDoc.removeField("control");
        ASSERT_NOT_OK(
            _bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), missingControlObj));

        // Bad control type.
        BSONObj badControlObj = bucketDoc.addFields(BSON("control" << BSONArray()));
        ASSERT_NOT_OK(
            _bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), badControlObj));

        // Bad control.version type.
        BSONObj badVersionObj = bucketDoc.addFields(BSON(
            "control" << BSON("version" << BSONArray() << "min"
                                        << BSON("time" << BSON("$date"
                                                               << "2022-06-06T15:34:00.000Z"))
                                        << "max"
                                        << BSON("time" << BSON("$date"
                                                               << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(
            _bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), badVersionObj));

        // Bad control.min type.
        BSONObj badMinObj = bucketDoc.addFields(BSON(
            "control" << BSON("version" << 1 << "min" << 123 << "max"
                                        << BSON("time" << BSON("$date"
                                                               << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(_bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), badMinObj));

        // Bad control.max type.
        BSONObj badMaxObj = bucketDoc.addFields(
            BSON("control" << BSON("version" << 1 << "min"
                                             << BSON("time" << BSON("$date"
                                                                    << "2022-06-06T15:34:00.000Z"))
                                             << "max" << 123)));
        ASSERT_NOT_OK(_bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), badMaxObj));

        // Missing control.min.time.
        BSONObj missingMinTimeObj = bucketDoc.addFields(BSON(
            "control" << BSON("version" << 1 << "min" << BSON("abc" << 1) << "max"
                                        << BSON("time" << BSON("$date"
                                                               << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(
            _bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), missingMinTimeObj));

        // Missing control.max.time.
        BSONObj missingMaxTimeObj = bucketDoc.addFields(
            BSON("control" << BSON("version" << 1 << "min"
                                             << BSON("time" << BSON("$date"
                                                                    << "2022-06-06T15:34:00.000Z"))
                                             << "max" << BSON("abc" << 1))));
        ASSERT_NOT_OK(
            _bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), missingMaxTimeObj));
    }


    {
        // Missing data field.
        BSONObj missingDataObj = bucketDoc.removeField("data");
        ASSERT_NOT_OK(
            _bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), missingDataObj));

        // Bad data type.
        BSONObj badDataObj = bucketDoc.addFields(BSON("data" << 123));
        ASSERT_NOT_OK(_bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), badDataObj));
    }
}

TEST_F(BucketCatalogTest, ReopenUncompressedBucketAndInsertCompatibleMeasurement) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagTimeseriesScalabilityImprovements",
                                                     true};

    // Bucket document to reopen.
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    Status status = _bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), bucketDoc);
    ASSERT_OK(status);
    ASSERT_EQ(1, _getExecutionStat(_ns1, kNumBucketsReopened));

    // Insert a measurement that is compatible with the reopened bucket.
    auto result =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":-100,"b":100})"),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);

    // No buckets are closed.
    ASSERT(result.getValue().closedBuckets.empty());
    ASSERT_EQ(0, _getExecutionStat(_ns1, kNumSchemaChanges));

    auto batch = result.getValue().batch;
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);

    // The reopened bucket already contains three committed measurements.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements(), 3);

    // Verify that the min and max is updated correctly when inserting new measurements.
    ASSERT_BSONOBJ_BINARY_EQ(batch->min(), BSON("u" << BSON("a" << -100)));
    ASSERT_BSONOBJ_BINARY_EQ(
        batch->max(),
        BSON("u" << BSON("time" << Date_t::fromMillisSinceEpoch(1654529680000) << "b" << 100)));

    _bucketCatalog->finish(batch, {});
}

TEST_F(BucketCatalogTest, ReopenUncompressedBucketAndInsertIncompatibleMeasurement) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagTimeseriesScalabilityImprovements",
                                                     true};

    // Bucket document to reopen.
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    Status status = _bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), bucketDoc);
    ASSERT_OK(status);
    ASSERT_EQ(1, _getExecutionStat(_ns1, kNumBucketsReopened));

    // Insert a measurement that is incompatible with the reopened bucket.
    auto result =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":{},"b":{}})"),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);

    // The reopened bucket gets closed as the schema is incompatible.
    ASSERT_EQ(1, result.getValue().closedBuckets.size());
    ASSERT_EQ(1, _getExecutionStat(_ns1, kNumSchemaChanges));

    auto batch = result.getValue().batch;
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);

    // Since the reopened bucket was incompatible, we opened a new one.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements(), 0);

    _bucketCatalog->finish(batch, {});
}

TEST_F(BucketCatalogTest, ReopenCompressedBucketAndInsertCompatibleMeasurement) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagTimeseriesScalabilityImprovements",
                                                     true};

    // Bucket document to reopen.
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    timeseries::CompressionResult compressionResult =
        timeseries::compressBucket(bucketDoc,
                                   _timeField,
                                   _ns1,
                                   /*eligibleForReopening=*/false,
                                   /*validateDecompression=*/true);
    const BSONObj& compressedBucketDoc = compressionResult.compressedBucket.value();

    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    Status status =
        _bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), compressedBucketDoc);
    ASSERT_OK(status);
    ASSERT_EQ(1, _getExecutionStat(_ns1, kNumBucketsReopened));

    // Insert a measurement that is compatible with the reopened bucket.
    auto result =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":-100,"b":100})"),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);

    // No buckets are closed.
    ASSERT(result.getValue().closedBuckets.empty());
    ASSERT_EQ(0, _getExecutionStat(_ns1, kNumSchemaChanges));

    auto batch = result.getValue().batch;
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);

    // The reopened bucket already contains three committed measurements.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements(), 3);

    // Verify that the min and max is updated correctly when inserting new measurements.
    ASSERT_BSONOBJ_BINARY_EQ(batch->min(), BSON("u" << BSON("a" << -100)));
    ASSERT_BSONOBJ_BINARY_EQ(
        batch->max(),
        BSON("u" << BSON("time" << Date_t::fromMillisSinceEpoch(1654529680000) << "b" << 100)));

    _bucketCatalog->finish(batch, {});
}

TEST_F(BucketCatalogTest, ReopenCompressedBucketAndInsertIncompatibleMeasurement) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagTimeseriesScalabilityImprovements",
                                                     true};

    // Bucket document to reopen.
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    timeseries::CompressionResult compressionResult =
        timeseries::compressBucket(bucketDoc,
                                   _timeField,
                                   _ns1,
                                   /*eligibleForReopening=*/false,
                                   /*validateDecompression=*/true);
    const BSONObj& compressedBucketDoc = compressionResult.compressedBucket.value();

    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    Status status =
        _bucketCatalog->reopenBucket(_opCtx, autoColl.getCollection(), compressedBucketDoc);
    ASSERT_OK(status);
    ASSERT_EQ(1, _getExecutionStat(_ns1, kNumBucketsReopened));

    // Insert a measurement that is incompatible with the reopened bucket.
    auto result =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":{},"b":{}})"),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);

    // The reopened bucket gets closed as the schema is incompatible.
    ASSERT_EQ(1, result.getValue().closedBuckets.size());
    ASSERT_EQ(1, _getExecutionStat(_ns1, kNumSchemaChanges));

    auto batch = result.getValue().batch;
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);

    // Since the reopened bucket was incompatible, we opened a new one.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements(), 0);

    _bucketCatalog->finish(batch, {});
}

TEST_F(BucketCatalogTest, ArchiveIfTimeForward) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagTimeseriesScalabilityImprovements",
                                                     true};
    auto baseTimestamp = Date_t::now();

    // Insert an initial document to make sure we have an open bucket.
    auto result1 =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << baseTimestamp),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result1.getStatus());
    auto batch1 = result1.getValue().batch;
    ASSERT(batch1->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch1));
    _bucketCatalog->finish(batch1, {});

    // Make sure we start out with nothing closed or archived.
    ASSERT_EQ(0, _getExecutionStat(_ns1, kNumArchivedDueToTimeForward));
    ASSERT_EQ(0, _getExecutionStat(_ns1, kNumClosedDueToTimeForward));

    // Now insert another that's too far forward to fit in the same bucket
    auto result2 =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << (baseTimestamp + Seconds{7200})),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result2.getStatus());
    auto batch2 = result2.getValue().batch;
    ASSERT(batch2->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch2));
    _bucketCatalog->finish(batch2, {});

    // Make sure it was archived, not closed.
    ASSERT_EQ(1, _getExecutionStat(_ns1, kNumArchivedDueToTimeForward));
    ASSERT_EQ(0, _getExecutionStat(_ns1, kNumClosedDueToTimeForward));
}

TEST_F(BucketCatalogTest, ArchiveIfTimeBackward) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagTimeseriesScalabilityImprovements",
                                                     true};
    auto baseTimestamp = Date_t::now();

    // Insert an initial document to make sure we have an open bucket.
    auto result1 =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << baseTimestamp),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result1.getStatus());
    auto batch1 = result1.getValue().batch;
    ASSERT(batch1->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch1));
    _bucketCatalog->finish(batch1, {});

    // Make sure we start out with nothing closed or archived.
    ASSERT_EQ(0, _getExecutionStat(_ns1, kNumArchivedDueToTimeBackward));
    ASSERT_EQ(0, _getExecutionStat(_ns1, kNumClosedDueToTimeBackward));

    // Now insert another that's too far Backward to fit in the same bucket
    auto result2 =
        _bucketCatalog->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << (baseTimestamp - Seconds{7200})),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result2.getStatus());
    auto batch2 = result2.getValue().batch;
    ASSERT(batch2->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch2));
    _bucketCatalog->finish(batch2, {});

    // Make sure it was archived, not closed.
    ASSERT_EQ(1, _getExecutionStat(_ns1, kNumArchivedDueToTimeBackward));
    ASSERT_EQ(0, _getExecutionStat(_ns1, kNumClosedDueToTimeBackward));
}

TEST_F(BucketCatalogTest, ArchivingUnderMemoryPressure) {
    RAIIServerParameterControllerForTest featureFlag{"featureFlagTimeseriesScalabilityImprovements",
                                                     true};
    RAIIServerParameterControllerForTest memoryLimit{
        "timeseriesIdleBucketExpiryMemoryUsageThreshold", 10000};

    // Insert a measurement with a unique meta value, guaranteeing we will open a new bucket but not
    // close an old one except under memory pressure.
    long long meta = 0;
    auto insertDocument = [&meta, this]() -> BucketCatalog::ClosedBuckets {
        auto result =
            _bucketCatalog->insert(_opCtx,
                                   _ns1,
                                   _getCollator(_ns1),
                                   _getTimeseriesOptions(_ns1),
                                   BSON(_timeField << Date_t::now() << _metaField << meta++),
                                   BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
        ASSERT_OK(result.getStatus());
        auto batch = result.getValue().batch;
        ASSERT(batch->claimCommitRights());
        ASSERT_OK(_bucketCatalog->prepareCommit(batch));
        _bucketCatalog->finish(batch, {});

        return result.getValue().closedBuckets;
    };

    // Ensure we start out with no buckets archived or closed due to memory pressure.
    ASSERT_EQ(0, _getExecutionStat(_ns1, kNumArchivedDueToMemoryThreshold));
    ASSERT_EQ(0, _getExecutionStat(_ns1, kNumClosedDueToMemoryThreshold));

    // With a memory limit of 10000 bytes, we should be guaranteed to hit the memory limit with no
    // more than 1000 buckets since an open bucket takes up at least 10 bytes (in reality,
    // significantly more, but this is definitely a safe assumption).
    for (int i = 0; i < 1000; ++i) {
        [[maybe_unused]] auto closedBuckets = insertDocument();

        if (0 < _getExecutionStat(_ns1, kNumArchivedDueToMemoryThreshold)) {
            break;
        }
    }

    // When we first hit the limit, we should try to archive some buckets prior to closing anything.
    // However, depending on how the buckets are distributed over the stripes, it's possible that
    // the current stripe will not have enough open buckets to archive to drop below the limit, and
    // may immediately close a bucket it has just archived. We should be able to guarantee that we
    // have archived a bucket prior to closing it though.
    ASSERT_LT(0, _getExecutionStat(_ns1, kNumArchivedDueToMemoryThreshold));
    auto numClosedInFirstRound = _getExecutionStat(_ns1, kNumClosedDueToMemoryThreshold);
    ASSERT_LTE(numClosedInFirstRound, _getExecutionStat(_ns1, kNumArchivedDueToMemoryThreshold));

    // If we continue to open more new buckets with distinct meta values, eventually we'll run out
    // of open buckets to archive and have to start closing archived buckets to relieve memory
    // pressure. Again, an archived bucket should take up more than 10 bytes in the catalog, so we
    // should be fine with a maximum of 1000 iterations.
    for (int i = 0; i < 1000; ++i) {
        auto closedBuckets = insertDocument();

        if (numClosedInFirstRound < _getExecutionStat(_ns1, kNumClosedDueToMemoryThreshold)) {
            ASSERT_FALSE(closedBuckets.empty());
            break;
        }
    }

    // We should have closed some (additional) buckets by now.
    ASSERT_LT(numClosedInFirstRound, _getExecutionStat(_ns1, kNumClosedDueToMemoryThreshold));
}

TEST_F(BucketCatalogTest, TryInsertWillNotCreateBucketWhenWeShouldTryToReopen) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // Try to insert with no open bucket. Should hint to re-open.
    auto result = _bucketCatalog->tryInsert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    ASSERT(result.getValue().closedBuckets.empty());
    ASSERT(!result.getValue().batch);
    ASSERT_EQ(result.getValue().candidate, boost::none);

    // Actually insert so we do have an open bucket to test against.
    result = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    auto batch = result.getValue().batch;
    ASSERT(batch);
    auto bucketId = batch->bucket().id;
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);
    _bucketCatalog->finish(batch, {});

    // Time backwards should hint to re-open.
    result = _bucketCatalog->tryInsert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:40.000Z"}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    ASSERT(result.getValue().closedBuckets.empty());
    ASSERT(!result.getValue().batch);
    ASSERT_EQ(result.getValue().candidate, boost::none);

    // So should time forward.
    result = _bucketCatalog->tryInsert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-07T15:34:40.000Z"}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    ASSERT(result.getValue().closedBuckets.empty());
    ASSERT(!result.getValue().batch);
    ASSERT_EQ(result.getValue().candidate, boost::none);

    // Now let's insert something so we archive the existing bucket.
    result = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-07T15:34:40.000Z"}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    batch = result.getValue().batch;
    ASSERT_NE(batch->bucket().id, bucketId);
    ASSERT(batch);
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);
    _bucketCatalog->finish(batch, {});

    // If we try to insert something that could fit in the archived bucket, we should get it back as
    // a candidate.
    result = _bucketCatalog->tryInsert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:35:40.000Z"}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    ASSERT(result.getValue().closedBuckets.empty());
    ASSERT(!result.getValue().batch);
    ASSERT(result.getValue().candidate.has_value());
    ASSERT_EQ(result.getValue().candidate.value(), bucketId);
}

TEST_F(BucketCatalogTest, TryInsertWillCreateBucketIfWeWouldCloseExistingBucket) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // Insert a document so we have a base bucket
    auto result = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"}, "a": true})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    auto batch = result.getValue().batch;
    ASSERT(batch);
    auto bucketId = batch->bucket().id;
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);
    _bucketCatalog->finish(batch, {});

    // Incompatible schema would close the existing bucket, so we should expect to open a new bucket
    // and proceed to insert the document.
    result = _bucketCatalog->tryInsert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:35:40.000Z"}, "a": {}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    batch = result.getValue().batch;
    ASSERT(batch);
    ASSERT_NE(batch->bucket().id, bucketId);
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);
    _bucketCatalog->finish(batch, {});
}

TEST_F(BucketCatalogTest, InsertIntoReopenedBucket) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // Insert a document so we have a base bucket and we can test that we archive it when we reopen
    // a conflicting bucket.
    auto result = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:40.000Z"}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    auto batch = result.getValue().batch;
    ASSERT(batch);
    auto oldBucketId = batch->bucket().id;
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);
    _bucketCatalog->finish(batch, {});

    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"}},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"}}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"}}}})");
    ASSERT_NE(bucketDoc["_id"].OID(), oldBucketId);
    auto validator = [&](OperationContext * opCtx, const BSONObj& bucketDoc) -> auto {
        return autoColl->checkValidation(opCtx, bucketDoc);
    };

    // We should be able to pass in a valid bucket and insert into it.
    result = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:35:40.000Z"}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow,
        BucketCatalog::BucketToReopen{bucketDoc, validator});
    ASSERT_OK(result.getStatus());
    batch = result.getValue().batch;
    ASSERT(batch);
    ASSERT_EQ(batch->bucket().id, bucketDoc["_id"].OID());
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);
    _bucketCatalog->finish(batch, {});
    ASSERT_EQ(1, _getExecutionStat(_ns1, kNumArchivedDueToReopening));
    ASSERT_EQ(1, _getExecutionStat(_ns1, kNumBucketsReopened));

    // Verify the old bucket was archived and we'll get it back as a candidate.
    result = _bucketCatalog->tryInsert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:35:40.000Z"}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    ASSERT(result.getValue().closedBuckets.empty());
    ASSERT(!result.getValue().batch);
    ASSERT(result.getValue().candidate.has_value());
    ASSERT_EQ(result.getValue().candidate.value(), oldBucketId);
}

TEST_F(BucketCatalogTest, CannotInsertIntoOutdatedBucket) {
    RAIIServerParameterControllerForTest controller{"featureFlagTimeseriesScalabilityImprovements",
                                                    true};
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // Insert a document so we have a base bucket and we can test that we archive it when we reopen
    // a conflicting bucket.
    auto result = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:40.000Z"}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    auto batch = result.getValue().batch;
    ASSERT(batch);
    auto oldBucketId = batch->bucket().id;
    ASSERT(batch->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch));
    ASSERT_EQ(batch->measurements().size(), 1);
    _bucketCatalog->finish(batch, {});

    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"}},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"}}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"}}}})");
    ASSERT_NE(bucketDoc["_id"].OID(), oldBucketId);
    auto validator = [&](OperationContext * opCtx, const BSONObj& bucketDoc) -> auto {
        return autoColl->checkValidation(opCtx, bucketDoc);
    };

    // If we advance the catalog era, then we shouldn't use a bucket that was fetched during a
    // previous era.
    _bucketCatalog->clear(OID());

    // We should get an WriteConflict back if we pass in an outdated bucket.
    result = _bucketCatalog->insert(
        _opCtx,
        _ns1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:35:40.000Z"}})"),
        BucketCatalog::CombineWithInsertsFromOtherClients::kAllow,
        BucketCatalog::BucketToReopen{bucketDoc, validator, result.getValue().catalogEra});
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::WriteConflict);
}

}  // namespace
}  // namespace mongo
