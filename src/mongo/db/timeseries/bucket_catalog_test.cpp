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
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {
class BucketCatalogTest : public CatalogTestFixture {
protected:
    class RunBackgroundTaskAndWaitForFailpoint {
        stdx::thread _taskThread;

    public:
        RunBackgroundTaskAndWaitForFailpoint(const std::string& failpointName,
                                             std::function<void()>&& fn);
        ~RunBackgroundTaskAndWaitForFailpoint();
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

    long long _getNumWaits(const NamespaceString& ns);
    long long _getNumSchemaChanges(const NamespaceString& ns);

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

void BucketCatalogTest::setUp() {
    CatalogTestFixture::setUp();

    _opCtx = operationContext();
    _bucketCatalog = &BucketCatalog::get(_opCtx);

    for (const auto& ns : {_ns1, _ns2, _ns3}) {
        ASSERT_OK(createCollection(
            _opCtx,
            ns.db().toString(),
            BSON("create" << ns.coll() << "timeseries" << _makeTimeseriesOptionsForCreate())));
    }
}

BucketCatalogTest::RunBackgroundTaskAndWaitForFailpoint::RunBackgroundTaskAndWaitForFailpoint(
    const std::string& failpointName, std::function<void()>&& fn) {
    auto fp = globalFailPointRegistry().find(failpointName);
    auto timesEntered = fp->setMode(FailPoint::alwaysOn, 0);

    // Start background job.
    _taskThread = stdx::thread(std::move(fn));

    // Once we hit the failpoint once, turn it off.
    fp->waitForTimesEntered(timesEntered + 1);
    fp->setMode(FailPoint::off, 0);
}

BucketCatalogTest::RunBackgroundTaskAndWaitForFailpoint::~RunBackgroundTaskAndWaitForFailpoint() {
    _taskThread.join();
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

long long BucketCatalogTest::_getNumWaits(const NamespaceString& ns) {
    BSONObjBuilder builder;
    _bucketCatalog->appendExecutionStats(ns, &builder);
    return builder.obj().getIntField("numWaits");
}

long long BucketCatalogTest::_getNumSchemaChanges(const NamespaceString& ns) {
    BSONObjBuilder builder;
    _bucketCatalog->appendExecutionStats(ns, &builder);
    return builder.obj().getIntField("numBucketsClosedDueToSchemaChange");
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

            auto pre = _getNumSchemaChanges(_ns1);
            auto result = _bucketCatalog
                              ->insert(_opCtx,
                                       _ns1,
                                       _getCollator(_ns1),
                                       _getTimeseriesOptions(_ns1),
                                       timestampedDoc.obj(),
                                       BucketCatalog::CombineWithInsertsFromOtherClients::kAllow)
                              .getValue();
            auto post = _getNumSchemaChanges(_ns1);

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

    {
        auto task = RunBackgroundTaskAndWaitForFailpoint{
            "hangWaitingForConflictingPreparedBatch",
            [&]() { ASSERT_OK(_bucketCatalog->prepareCommit(batch2)); }};

        // Finish the first batch.
        _bucketCatalog->finish(batch1, {});
        ASSERT(batch1->finished());
    }

    _bucketCatalog->finish(batch2, {});
    ASSERT(batch2->finished());
}

TEST_F(BucketCatalogTest, AbortingBatchEnsuresBucketIsEventuallyClosed) {
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
                               BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow)
                      .getValue()
                      .batch;
    ASSERT_EQ(batch1->bucket().id, batch2->bucket().id);
    ASSERT_EQ(batch1->bucket().id, batch3->bucket().id);

    ASSERT(batch1->claimCommitRights());
    ASSERT(batch2->claimCommitRights());
    ASSERT(batch3->claimCommitRights());

    // Batch 2 will not be able to commit until batch 1 has finished.
    ASSERT_OK(_bucketCatalog->prepareCommit(batch1));

    {
        auto task = RunBackgroundTaskAndWaitForFailpoint{
            "hangWaitingForConflictingPreparedBatch",
            [&]() { ASSERT_NOT_OK(_bucketCatalog->prepareCommit(batch2)); }};

        // If we abort the third batch, it should abort the second one too, as it isn't prepared.
        // However, since the first batch is prepared, we can't abort it or clean up the bucket. We
        // can then finish the first batch, which will allow the second batch to proceed. It should
        // recognize it has been aborted and clean up the bucket.
        _bucketCatalog->abort(batch3, Status{ErrorCodes::TimeseriesBucketCleared, "cleared"});
        _bucketCatalog->finish(batch1, {});
        ASSERT(batch1->finished());
    }
    // Wait for the batch 2 task to finish preparing commit. Since batch 1 finished, batch 2 should
    // be unblocked. Note that after aborting batch 3, batch 2 was not in a prepared state, so we
    // expect the prepareCommit() call to fail.
    ASSERT(batch2->finished());

    // Make sure a new batch ends up in a new bucket.
    auto batch4 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow)
                      .getValue()
                      .batch;
    ASSERT_NE(batch2->bucket().id, batch4->bucket().id);
}

TEST_F(BucketCatalogTest, AbortingBatchEnsuresNewInsertsGoToNewBucket) {
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

    // Batch 1 and 2 use the same bucket.
    ASSERT_EQ(batch1->bucket().id, batch2->bucket().id);
    ASSERT(batch1->claimCommitRights());
    ASSERT(batch2->claimCommitRights());
    ASSERT_OK(_bucketCatalog->prepareCommit(batch1));

    // Batch 1 will be in a prepared state now. Abort the second batch so that bucket 1 will be
    // closed after batch 1 finishes.
    _bucketCatalog->abort(batch2, Status{ErrorCodes::TimeseriesBucketCleared, "cleared"});
    _bucketCatalog->finish(batch1, {});
    ASSERT(batch1->finished());
    ASSERT(batch2->finished());

    // Ensure a batch started after batch 2 aborts, does not insert future measurements into the
    // aborted batch/bucket.
    auto batch3 = _bucketCatalog
                      ->insert(_opCtx,
                               _ns1,
                               _getCollator(_ns1),
                               _getTimeseriesOptions(_ns1),
                               BSON(_timeField << Date_t::now()),
                               BucketCatalog::CombineWithInsertsFromOtherClients::kDisallow)
                      .getValue()
                      .batch;
    ASSERT_NE(batch1->bucket().id, batch3->bucket().id);
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

}  // namespace
}  // namespace mongo
