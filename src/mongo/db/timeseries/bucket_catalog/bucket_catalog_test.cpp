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

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <initializer_list>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

namespace mongo::timeseries::bucket_catalog {
namespace {
constexpr StringData kNumSchemaChanges = "numBucketsClosedDueToSchemaChange"_sd;
constexpr StringData kNumBucketsReopened = "numBucketsReopened"_sd;
constexpr StringData kNumArchivedDueToMemoryThreshold = "numBucketsArchivedDueToMemoryThreshold"_sd;
constexpr StringData kNumClosedDueToReopening = "numBucketsClosedDueToReopening"_sd;
constexpr StringData kNumClosedDueToTimeForward = "numBucketsClosedDueToTimeForward"_sd;
constexpr StringData kNumClosedDueToTimeBackward = "numBucketsClosedDueToTimeBackward"_sd;
constexpr StringData kNumClosedDueToMemoryThreshold = "numBucketsClosedDueToMemoryThreshold"_sd;

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
    virtual void preSetUp();
    virtual std::vector<NamespaceString> getNamespaceStrings();

    std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
    _makeOperationContext();

    virtual BSONObj _makeTimeseriesOptionsForCreate() const;

    TimeseriesOptions _getTimeseriesOptions(const NamespaceString& ns) const;
    const CollatorInterface* _getCollator(const NamespaceString& ns) const;

    void _commit(const NamespaceString& ns,
                 const std::shared_ptr<WriteBatch>& batch,
                 uint16_t numPreviouslyCommittedMeasurements,
                 size_t expectedBatchSize = 1);
    void _insertOneAndCommit(const NamespaceString& ns,
                             const UUID& uuid,
                             uint16_t numPreviouslyCommittedMeasurements);

    long long _getExecutionStat(const UUID& uuid, StringData stat);

    // Check that each group of objects has compatible schema with itself, but that inserting the
    // first object in new group closes the existing bucket and opens a new one
    void _testMeasurementSchema(
        const std::initializer_list<std::initializer_list<BSONObj>>& groups);

    Status _reopenBucket(const CollectionPtr& coll, const BSONObj& bucketDoc);

    OperationContext* _opCtx;
    BucketCatalog* _bucketCatalog;

    StringData _timeField = "time";
    StringData _metaField = "tag";

    NamespaceString _ns1 =
        NamespaceString::createNamespaceString_forTest("bucket_catalog_test_1", "t_1");
    NamespaceString _ns2 =
        NamespaceString::createNamespaceString_forTest("bucket_catalog_test_1", "t_2");
    NamespaceString _ns3 =
        NamespaceString::createNamespaceString_forTest("bucket_catalog_test_2", "t_1");

    UUID _uuid1 = UUID::gen();
    UUID _uuid2 = UUID::gen();
    UUID _uuid3 = UUID::gen();
};

class BucketCatalogWithoutMetadataTest : public BucketCatalogTest {
protected:
    BSONObj _makeTimeseriesOptionsForCreate() const override;
};

void BucketCatalogTest::preSetUp() {}

std::vector<NamespaceString> BucketCatalogTest::getNamespaceStrings() {
    return {_ns1, _ns2, _ns3};
}

void BucketCatalogTest::setUp() {
    preSetUp();
    CatalogTestFixture::setUp();

    _opCtx = operationContext();
    _bucketCatalog = &BucketCatalog::get(_opCtx);

    for (auto&& [ns, uuid] : std::initializer_list<std::pair<NamespaceString*, UUID*>>{
             {&_ns1, &_uuid1}, {&_ns2, &_uuid2}, {&_ns3, &_uuid3}}) {
        ASSERT_OK(createCollection(
            _opCtx,
            ns->dbName(),
            BSON("create" << ns->coll() << "timeseries" << _makeTimeseriesOptionsForCreate())));
        AutoGetCollection autoColl(_opCtx, ns->makeTimeseriesBucketsNamespace(), MODE_IS);
        *uuid = autoColl.getCollection()->uuid();
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
    auto client = getServiceContext()->getService()->makeClient("BucketCatalogTest");
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

void BucketCatalogTest::_commit(const NamespaceString& ns,
                                const std::shared_ptr<WriteBatch>& batch,
                                uint16_t numPreviouslyCommittedMeasurements,
                                size_t expectedBatchSize) {
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, ns, batch));
    ASSERT_EQ(batch->measurements.size(), expectedBatchSize);
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, numPreviouslyCommittedMeasurements);

    finish(_opCtx, *_bucketCatalog, ns, batch, {});
}

void BucketCatalogTest::_insertOneAndCommit(const NamespaceString& ns,
                                            const UUID& uuid,
                                            uint16_t numPreviouslyCommittedMeasurements) {
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         ns,
                         uuid,
                         _getCollator(ns),
                         _getTimeseriesOptions(ns),
                         BSON(_timeField << Date_t::now()),
                         CombineWithInsertsFromOtherClients::kAllow);
    auto& batch = get<SuccessfulInsertion>(result.getValue()).batch;
    _commit(ns, batch, numPreviouslyCommittedMeasurements);
}

long long BucketCatalogTest::_getExecutionStat(const UUID& uuid, StringData stat) {
    BSONObjBuilder builder;
    appendExecutionStats(*_bucketCatalog, uuid, builder);
    return builder.obj().getIntField(stat);
}

void BucketCatalogTest::_testMeasurementSchema(
    const std::initializer_list<std::initializer_list<BSONObj>>& groups) {
    // Make sure we start and end with a clean slate.
    clear(*_bucketCatalog, _uuid1);
    ScopeGuard guard([this]() { clear(*_bucketCatalog, _uuid1); });

    bool firstGroup = true;
    for (const auto& group : groups) {
        bool firstMember = true;
        for (const auto& doc : group) {
            BSONObjBuilder timestampedDoc;
            timestampedDoc.append(_timeField, Date_t::now());
            timestampedDoc.appendElements(doc);

            auto pre = _getExecutionStat(_uuid1, kNumSchemaChanges);
            ASSERT(insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          timestampedDoc.obj(),
                          CombineWithInsertsFromOtherClients::kAllow)
                       .isOK());
            auto post = _getExecutionStat(_uuid1, kNumSchemaChanges);

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

Status BucketCatalogTest::_reopenBucket(const CollectionPtr& coll, const BSONObj& bucketDoc) {
    const NamespaceString ns = coll->ns().getTimeseriesViewNamespace();
    const UUID uuid = coll->uuid();
    const boost::optional<TimeseriesOptions> options = coll->getTimeseriesOptions();
    invariant(options,
              str::stream() << "Attempting to reopen a bucket for a non-timeseries collection: "
                            << ns.toStringForErrorMsg());

    BSONElement metadata;
    auto metaFieldName = options->getMetaField();
    if (metaFieldName) {
        metadata = bucketDoc.getField(kBucketMetaFieldName);
    }
    TrackingContext trackingContext;
    auto key = BucketKey{
        uuid, BucketMetadata{trackingContext, metadata, coll->getDefaultCollator(), metaFieldName}};

    // Validate the bucket document against the schema.
    auto validator = [&](OperationContext * opCtx, const BSONObj& bucketDoc) -> auto {
        return coll->checkValidation(opCtx, bucketDoc);
    };

    auto stats = internal::getOrInitializeExecutionStats(*_bucketCatalog, uuid);

    auto res = internal::rehydrateBucket(_opCtx,
                                         *_bucketCatalog,
                                         stats,
                                         uuid,
                                         coll->getDefaultCollator(),
                                         *options,
                                         BucketToReopen{bucketDoc, validator},
                                         getCurrentEra(_bucketCatalog->bucketStateRegistry),
                                         nullptr);
    if (!res.isOK()) {
        return res.getStatus();
    }
    auto bucket = std::move(res.getValue());

    auto stripeNumber = internal::getStripeNumber(key, _bucketCatalog->numberOfStripes);

    // Register the reopened bucket with the catalog.
    auto& stripe = *_bucketCatalog->stripes[stripeNumber];
    stdx::lock_guard stripeLock{stripe.mutex};

    ClosedBuckets closedBuckets;
    return internal::reopenBucket(_opCtx,
                                  *_bucketCatalog,
                                  stripe,
                                  stripeLock,
                                  stats,
                                  key,
                                  std::move(bucket),
                                  getCurrentEra(_bucketCatalog->bucketStateRegistry),
                                  closedBuckets)
        .getStatus();
}


TEST_F(BucketCatalogTest, InsertIntoSameBucket) {
    // The first insert should be able to take commit rights
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch1 = get<SuccessfulInsertion>(result1.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch1));

    // A subsequent insert into the same bucket should land in the same batch, but not be able to
    // claim commit rights
    auto result2 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;
    ASSERT_EQ(batch1, batch2);
    ASSERT(!claimWriteBatchCommitRights(*batch2));

    // The batch hasn't actually been committed yet.
    ASSERT(!isWriteBatchFinished(*batch1));

    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch1));

    // Still not finished.
    ASSERT(!isWriteBatchFinished(*batch1));

    // The batch should contain both documents since they belong in the same bucket and happened
    // in the same commit epoch. Nothing else has been committed in this bucket yet.
    ASSERT_EQ(batch1->measurements.size(), 2);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0);

    // Once the commit has occurred, the waiter should be notified.
    finish(_opCtx, *_bucketCatalog, _ns1, batch1, {});
    ASSERT(isWriteBatchFinished(*batch2));
    auto result3 = getWriteBatchResult(*batch2);
    ASSERT_OK(result3.getStatus());
}

TEST_F(BucketCatalogTest, GetMetadataReturnsEmptyDocOnMissingBucket) {
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         BSON(_timeField << Date_t::now()),
                         CombineWithInsertsFromOtherClients::kAllow);
    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch));
    auto bucket = batch->bucketHandle;
    abort(*_bucketCatalog, batch, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT_BSONOBJ_EQ(BSONObj(), getMetadata(*_bucketCatalog, bucket));
}

TEST_F(BucketCatalogTest, InsertIntoDifferentBuckets) {
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now() << _metaField << "123"),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto result2 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now() << _metaField << BSONObj()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto result3 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns2,
                          _uuid2,
                          _getCollator(_ns2),
                          _getTimeseriesOptions(_ns2),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);

    // Inserts should all be into three distinct buckets (and therefore batches).
    ASSERT_NE(get<SuccessfulInsertion>(result1.getValue()).batch,
              get<SuccessfulInsertion>(result2.getValue()).batch);
    ASSERT_NE(get<SuccessfulInsertion>(result1.getValue()).batch,
              get<SuccessfulInsertion>(result3.getValue()).batch);
    ASSERT_NE(get<SuccessfulInsertion>(result2.getValue()).batch,
              get<SuccessfulInsertion>(result3.getValue()).batch);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << "123"),
        getMetadata(*_bucketCatalog,
                    get<SuccessfulInsertion>(result1.getValue()).batch->bucketHandle));
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << BSONObj()),
        getMetadata(*_bucketCatalog,
                    get<SuccessfulInsertion>(result2.getValue()).batch->bucketHandle));
    ASSERT(getMetadata(*_bucketCatalog,
                       get<SuccessfulInsertion>(result3.getValue()).batch->bucketHandle)
               .isEmpty());

    // Committing one bucket should only return the one document in that bucket and should not
    // affect the other bucket.
    _commit(_ns1, get<SuccessfulInsertion>(result1.getValue()).batch, 0);
    _commit(_ns1, get<SuccessfulInsertion>(result2.getValue()).batch, 0);
    _commit(_ns2, get<SuccessfulInsertion>(result3.getValue()).batch, 0);
}

TEST_F(BucketCatalogTest, InsertThroughDifferentCatalogsIntoDifferentBuckets) {
    BucketCatalog temporaryBucketCatalog(/*numberOfStripes=*/1,
                                         getTimeseriesIdleBucketExpiryMemoryUsageThresholdBytes);
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch1 = get<SuccessfulInsertion>(result1.getValue()).batch;
    auto result2 = insert(_opCtx,
                          temporaryBucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;

    // Inserts should be into different buckets (and therefore batches) because they went through
    // different bucket catalogs.
    ASSERT_NE(batch1, batch2);

    // Committing one bucket should only return the one document in that bucket and should not
    // affect the other bucket.
    ASSERT(claimWriteBatchCommitRights(*batch1));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch1));
    ASSERT_EQ(batch1->measurements.size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0);
    finish(_opCtx, *_bucketCatalog, _ns1, batch1, {});

    ASSERT(claimWriteBatchCommitRights(*batch2));
    ASSERT_OK(prepareCommit(temporaryBucketCatalog, _ns1, batch2));
    ASSERT_EQ(batch2->measurements.size(), 1);
    ASSERT_EQ(batch2->numPreviouslyCommittedMeasurements, 0);
    finish(_opCtx, temporaryBucketCatalog, _ns1, batch2, {});
}

TEST_F(BucketCatalogTest, InsertIntoSameBucketArray) {
    auto result1 = insert(
        _opCtx,
        *_bucketCatalog,
        _ns1,
        _uuid1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField << BSON_ARRAY(BSON("a" << 0 << "b" << 1))),
        CombineWithInsertsFromOtherClients::kAllow);
    auto result2 = insert(
        _opCtx,
        *_bucketCatalog,
        _ns1,
        _uuid1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        BSON(_timeField << Date_t::now() << _metaField << BSON_ARRAY(BSON("b" << 1 << "a" << 0))),
        CombineWithInsertsFromOtherClients::kAllow);

    ASSERT_EQ(get<SuccessfulInsertion>(result1.getValue()).batch,
              get<SuccessfulInsertion>(result2.getValue()).batch);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << BSON_ARRAY(BSON("a" << 0 << "b" << 1))),
        getMetadata(*_bucketCatalog,
                    get<SuccessfulInsertion>(result1.getValue()).batch->bucketHandle));
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << BSON_ARRAY(BSON("a" << 0 << "b" << 1))),
        getMetadata(*_bucketCatalog,
                    get<SuccessfulInsertion>(result2.getValue()).batch->bucketHandle));
}

TEST_F(BucketCatalogTest, InsertIntoSameBucketObjArray) {
    auto result1 =
        insert(_opCtx,
               *_bucketCatalog,
               _ns1,
               _uuid1,
               _getCollator(_ns1),
               _getTimeseriesOptions(_ns1),
               BSON(_timeField << Date_t::now() << _metaField
                               << BSONObj(BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1)
                                                                 << BSON("f" << 1 << "g" << 0))))),
               CombineWithInsertsFromOtherClients::kAllow);
    auto result2 =
        insert(_opCtx,
               *_bucketCatalog,
               _ns1,
               _uuid1,
               _getCollator(_ns1),
               _getTimeseriesOptions(_ns1),
               BSON(_timeField << Date_t::now() << _metaField
                               << BSONObj(BSON("c" << BSON_ARRAY(BSON("b" << 1 << "a" << 0)
                                                                 << BSON("g" << 0 << "f" << 1))))),
               CombineWithInsertsFromOtherClients::kAllow);

    ASSERT_EQ(get<SuccessfulInsertion>(result1.getValue()).batch,
              get<SuccessfulInsertion>(result2.getValue()).batch);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << BSONObj(BSON(
                 "c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1) << BSON("f" << 1 << "g" << 0))))),
        getMetadata(*_bucketCatalog,
                    get<SuccessfulInsertion>(result1.getValue()).batch->bucketHandle));
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << BSONObj(BSON(
                 "c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1) << BSON("f" << 1 << "g" << 0))))),
        getMetadata(*_bucketCatalog,
                    get<SuccessfulInsertion>(result2.getValue()).batch->bucketHandle));
}


TEST_F(BucketCatalogTest, InsertIntoSameBucketNestedArray) {
    auto result1 =
        insert(_opCtx,
               *_bucketCatalog,
               _ns1,
               _uuid1,
               _getCollator(_ns1),
               _getTimeseriesOptions(_ns1),
               BSON(_timeField << Date_t::now() << _metaField
                               << BSONObj(BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1)
                                                                 << BSON_ARRAY("123"
                                                                               << "456"))))),
               CombineWithInsertsFromOtherClients::kAllow);
    auto result2 =
        insert(_opCtx,
               *_bucketCatalog,
               _ns1,
               _uuid1,
               _getCollator(_ns1),
               _getTimeseriesOptions(_ns1),
               BSON(_timeField << Date_t::now() << _metaField
                               << BSONObj(BSON("c" << BSON_ARRAY(BSON("b" << 1 << "a" << 0)
                                                                 << BSON_ARRAY("123"
                                                                               << "456"))))),
               CombineWithInsertsFromOtherClients::kAllow);

    ASSERT_EQ(get<SuccessfulInsertion>(result1.getValue()).batch,
              get<SuccessfulInsertion>(result2.getValue()).batch);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << BSONObj(
                 BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1) << BSON_ARRAY("123"
                                                                                 << "456"))))),
        getMetadata(*_bucketCatalog,
                    get<SuccessfulInsertion>(result1.getValue()).batch->bucketHandle));
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << BSONObj(
                 BSON("c" << BSON_ARRAY(BSON("a" << 0 << "b" << 1) << BSON_ARRAY("123"
                                                                                 << "456"))))),
        getMetadata(*_bucketCatalog,
                    get<SuccessfulInsertion>(result2.getValue()).batch->bucketHandle));
}

TEST_F(BucketCatalogTest, InsertNullAndMissingMetaFieldIntoDifferentBuckets) {
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now() << _metaField << BSONNULL),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto result2 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);

    // Inserts should all be into three distinct buckets (and therefore batches).
    ASSERT_NE(get<SuccessfulInsertion>(result1.getValue()).batch,
              get<SuccessfulInsertion>(result2.getValue()).batch);

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(
        BSON(_metaField << BSONNULL),
        getMetadata(*_bucketCatalog,
                    get<SuccessfulInsertion>(result1.getValue()).batch->bucketHandle));
    ASSERT(getMetadata(*_bucketCatalog,
                       get<SuccessfulInsertion>(result2.getValue()).batch->bucketHandle)
               .isEmpty());

    // Committing one bucket should only return the one document in that bucket and should not
    // affect the other bucket.
    for (const auto& batch : {get<SuccessfulInsertion>(result1.getValue()).batch,
                              get<SuccessfulInsertion>(result2.getValue()).batch}) {
        _commit(_ns1, batch, 0);
    }
}

TEST_F(BucketCatalogTest, NumCommittedMeasurementsAccumulates) {
    // The numCommittedMeasurements returned when committing should accumulate as more entries in
    // the bucket are committed.
    _insertOneAndCommit(_ns1, _uuid1, 0);
    _insertOneAndCommit(_ns1, _uuid1, 1);
}

TEST_F(BucketCatalogTest, ClearNamespaceBuckets) {
    _insertOneAndCommit(_ns1, _uuid1, 0);
    _insertOneAndCommit(_ns2, _uuid2, 0);

    clear(*_bucketCatalog, _uuid1);

    _insertOneAndCommit(_ns1, _uuid1, 0);
    _insertOneAndCommit(_ns2, _uuid2, 1);
}

TEST_F(BucketCatalogTest, InsertBetweenPrepareAndFinish) {
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch1 = get<SuccessfulInsertion>(result1.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch1));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch1));
    ASSERT_EQ(batch1->measurements.size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0);

    // Insert before finish so there's a second batch live at the same time.
    auto result2 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;
    ASSERT_NE(batch1, batch2);

    finish(_opCtx, *_bucketCatalog, _ns1, batch1, {});
    ASSERT(isWriteBatchFinished(*batch1));

    // Verify the second batch still commits one doc, and that the first batch only commited one.
    _commit(_ns1, batch2, 1);
}

DEATH_TEST_F(BucketCatalogTest, CannotCommitWithoutRights, "invariant") {
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         BSON(_timeField << Date_t::now()),
                         CombineWithInsertsFromOtherClients::kAllow);
    auto& batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));

    // BucketCatalog::prepareCommit uses dassert, so it will only invariant in debug mode. Ensure we
    // die here in non-debug mode as well.
    invariant(kDebugBuild);
}

TEST_F(BucketCatalogWithoutMetadataTest, GetMetadataReturnsEmptyDoc) {
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         BSON(_timeField << Date_t::now()),
                         CombineWithInsertsFromOtherClients::kAllow);
    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;

    ASSERT_BSONOBJ_EQ(BSONObj(), getMetadata(*_bucketCatalog, batch->bucketHandle));

    _commit(_ns1, batch, 0);
}

TEST_F(BucketCatalogWithoutMetadataTest, CommitReturnsNewFields) {
    // Creating a new bucket should return all fields from the initial measurement.
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         BSON(_timeField << Date_t::now() << "a" << 0),
                         CombineWithInsertsFromOtherClients::kAllow);
    ASSERT(result.isOK());
    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    auto oldId = batch->bucketHandle.bucketId;
    _commit(_ns1, batch, 0);
    ASSERT_EQ(2U, batch->newFieldNamesToBeInserted.size()) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted.count(_timeField)) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted.count("a")) << batch->toBSON();

    // Inserting a new measurement with the same fields should return an empty set of new fields.

    result = insert(_opCtx,
                    *_bucketCatalog,
                    _ns1,
                    _uuid1,
                    _getCollator(_ns1),
                    _getTimeseriesOptions(_ns1),
                    BSON(_timeField << Date_t::now() << "a" << 1),
                    CombineWithInsertsFromOtherClients::kAllow);
    ASSERT(result.isOK());
    batch = get<SuccessfulInsertion>(result.getValue()).batch;
    _commit(_ns1, batch, 1);
    ASSERT_EQ(0U, batch->newFieldNamesToBeInserted.size()) << batch->toBSON();

    // Insert a new measurement with the a new field.
    result = insert(_opCtx,
                    *_bucketCatalog,
                    _ns1,
                    _uuid1,
                    _getCollator(_ns1),
                    _getTimeseriesOptions(_ns1),
                    BSON(_timeField << Date_t::now() << "a" << 2 << "b" << 2),
                    CombineWithInsertsFromOtherClients::kAllow);
    ASSERT(result.isOK());
    batch = get<SuccessfulInsertion>(result.getValue()).batch;
    _commit(_ns1, batch, 2);
    ASSERT_EQ(1U, batch->newFieldNamesToBeInserted.size()) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted.count("b")) << batch->toBSON();

    // Fill up the bucket.
    for (auto i = 3; i < gTimeseriesBucketMaxCount; ++i) {
        result = insert(_opCtx,
                        *_bucketCatalog,
                        _ns1,
                        _uuid1,
                        _getCollator(_ns1),
                        _getTimeseriesOptions(_ns1),
                        BSON(_timeField << Date_t::now() << "a" << i),
                        CombineWithInsertsFromOtherClients::kAllow);
        ASSERT(result.isOK());
        batch = get<SuccessfulInsertion>(result.getValue()).batch;
        _commit(_ns1, batch, i);
        ASSERT_EQ(0U, batch->newFieldNamesToBeInserted.size()) << i << ":" << batch->toBSON();
    }

    // When a bucket overflows, committing to the new overflow bucket should return the fields of
    // the first measurement as new fields.
    auto result2 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now() << "a" << gTimeseriesBucketMaxCount),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto& batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;
    ASSERT_NE(oldId, batch2->bucketHandle.bucketId);
    _commit(_ns1, batch2, 0);
    ASSERT_EQ(2U, batch2->newFieldNamesToBeInserted.size()) << batch2->toBSON();
    ASSERT(batch2->newFieldNamesToBeInserted.count(_timeField)) << batch2->toBSON();
    ASSERT(batch2->newFieldNamesToBeInserted.count("a")) << batch2->toBSON();
}

TEST_F(BucketCatalogTest, AbortBatchOnBucketWithPreparedCommit) {
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch1 = get<SuccessfulInsertion>(result1.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch1));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch1));
    ASSERT_EQ(batch1->measurements.size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0);

    // Insert before finish so there's a second batch live at the same time.
    auto result2 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;
    ASSERT_NE(batch1, batch2);

    ASSERT(claimWriteBatchCommitRights(*batch2));
    abort(*_bucketCatalog, batch2, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(isWriteBatchFinished(*batch2));
    ASSERT_EQ(getWriteBatchResult(*batch2).getStatus(), ErrorCodes::TimeseriesBucketCleared);

    finish(_opCtx, *_bucketCatalog, _ns1, batch1, {});
    ASSERT(isWriteBatchFinished(*batch1));
    ASSERT_OK(getWriteBatchResult(*batch1).getStatus());
}

TEST_F(BucketCatalogTest, ClearNamespaceWithConcurrentWrites) {
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         BSON(_timeField << Date_t::now()),
                         CombineWithInsertsFromOtherClients::kAllow);
    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch));

    clear(*_bucketCatalog, _uuid1);

    ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchResult(*batch).getStatus(), ErrorCodes::TimeseriesBucketCleared);

    result = insert(_opCtx,
                    *_bucketCatalog,
                    _ns1,
                    _uuid1,
                    _getCollator(_ns1),
                    _getTimeseriesOptions(_ns1),
                    BSON(_timeField << Date_t::now()),
                    CombineWithInsertsFromOtherClients::kAllow);
    batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 0);

    clear(*_bucketCatalog, _uuid1);

    // Even though bucket has been cleared, finish should still report success. Basically, in this
    // case we know that the write succeeded, so it must have happened before the namespace drop
    // operation got the collection lock. So the write did actually happen, but is has since been
    // removed, and that's fine for our purposes. The finish just records the result to the batch
    // and updates some statistics.
    finish(_opCtx, *_bucketCatalog, _ns1, batch, {});
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_OK(getWriteBatchResult(*batch).getStatus());
}


TEST_F(BucketCatalogTest, ClearBucketWithPreparedBatchThrowsConflict) {
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         BSON(_timeField << Date_t::now()),
                         CombineWithInsertsFromOtherClients::kAllow);
    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 0);

    ASSERT_THROWS(directWriteStart(_bucketCatalog->bucketStateRegistry,
                                   _uuid1,
                                   batch->bucketHandle.bucketId.oid),
                  WriteConflictException);

    abort(*_bucketCatalog, batch, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchResult(*batch).getStatus(), ErrorCodes::TimeseriesBucketCleared);
}

TEST_F(BucketCatalogTest, PrepareCommitOnClearedBatchWithAlreadyPreparedBatch) {
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch1 = get<SuccessfulInsertion>(result1.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch1));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch1));
    ASSERT_EQ(batch1->measurements.size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements, 0);

    // Insert before clear so there's a second batch live at the same time.
    auto result2 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;
    ASSERT_NE(batch1, batch2);
    ASSERT_EQ(batch1->bucketHandle.bucketId, batch2->bucketHandle.bucketId);

    // Now clear the bucket. Since there's a prepared batch it should conflict.
    clearBucketState(_bucketCatalog->bucketStateRegistry, batch1->bucketHandle.bucketId);

    // Now try to prepare the second batch. Ensure it aborts the batch.
    ASSERT(claimWriteBatchCommitRights(*batch2));
    ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, _ns1, batch2));
    ASSERT(isWriteBatchFinished(*batch2));
    ASSERT_EQ(getWriteBatchResult(*batch2).getStatus(), ErrorCodes::TimeseriesBucketCleared);

    // Make sure we didn't clear the bucket state when we aborted the second batch.
    clear(*_bucketCatalog, _uuid1);

    // Make sure a subsequent insert, which opens a new bucket, doesn't corrupt the old bucket
    // state and prevent us from finishing the first batch.
    auto result3 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch3 = get<SuccessfulInsertion>(result3.getValue()).batch;
    ASSERT_NE(batch1, batch3);
    ASSERT_NE(batch2, batch3);
    ASSERT_NE(batch1->bucketHandle.bucketId, batch3->bucketHandle.bucketId);
    // Clean up this batch
    ASSERT(claimWriteBatchCommitRights(*batch3));
    abort(*_bucketCatalog, batch3, {ErrorCodes::TimeseriesBucketCleared, ""});

    // Make sure we can finish the cleanly prepared batch.
    finish(_opCtx, *_bucketCatalog, _ns1, batch1, {});
    ASSERT(isWriteBatchFinished(*batch1));
    ASSERT_OK(getWriteBatchResult(*batch1).getStatus());
}

TEST_F(BucketCatalogTest, PrepareCommitOnAlreadyAbortedBatch) {
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         BSON(_timeField << Date_t::now()),
                         CombineWithInsertsFromOtherClients::kAllow);
    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch));

    abort(*_bucketCatalog, batch, {ErrorCodes::TimeseriesBucketCleared, ""});
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchResult(*batch).getStatus(), ErrorCodes::TimeseriesBucketCleared);

    ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT(isWriteBatchFinished(*batch));
    ASSERT_EQ(getWriteBatchResult(*batch).getStatus(), ErrorCodes::TimeseriesBucketCleared);
}

TEST_F(BucketCatalogTest, CombiningWithInsertsFromOtherClients) {
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch1 = get<SuccessfulInsertion>(result1.getValue()).batch;

    auto result2 = insert(_makeOperationContext().second.get(),
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;

    auto result3 = insert(_makeOperationContext().second.get(),
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch3 = get<SuccessfulInsertion>(result3.getValue()).batch;

    auto result4 = insert(_makeOperationContext().second.get(),
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kAllow);
    auto batch4 = get<SuccessfulInsertion>(result4.getValue()).batch;

    ASSERT_NE(batch1, batch2);
    ASSERT_NE(batch1, batch3);
    ASSERT_NE(batch2, batch3);
    ASSERT_EQ(batch3, batch4);

    _commit(_ns1, batch1, 0);
    _commit(_ns1, batch2, 1);
    _commit(_ns1, batch3, 2, 2);
}

TEST_F(BucketCatalogTest, CannotConcurrentlyCommitBatchesForSameBucket) {
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch1 = get<SuccessfulInsertion>(result1.getValue()).batch;

    auto result2 = insert(_makeOperationContext().second.get(),
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;

    ASSERT(claimWriteBatchCommitRights(*batch1));
    ASSERT(claimWriteBatchCommitRights(*batch2));

    // Batch 2 will not be able to commit until batch 1 has finished.
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch1));

    {
        auto task = RunBackgroundTaskAndWaitForFailpoint{
            "hangTimeSeriesBatchPrepareWaitingForConflictingOperation", [&]() {
                ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch2));
            }};

        // Finish the first batch.
        finish(_opCtx, *_bucketCatalog, _ns1, batch1, {});
        ASSERT(isWriteBatchFinished(*batch1));
    }

    finish(_opCtx, *_bucketCatalog, _ns1, batch2, {});
    ASSERT(isWriteBatchFinished(*batch2));
}

TEST_F(BucketCatalogTest, AbortingBatchEnsuresBucketIsEventuallyClosed) {
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch1 = get<SuccessfulInsertion>(result1.getValue()).batch;

    auto result2 = insert(_makeOperationContext().second.get(),
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;

    auto result3 = insert(_makeOperationContext().second.get(),
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch3 = get<SuccessfulInsertion>(result3.getValue()).batch;

    ASSERT_EQ(batch1->bucketHandle.bucketId, batch2->bucketHandle.bucketId);
    ASSERT_EQ(batch1->bucketHandle.bucketId, batch3->bucketHandle.bucketId);

    ASSERT(claimWriteBatchCommitRights(*batch1));
    ASSERT(claimWriteBatchCommitRights(*batch2));
    ASSERT(claimWriteBatchCommitRights(*batch3));

    // Batch 2 will not be able to commit until batch 1 has finished.
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch1));

    {
        auto task = RunBackgroundTaskAndWaitForFailpoint{
            "hangTimeSeriesBatchPrepareWaitingForConflictingOperation", [&]() {
                ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, _ns1, batch2));
            }};

        // If we abort the third batch, it should abort the second one too, as it isn't prepared.
        // However, since the first batch is prepared, we can't abort it or clean up the bucket. We
        // can then finish the first batch, which will allow the second batch to proceed. It should
        // recognize it has been aborted and clean up the bucket.
        abort(*_bucketCatalog, batch3, Status{ErrorCodes::TimeseriesBucketCleared, "cleared"});
        finish(_opCtx, *_bucketCatalog, _ns1, batch1, {});
        ASSERT(isWriteBatchFinished(*batch1));
    }
    // Wait for the batch 2 task to finish preparing commit. Since batch 1 finished, batch 2 should
    // be unblocked. Note that after aborting batch 3, batch 2 was not in a prepared state, so we
    // expect the prepareCommit() call to fail.
    ASSERT_NOT_OK(prepareCommit(*_bucketCatalog, _ns1, batch2));
    ASSERT(isWriteBatchFinished(*batch2));

    // Make sure a new batch ends up in a new bucket.
    auto result4 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch4 = get<SuccessfulInsertion>(result4.getValue()).batch;
    ASSERT_NE(batch2->bucketHandle.bucketId, batch4->bucketHandle.bucketId);
}

TEST_F(BucketCatalogTest, AbortingBatchEnsuresNewInsertsGoToNewBucket) {
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch1 = get<SuccessfulInsertion>(result1.getValue()).batch;

    auto result2 = insert(_makeOperationContext().second.get(),
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;

    // Batch 1 and 2 use the same bucket.
    ASSERT_EQ(batch1->bucketHandle.bucketId, batch2->bucketHandle.bucketId);
    ASSERT(claimWriteBatchCommitRights(*batch1));
    ASSERT(claimWriteBatchCommitRights(*batch2));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch1));

    // Batch 1 will be in a prepared state now. Abort the second batch so that bucket 1 will be
    // closed after batch 1 finishes.
    abort(*_bucketCatalog, batch2, Status{ErrorCodes::TimeseriesBucketCleared, "cleared"});
    finish(_opCtx, *_bucketCatalog, _ns1, batch1, {});
    ASSERT(isWriteBatchFinished(*batch1));
    ASSERT(isWriteBatchFinished(*batch2));

    // Ensure a batch started after batch 2 aborts, does not insert future measurements into the
    // aborted batch/bucket.
    auto result3 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch3 = get<SuccessfulInsertion>(result3.getValue()).batch;
    ASSERT_NE(batch1->bucketHandle.bucketId, batch3->bucketHandle.bucketId);
}

TEST_F(BucketCatalogTest, DuplicateNewFieldNamesAcrossConcurrentBatches) {
    auto result1 = insert(_opCtx,
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch1 = get<SuccessfulInsertion>(result1.getValue()).batch;

    auto result2 = insert(_makeOperationContext().second.get(),
                          *_bucketCatalog,
                          _ns1,
                          _uuid1,
                          _getCollator(_ns1),
                          _getTimeseriesOptions(_ns1),
                          BSON(_timeField << Date_t::now()),
                          CombineWithInsertsFromOtherClients::kDisallow);
    auto batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;

    // Batch 2 is the first batch to commit the time field.
    ASSERT(claimWriteBatchCommitRights(*batch2));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch2));
    ASSERT_EQ(batch2->newFieldNamesToBeInserted.size(), 1);
    ASSERT_EQ(batch2->newFieldNamesToBeInserted.begin()->first, _timeField);
    finish(_opCtx, *_bucketCatalog, _ns1, batch2, {});

    // Batch 1 was the first batch to insert the time field, but by commit time it was already
    // committed by batch 2.
    ASSERT(claimWriteBatchCommitRights(*batch1));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch1));
    ASSERT(batch1->newFieldNamesToBeInserted.empty());
    finish(_opCtx, *_bucketCatalog, _ns1, batch1, {});
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
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), missingIdObj));

        // Bad _id type.
        BSONObj badIdObj = bucketDoc.addFields(BSON("_id" << 123));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badIdObj));
    }

    {
        // Missing control field.
        BSONObj missingControlObj = bucketDoc.removeField("control");
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), missingControlObj));

        // Bad control type.
        BSONObj badControlObj = bucketDoc.addFields(BSON("control" << BSONArray()));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badControlObj));

        // Bad control.version type.
        BSONObj badVersionObj = bucketDoc.addFields(BSON(
            "control" << BSON("version" << BSONArray() << "min"
                                        << BSON("time" << BSON("$date"
                                                               << "2022-06-06T15:34:00.000Z"))
                                        << "max"
                                        << BSON("time" << BSON("$date"
                                                               << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badVersionObj));

        // Bad control.min type.
        BSONObj badMinObj = bucketDoc.addFields(BSON(
            "control" << BSON("version" << 1 << "min" << 123 << "max"
                                        << BSON("time" << BSON("$date"
                                                               << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badMinObj));

        // Bad control.max type.
        BSONObj badMaxObj = bucketDoc.addFields(
            BSON("control" << BSON("version" << 1 << "min"
                                             << BSON("time" << BSON("$date"
                                                                    << "2022-06-06T15:34:00.000Z"))
                                             << "max" << 123)));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badMaxObj));

        // Missing control.min.time.
        BSONObj missingMinTimeObj = bucketDoc.addFields(BSON(
            "control" << BSON("version" << 1 << "min" << BSON("abc" << 1) << "max"
                                        << BSON("time" << BSON("$date"
                                                               << "2022-06-06T15:34:30.000Z")))));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), missingMinTimeObj));

        // Missing control.max.time.
        BSONObj missingMaxTimeObj = bucketDoc.addFields(
            BSON("control" << BSON("version" << 1 << "min"
                                             << BSON("time" << BSON("$date"
                                                                    << "2022-06-06T15:34:00.000Z"))
                                             << "max" << BSON("abc" << 1))));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), missingMaxTimeObj));
    }


    {
        // Missing data field.
        BSONObj missingDataObj = bucketDoc.removeField("data");
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), missingDataObj));

        // Bad data type.
        BSONObj badDataObj = bucketDoc.addFields(BSON("data" << 123));
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), badDataObj));
    }
}

TEST_F(BucketCatalogTest, ReopenMixedSchemaDataBucket) {
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"02091c2c050b7495eaef4581"},
            "control":{"version":1,
                       "min":{"_id":{"$oid":"63091c30138e9261fd70a903"},
                              "time":{"$date":"2022-08-26T19:19:00Z"},
                              "x":1},
                       "max":{"_id":{"$oid":"63091c30138e9261fd70a905"},
                       "time":{"$date":"2022-08-26T19:19:30Z"},
                       "x":{"y":"z"}}},
            "data":{"_id":{"0":{"$oid":"63091c30138e9261fd70a903"},
                           "1":{"$oid":"63091c30138e9261fd70a904"},
                           "2":{"$oid":"63091c30138e9261fd70a905"}},
                    "time":{"0":{"$date":"2022-08-26T19:19:30Z"},
                            "1":{"$date":"2022-08-26T19:19:30Z"},
                            "2":{"$date":"2022-08-26T19:19:30Z"}},
                    "x":{"0":1,"1":{"y":"z"},"2":"abc"}}})");

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), bucketDoc));

    auto stats = internal::getExecutionStats(*_bucketCatalog, _uuid1);
    ASSERT_EQ(1, stats->numBucketReopeningsFailed.load());
}

TEST_F(BucketCatalogTest, ReopenClosedBuckets) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    {
        // control.closed: true
        BSONObj closedBucket = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3},
                                   "closed": true},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
        ASSERT_NOT_OK(_reopenBucket(autoColl.getCollection(), closedBucket));
    }

    {
        // control.closed: false
        BSONObj openBucket = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a518"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3},
                                   "closed": false},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
        ASSERT_OK(_reopenBucket(autoColl.getCollection(), openBucket));
    }

    {
        // No control.closed
        BSONObj openBucket = ::mongo::fromjson(
            R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");
        ASSERT_OK(_reopenBucket(autoColl.getCollection(), openBucket));
    }
}

TEST_F(BucketCatalogTest, ReopenUncompressedBucketAndInsertCompatibleMeasurement) {
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

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    auto memUsageBefore = getMemoryUsage(*_bucketCatalog);
    Status status = _reopenBucket(autoColl.getCollection(), bucketDoc);
    auto memUsageAfter = getMemoryUsage(*_bucketCatalog);
    ASSERT_OK(status);
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumBucketsReopened));
    ASSERT_GT(memUsageAfter, memUsageBefore);

    // Insert a measurement that is compatible with the reopened bucket.
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":-100,"b":100})"),
                         CombineWithInsertsFromOtherClients::kAllow);

    // No buckets are closed.
    ASSERT(get<SuccessfulInsertion>(result.getValue()).closedBuckets.empty());
    ASSERT_EQ(0, _getExecutionStat(_uuid1, kNumSchemaChanges));

    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);

    // The reopened bucket already contains three committed measurements.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 3);

    // Verify that the min and max is updated correctly when inserting new measurements.
    ASSERT_BSONOBJ_BINARY_EQ(batch->min, BSON("u" << BSON("a" << -100)));
    ASSERT_BSONOBJ_BINARY_EQ(
        batch->max,
        BSON("u" << BSON("time" << Date_t::fromMillisSinceEpoch(1654529680000) << "b" << 100)));

    finish(_opCtx, *_bucketCatalog, _ns1, batch, {});
}

TEST_F(BucketCatalogTest, ReopenUncompressedBucketAndInsertCompatibleMeasurementWithMeta) {
    // Bucket document to reopen.
    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a642"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"},"a":1,"b":1},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"},"a":3,"b":3}},
            "meta": 42,
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"},
                            "1":{"$date":"2022-06-06T15:34:30.000Z"},
                            "2":{"$date":"2022-06-06T15:34:30.000Z"}},
                    "a":{"0":1,"1":2,"2":3},
                    "b":{"0":1,"1":2,"2":3}}})");

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    Status status = _reopenBucket(autoColl.getCollection(), bucketDoc);
    ASSERT_OK(status);

    // Insert a measurement that is compatible with the reopened bucket.
    auto result =
        insert(_opCtx,
               *_bucketCatalog,
               _ns1,
               _uuid1,
               _getCollator(_ns1),
               _getTimeseriesOptions(_ns1),
               ::mongo::fromjson(
                   R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},"tag":42, "a":-100,"b":100})"),
               CombineWithInsertsFromOtherClients::kAllow);

    // No buckets are closed.
    ASSERT(get<SuccessfulInsertion>(result.getValue()).closedBuckets.empty());
    ASSERT_EQ(0, _getExecutionStat(_uuid1, kNumSchemaChanges));

    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);

    // The reopened bucket already contains three committed measurements.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 3);

    // Verify that the min and max is updated correctly when inserting new measurements.
    ASSERT_BSONOBJ_BINARY_EQ(batch->min, BSON("u" << BSON("a" << -100)));
    ASSERT_BSONOBJ_BINARY_EQ(
        batch->max,
        BSON("u" << BSON("time" << Date_t::fromMillisSinceEpoch(1654529680000) << "b" << 100)));

    finish(_opCtx, *_bucketCatalog, _ns1, batch, {});
}

TEST_F(BucketCatalogTest, ReopenUncompressedBucketAndInsertIncompatibleMeasurement) {
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

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    auto memUsageBefore = getMemoryUsage(*_bucketCatalog);
    Status status = _reopenBucket(autoColl.getCollection(), bucketDoc);
    auto memUsageAfter = getMemoryUsage(*_bucketCatalog);
    ASSERT_OK(status);
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumBucketsReopened));
    ASSERT_GT(memUsageAfter, memUsageBefore);

    // Insert a measurement that is incompatible with the reopened bucket.
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":{},"b":{}})"),
                         CombineWithInsertsFromOtherClients::kAllow);

    // The reopened bucket gets closed as the schema is incompatible.
    ASSERT_EQ(1, get<SuccessfulInsertion>(result.getValue()).closedBuckets.size());
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumSchemaChanges));

    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);

    // Since the reopened bucket was incompatible, we opened a new one.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 0);

    finish(_opCtx, *_bucketCatalog, _ns1, batch, {});
}

TEST_F(BucketCatalogTest, ReopenCompressedBucketAndInsertCompatibleMeasurement) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTimeseriesAlwaysUseCompressedBuckets", true);

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

    CompressionResult compressionResult = compressBucket(bucketDoc,
                                                         _timeField,
                                                         _ns1,
                                                         /*validateDecompression*/ true);
    const BSONObj& compressedBucketDoc = compressionResult.compressedBucket.value();

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    auto memUsageBefore = getMemoryUsage(*_bucketCatalog);
    Status status = _reopenBucket(autoColl.getCollection(), compressedBucketDoc);
    auto memUsageAfter = getMemoryUsage(*_bucketCatalog);
    ASSERT_OK(status);
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumBucketsReopened));
    ASSERT_GT(memUsageAfter, memUsageBefore);

    // Insert a measurement that is compatible with the reopened bucket.
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":-100,"b":100})"),
                         CombineWithInsertsFromOtherClients::kAllow);

    // No buckets are closed.
    ASSERT(get<SuccessfulInsertion>(result.getValue()).closedBuckets.empty());
    ASSERT_EQ(0, _getExecutionStat(_uuid1, kNumSchemaChanges));

    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);

    // The reopened bucket already contains three committed measurements.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 3);

    // Verify that the min and max is updated correctly when inserting new measurements.
    ASSERT_BSONOBJ_BINARY_EQ(batch->min, BSON("u" << BSON("a" << -100)));
    ASSERT_BSONOBJ_BINARY_EQ(
        batch->max,
        BSON("u" << BSON("time" << Date_t::fromMillisSinceEpoch(1654529680000) << "b" << 100)));

    finish(_opCtx, *_bucketCatalog, _ns1, batch, {});
}

TEST_F(BucketCatalogTest, ReopenCompressedBucketAndInsertIncompatibleMeasurement) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagTimeseriesAlwaysUseCompressedBuckets", true);

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

    CompressionResult compressionResult = compressBucket(bucketDoc,
                                                         _timeField,
                                                         _ns1,
                                                         /*validateDecompression*/ true);
    const BSONObj& compressedBucketDoc = compressionResult.compressedBucket.value();

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    auto memUsageBefore = getMemoryUsage(*_bucketCatalog);
    Status status = _reopenBucket(autoColl.getCollection(), compressedBucketDoc);
    auto memUsageAfter = getMemoryUsage(*_bucketCatalog);
    ASSERT_OK(status);
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumBucketsReopened));
    ASSERT_GT(memUsageAfter, memUsageBefore);

    // Insert a measurement that is incompatible with the reopened bucket.
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},
                                                     "a":{},"b":{}})"),
                         CombineWithInsertsFromOtherClients::kAllow);

    // The reopened bucket is closed but not added to closedBuckets anyore, when the feature flag
    // is enabled, because closedBuckets only stored buckets that would need to be compressed on
    // closing. With the feature flag enabled buckets are already compressed.
    ASSERT_EQ(0, get<SuccessfulInsertion>(result.getValue()).closedBuckets.size());
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumSchemaChanges));

    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);

    // Since the reopened bucket was incompatible, we opened a new one.
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements, 0);

    finish(_opCtx, *_bucketCatalog, _ns1, batch, {});
}

TEST_F(BucketCatalogTest, ReopenCompressedBucketFails) {
    // Reopening compressed buckets is not supported without the always use compressed buckets
    // feature flag.

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

    CompressionResult compressionResult = compressBucket(bucketDoc,
                                                         _timeField,
                                                         _ns1,
                                                         /*validateDecompression*/ true);
    const BSONObj& compressedBucketDoc = compressionResult.compressedBucket.value();

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);
    Status status = _reopenBucket(autoColl.getCollection(), compressedBucketDoc);
    ASSERT_EQ(ErrorCodes::BadValue, status);
}

TEST_F(BucketCatalogTest, ArchivingUnderMemoryPressure) {
    RAIIServerParameterControllerForTest memoryLimit{
        "timeseriesIdleBucketExpiryMemoryUsageThreshold", 10000};

    // Insert a measurement with a unique meta value, guaranteeing we will open a new bucket but not
    // close an old one except under memory pressure.
    long long meta = 0;
    auto insertDocument = [&meta, this]() -> ClosedBuckets {
        auto result = insert(_opCtx,
                             *_bucketCatalog,
                             _ns1,
                             _uuid1,
                             _getCollator(_ns1),
                             _getTimeseriesOptions(_ns1),
                             BSON(_timeField << Date_t::now() << _metaField << meta++),
                             CombineWithInsertsFromOtherClients::kAllow);
        ASSERT_OK(result.getStatus());
        auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
        ASSERT(claimWriteBatchCommitRights(*batch));
        ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
        finish(_opCtx, *_bucketCatalog, _ns1, batch, {});

        return std::move(get<SuccessfulInsertion>(result.getValue()).closedBuckets);
    };

    // Ensure we start out with no buckets archived or closed due to memory pressure.
    ASSERT_EQ(0, _getExecutionStat(_uuid1, kNumArchivedDueToMemoryThreshold));
    ASSERT_EQ(0, _getExecutionStat(_uuid1, kNumClosedDueToMemoryThreshold));

    // With a memory limit of 10000 bytes, we should be guaranteed to hit the memory limit with no
    // more than 1000 buckets since an open bucket takes up at least 10 bytes (in reality,
    // significantly more, but this is definitely a safe assumption).
    for (int i = 0; i < 1000; ++i) {
        [[maybe_unused]] auto closedBuckets = insertDocument();

        if (0 < _getExecutionStat(_uuid1, kNumArchivedDueToMemoryThreshold)) {
            break;
        }
    }

    // When we first hit the limit, we should try to archive some buckets prior to closing anything.
    // However, depending on how the buckets are distributed over the stripes, it's possible that
    // the current stripe will not have enough open buckets to archive to drop below the limit, and
    // may immediately close a bucket it has just archived. We should be able to guarantee that we
    // have archived a bucket prior to closing it though.
    ASSERT_LT(0, _getExecutionStat(_uuid1, kNumArchivedDueToMemoryThreshold));
    auto numClosedInFirstRound = _getExecutionStat(_uuid1, kNumClosedDueToMemoryThreshold);
    ASSERT_LTE(numClosedInFirstRound, _getExecutionStat(_uuid1, kNumArchivedDueToMemoryThreshold));

    // If we continue to open more new buckets with distinct meta values, eventually we'll run out
    // of open buckets to archive and have to start closing archived buckets to relieve memory
    // pressure. Again, an archived bucket should take up more than 10 bytes in the catalog, so we
    // should be fine with a maximum of 1000 iterations.
    for (int i = 0; i < 1000; ++i) {
        auto closedBuckets = insertDocument();

        if (numClosedInFirstRound < _getExecutionStat(_uuid1, kNumClosedDueToMemoryThreshold)) {
            ASSERT_FALSE(closedBuckets.empty());
            break;
        }
    }

    // We should have closed some (additional) buckets by now.
    ASSERT_LT(numClosedInFirstRound, _getExecutionStat(_uuid1, kNumClosedDueToMemoryThreshold));
}

TEST_F(BucketCatalogTest, TryInsertWillNotCreateBucketWhenWeShouldTryToReopen) {
    FailPointEnableBlock failPoint("alwaysUseSameBucketCatalogStripe");
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // Try to insert with no open bucket. Should hint to re-open.
    {
        auto result =
            tryInsert(_opCtx,
                      *_bucketCatalog,
                      _ns1,
                      _uuid1,
                      _getCollator(_ns1),
                      _getTimeseriesOptions(_ns1),
                      ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"}})"),
                      CombineWithInsertsFromOtherClients::kAllow);
        ASSERT_OK(result.getStatus());
        ASSERT(holds_alternative<ReopeningContext>(result.getValue()));
        ASSERT_TRUE(holds_alternative<std::vector<BSONObj>>(
            get<ReopeningContext>(result.getValue()).candidate));
    }

    // Actually insert so we do have an open bucket to test against.
    BucketId bucketId{_uuid1, OID::gen()};  // placeholder initialization, will be set properlybelow
    {
        auto result = insert(_opCtx,
                             *_bucketCatalog,
                             _ns1,
                             _uuid1,
                             _getCollator(_ns1),
                             _getTimeseriesOptions(_ns1),
                             ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"}})"),
                             CombineWithInsertsFromOtherClients::kAllow);
        ASSERT_OK(result.getStatus());
        auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
        ASSERT(batch);
        bucketId = batch->bucketHandle.bucketId;
        ASSERT(claimWriteBatchCommitRights(*batch));
        ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
        ASSERT_EQ(batch->measurements.size(), 1);
        finish(_opCtx, *_bucketCatalog, _ns1, batch, {});
    }

    // Time backwards should hint to re-open.
    {
        auto result =
            tryInsert(_opCtx,
                      *_bucketCatalog,
                      _ns1,
                      _uuid1,
                      _getCollator(_ns1),
                      _getTimeseriesOptions(_ns1),
                      ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:40.000Z"}})"),
                      CombineWithInsertsFromOtherClients::kAllow);
        ASSERT_OK(result.getStatus());
        ASSERT(holds_alternative<ReopeningContext>(result.getValue()));
        ASSERT_TRUE(holds_alternative<std::vector<BSONObj>>(
            get<ReopeningContext>(result.getValue()).candidate));
    }

    // Time forward should not hint to re-open.
    {
        auto result =
            tryInsert(_opCtx,
                      *_bucketCatalog,
                      _ns1,
                      _uuid1,
                      _getCollator(_ns1),
                      _getTimeseriesOptions(_ns1),
                      ::mongo::fromjson(R"({"time":{"$date":"2022-06-07T15:34:40.000Z"}})"),
                      CombineWithInsertsFromOtherClients::kAllow);
        ASSERT_OK(result.getStatus());
        ASSERT(holds_alternative<ReopeningContext>(result.getValue()));
        ASSERT(
            holds_alternative<std::monostate>(get<ReopeningContext>(result.getValue()).candidate));
    }

    // Set a memory limit that only allows one open bucket at a time.
    int memoryThreshold = getMemoryUsage(*_bucketCatalog) - 1;
    RAIIServerParameterControllerForTest memoryController{
        "timeseriesIdleBucketExpiryMemoryUsageThreshold", memoryThreshold};

    // Now let's insert something with a different meta, so we open a new bucket, see we're past the
    // memory limit, and archive the existing bucket.
    {
        auto result = insert(
            _opCtx,
            *_bucketCatalog,
            _ns1,
            _uuid1,
            _getCollator(_ns1),
            _getTimeseriesOptions(_ns1),
            ::mongo::fromjson(R"({"time":{"$date":"2022-06-07T15:34:40.000Z"}, "tag": "foo"})"),
            CombineWithInsertsFromOtherClients::kAllow);
        ASSERT_OK(result.getStatus());
        ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumArchivedDueToMemoryThreshold));
        ASSERT_EQ(0, _getExecutionStat(_uuid1, kNumClosedDueToMemoryThreshold));
        auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
        ASSERT_NE(batch->bucketHandle.bucketId, bucketId);
        ASSERT(batch);
        ASSERT(claimWriteBatchCommitRights(*batch));
        ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
        ASSERT_EQ(batch->measurements.size(), 1);
        finish(_opCtx, *_bucketCatalog, _ns1, batch, {});
    }

    // If we try to insert something that could fit in the archived bucket, we should get it back as
    // a candidate.
    {
        auto result =
            tryInsert(_opCtx,
                      *_bucketCatalog,
                      _ns1,
                      _uuid1,
                      _getCollator(_ns1),
                      _getTimeseriesOptions(_ns1),
                      ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:35:40.000Z"}})"),
                      CombineWithInsertsFromOtherClients::kAllow);
        ASSERT_OK(result.getStatus());
        ASSERT(holds_alternative<ReopeningContext>(result.getValue()));
        ASSERT_TRUE(holds_alternative<OID>(get<ReopeningContext>(result.getValue()).candidate));
        ASSERT_EQ(get<OID>(get<ReopeningContext>(result.getValue()).candidate), bucketId.oid);
    }
}

TEST_F(BucketCatalogTest, TryInsertWillCreateBucketIfWeWouldCloseExistingBucket) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // Insert a document so we have a base bucket
    auto result =
        insert(_opCtx,
               *_bucketCatalog,
               _ns1,
               _uuid1,
               _getCollator(_ns1),
               _getTimeseriesOptions(_ns1),
               ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"}, "a": true})"),
               CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(batch);
    auto bucketId = batch->bucketHandle.bucketId;
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);
    finish(_opCtx, *_bucketCatalog, _ns1, batch, {});

    // Incompatible schema would close the existing bucket, so we should expect to open a new bucket
    // and proceed to insert the document.
    result =
        tryInsert(_opCtx,
                  *_bucketCatalog,
                  _ns1,
                  _uuid1,
                  _getCollator(_ns1),
                  _getTimeseriesOptions(_ns1),
                  ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:35:40.000Z"}, "a": {}})"),
                  CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(batch);
    ASSERT_NE(batch->bucketHandle.bucketId, bucketId);
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);
    finish(_opCtx, *_bucketCatalog, _ns1, batch, {});
}

TEST_F(BucketCatalogTest, InsertIntoReopenedBucket) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // Insert a document so we have a base bucket and we can test that we soft close it when we
    // reopen a conflicting bucket.
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:40.000Z"}})"),
                         CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(batch);
    auto oldBucketId = batch->bucketHandle.bucketId;
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);
    finish(_opCtx, *_bucketCatalog, _ns1, batch, {});

    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"}},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"}}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"}}}})");
    ASSERT_NE(bucketDoc["_id"].OID(), oldBucketId.oid);
    auto validator = [&](OperationContext * opCtx, const BSONObj& bucketDoc) -> auto {
        return autoColl->checkValidation(opCtx, bucketDoc);
    };

    ReopeningContext reopeningContext{*_bucketCatalog,
                                      *_bucketCatalog->stripes[0],
                                      WithLock::withoutLock(),
                                      batch->bucketKey.cloneAsUntracked(),
                                      getCurrentEra(_bucketCatalog->bucketStateRegistry),
                                      {}};
    reopeningContext.bucketToReopen = BucketToReopen{bucketDoc, validator};

    // We should be able to pass in a valid bucket and insert into it.
    result = insertWithReopeningContext(
        _opCtx,
        *_bucketCatalog,
        _ns1,
        _uuid1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:35:40.000Z"}})"),
        CombineWithInsertsFromOtherClients::kAllow,
        reopeningContext);
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(holds_alternative<SuccessfulInsertion>(result.getValue()));
    batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(batch);
    ASSERT_EQ(batch->bucketHandle.bucketId.oid, bucketDoc["_id"].OID());
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);
    finish(_opCtx, *_bucketCatalog, _ns1, batch, {});
    // Verify the old bucket was soft-closed
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumClosedDueToReopening));
    ASSERT_EQ(1, _getExecutionStat(_uuid1, kNumBucketsReopened));
    ASSERT_FALSE(get<SuccessfulInsertion>(result.getValue()).closedBuckets.empty());

    // Verify that if we try another insert for the soft-closed bucket, we get a query-based
    // reopening candidate.
    result = tryInsert(_opCtx,
                       *_bucketCatalog,
                       _ns1,
                       _uuid1,
                       _getCollator(_ns1),
                       _getTimeseriesOptions(_ns1),
                       ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:35:40.000Z"}})"),
                       CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(holds_alternative<ReopeningContext>(result.getValue()));
    ASSERT_TRUE(holds_alternative<std::vector<BSONObj>>(
        get<ReopeningContext>(result.getValue()).candidate));
}

TEST_F(BucketCatalogTest, CannotInsertIntoOutdatedBucket) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // Insert a document so we have a base bucket and we can test that we archive it when we reopen
    // a conflicting bucket.
    auto result = insert(_opCtx,
                         *_bucketCatalog,
                         _ns1,
                         _uuid1,
                         _getCollator(_ns1),
                         _getTimeseriesOptions(_ns1),
                         ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:40.000Z"}})"),
                         CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result.getStatus());
    auto batch = get<SuccessfulInsertion>(result.getValue()).batch;
    ASSERT(batch);
    auto oldBucketId = batch->bucketHandle.bucketId;
    ASSERT(claimWriteBatchCommitRights(*batch));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
    ASSERT_EQ(batch->measurements.size(), 1);
    finish(_opCtx, *_bucketCatalog, _ns1, batch, {});

    BSONObj bucketDoc = ::mongo::fromjson(
        R"({"_id":{"$oid":"629e1e680958e279dc29a517"},
            "control":{"version":1,"min":{"time":{"$date":"2022-06-06T15:34:00.000Z"}},
                                   "max":{"time":{"$date":"2022-06-06T15:34:30.000Z"}}},
            "data":{"time":{"0":{"$date":"2022-06-06T15:34:30.000Z"}}}})");
    ASSERT_NE(bucketDoc["_id"].OID(), oldBucketId.oid);
    auto validator = [&](OperationContext * opCtx, const BSONObj& bucketDoc) -> auto {
        return autoColl->checkValidation(opCtx, bucketDoc);
    };

    // If we advance the catalog era, then we shouldn't use a bucket that was fetched during a
    // previous era.
    auto oldCatalogEra = getCurrentEra(_bucketCatalog->bucketStateRegistry);
    const UUID fakeUUID = UUID::gen();
    const auto fakeId = OID();
    directWriteStart(_bucketCatalog->bucketStateRegistry, fakeUUID, fakeId);
    directWriteFinish(_bucketCatalog->bucketStateRegistry, fakeUUID, fakeId);

    ReopeningContext reopeningContext{*_bucketCatalog,
                                      *_bucketCatalog->stripes[0],
                                      WithLock::withoutLock(),
                                      batch->bucketKey.cloneAsUntracked(),
                                      oldCatalogEra,
                                      {}};
    reopeningContext.bucketToReopen = BucketToReopen{bucketDoc, validator};

    // We should get an WriteConflict back if we pass in an outdated bucket.
    result = insertWithReopeningContext(
        _opCtx,
        *_bucketCatalog,
        _ns1,
        _uuid1,
        _getCollator(_ns1),
        _getTimeseriesOptions(_ns1),
        ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:35:40.000Z"}})"),
        CombineWithInsertsFromOtherClients::kAllow,
        reopeningContext);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::WriteConflict);
}

TEST_F(BucketCatalogTest, QueryBasedReopeningConflictsWithQueryBasedReopening) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // First attempt to insert to a series should trigger a reopening request to check for a bucket
    // on disk.
    auto result1 =
        tryInsert(_opCtx,
                  *_bucketCatalog,
                  _ns1,
                  _uuid1,
                  _getCollator(_ns1),
                  _getTimeseriesOptions(_ns1),
                  ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:40.000Z"},"tag":"a"})"),
                  CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result1.getStatus());
    auto* context = get_if<ReopeningContext>(&result1.getValue());
    ASSERT(context);
    ASSERT(holds_alternative<std::vector<BSONObj>>(context->candidate));

    // A subsequent attempt while the first one is still outstanding should conflict and yield a
    // InsertWaiter.
    auto result2 =
        tryInsert(_opCtx,
                  *_bucketCatalog,
                  _ns1,
                  _uuid1,
                  _getCollator(_ns1),
                  _getTimeseriesOptions(_ns1),
                  ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:50.000Z"},"tag":"a"})"),
                  CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result2.getStatus());
    ASSERT(holds_alternative<InsertWaiter>(result2.getValue()));
}

TEST_F(BucketCatalogTest, ReopeningConflictsWithPreparedBatch) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // Stage and prepare an insert.
    auto result1 =
        insert(_opCtx,
               *_bucketCatalog,
               _ns1,
               _uuid1,
               _getCollator(_ns1),
               _getTimeseriesOptions(_ns1),
               ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:40.000Z"},"tag":"b"})"),
               CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result1.getStatus());
    auto batch1 = get<SuccessfulInsertion>(result1.getValue()).batch;
    ASSERT(batch1);
    ASSERT(claimWriteBatchCommitRights(*batch1));
    ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch1));
    ASSERT_EQ(batch1->measurements.size(), 1);

    // Stage and abort another insert on the same bucket, so that new inserts can't land without
    // reopening.
    auto result2 =
        insert(_opCtx,
               *_bucketCatalog,
               _ns1,
               _uuid1,
               _getCollator(_ns1),
               _getTimeseriesOptions(_ns1),
               ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:45.000Z"},"tag":"b"})"),
               CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result2.getStatus());
    auto batch2 = get<SuccessfulInsertion>(result2.getValue()).batch;
    ASSERT(batch2);
    ASSERT(claimWriteBatchCommitRights(*batch2));
    abort(*_bucketCatalog, batch2, {ErrorCodes::WriteConflict, "foo"});

    // A subsequent attempt to reopen a bucket should conflict and yield a InsertWaiter.
    auto result3 =
        tryInsert(_opCtx,
                  *_bucketCatalog,
                  _ns1,
                  _uuid1,
                  _getCollator(_ns1),
                  _getTimeseriesOptions(_ns1),
                  ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:50.000Z"},"tag":"b"})"),
                  CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result3.getStatus());
    ASSERT(holds_alternative<InsertWaiter>(result3.getValue()));
}

TEST_F(BucketCatalogTest, PreparingBatchConflictsWithQueryBasedReopening) {
    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // First attempt to insert to a series should trigger a reopening request to check for a bucket
    // on disk.
    boost::optional<StatusWith<InsertResult>> result1 =
        tryInsert(_opCtx,
                  *_bucketCatalog,
                  _ns1,
                  _uuid1,
                  _getCollator(_ns1),
                  _getTimeseriesOptions(_ns1),
                  ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:40.000Z"},"tag":"c"})"),
                  CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result1->getStatus());
    auto* context = get_if<ReopeningContext>(&result1->getValue());
    ASSERT(context);
    ASSERT(holds_alternative<std::vector<BSONObj>>(context->candidate));

    // Stage an insert for the same series, but a different bucket.
    auto result2 =
        insert(_opCtx,
               *_bucketCatalog,
               _ns1,
               _uuid1,
               _getCollator(_ns1),
               _getTimeseriesOptions(_ns1),
               ::mongo::fromjson(R"({"time":{"$date":"2022-07-05T15:34:40.000Z"},"tag":"c"})"),
               CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result2.getStatus());
    auto batch = get<SuccessfulInsertion>(result2.getValue()).batch;
    ASSERT(batch);
    ASSERT(claimWriteBatchCommitRights(*batch));

    // Ensure it blocks until we resolve the reopening request.
    auto task = RunBackgroundTaskAndWaitForFailpoint{
        "hangTimeSeriesBatchPrepareWaitingForConflictingOperation", [&]() {
            ASSERT_OK(prepareCommit(*_bucketCatalog, _ns1, batch));
        }};
    result1 = boost::none;
}

TEST_F(BucketCatalogTest, ArchiveBasedReopeningConflictsWithArchiveBasedReopeningOnSameBucket) {
    // Simplify test by restricting to a single stripe.
    FailPointEnableBlock failPoint("alwaysUseSameBucketCatalogStripe");

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // Inject an archived record.
    auto options = _getTimeseriesOptions(_ns1);
    BSONObj doc = ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:40.000Z"},"tag":"c"})");
    TrackingContext trackingContext;
    BucketKey key{_uuid1,
                  BucketMetadata{trackingContext, doc["tag"], nullptr, options.getMetaField()}};
    auto minTime = roundTimestampToGranularity(doc["time"].Date(), options);
    BucketId id{_uuid1, OID::gen()};
    ASSERT_OK(initializeBucketState(_bucketCatalog->bucketStateRegistry, id));
    _bucketCatalog->stripes[0]->archivedBuckets[key.hash].emplace(
        minTime,
        ArchivedBucket{id,
                       make_tracked_string(_bucketCatalog->trackingContext,
                                           options.getTimeField().toString())});

    // Should try to reopen archived bucket.
    boost::optional<StatusWith<InsertResult>> result1 =
        tryInsert(_opCtx,
                  *_bucketCatalog,
                  _ns1,
                  _uuid1,
                  _getCollator(_ns1),
                  options,
                  doc,
                  CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result1->getStatus());
    auto* context = get_if<ReopeningContext>(&result1->getValue());
    ASSERT(context);
    auto* oid = get_if<OID>(&context->candidate);
    ASSERT(oid);
    ASSERT_EQ(*oid, id.oid);

    // A second attempt should block.
    boost::optional<StatusWith<InsertResult>> result2 =
        tryInsert(_opCtx,
                  *_bucketCatalog,
                  _ns1,
                  _uuid1,
                  _getCollator(_ns1),
                  options,
                  doc,
                  CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result2->getStatus());
    ASSERT(holds_alternative<InsertWaiter>(result2->getValue()));
}

TEST_F(BucketCatalogTest,
       ArchiveBasedReopeningDoesNotConflictWithArchiveBasedReopeningOnDifferentBucket) {
    // Simplify test by restricting to a single stripe.
    FailPointEnableBlock failPoint("alwaysUseSameBucketCatalogStripe");

    AutoGetCollection autoColl(_opCtx, _ns1.makeTimeseriesBucketsNamespace(), MODE_IX);

    // Inject an archived record.
    auto options = _getTimeseriesOptions(_ns1);
    BSONObj doc1 = ::mongo::fromjson(R"({"time":{"$date":"2022-06-05T15:34:40.000Z"},"tag":"c"})");
    TrackingContext trackingContext;
    BucketKey key{_uuid1,
                  BucketMetadata{trackingContext, doc1["tag"], nullptr, options.getMetaField()}};
    auto minTime1 = roundTimestampToGranularity(doc1["time"].Date(), options);
    BucketId id1{_uuid1, OID::gen()};
    ASSERT_OK(initializeBucketState(_bucketCatalog->bucketStateRegistry, id1));
    _bucketCatalog->stripes[0]->archivedBuckets[key.hash].emplace(
        minTime1,
        ArchivedBucket{id1,
                       make_tracked_string(_bucketCatalog->trackingContext,
                                           options.getTimeField().toString())});

    // Should try to reopen archived bucket.
    boost::optional<StatusWith<InsertResult>> result1 =
        tryInsert(_opCtx,
                  *_bucketCatalog,
                  _ns1,
                  _uuid1,
                  _getCollator(_ns1),
                  options,
                  doc1,
                  CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result1->getStatus());
    auto* context1 = get_if<ReopeningContext>(&result1->getValue());
    ASSERT(context1);
    auto* oid1 = get_if<OID>(&context1->candidate);
    ASSERT(oid1);
    ASSERT_EQ(*oid1, id1.oid);

    // Inject another archived record on the same series, but a different bucket.
    BSONObj doc2 = ::mongo::fromjson(R"({"time":{"$date":"2022-06-06T15:34:40.000Z"},"tag":"c"})");
    auto minTime2 = roundTimestampToGranularity(doc2["time"].Date(), options);
    BucketId id2{_uuid1, OID::gen()};
    ASSERT_OK(initializeBucketState(_bucketCatalog->bucketStateRegistry, id2));
    _bucketCatalog->stripes[0]->archivedBuckets[key.hash].emplace(
        minTime2,
        ArchivedBucket{id2,
                       make_tracked_string(_bucketCatalog->trackingContext,
                                           options.getTimeField().toString())});

    // A second attempt should block.
    boost::optional<StatusWith<InsertResult>> result2 =
        tryInsert(_opCtx,
                  *_bucketCatalog,
                  _ns1,
                  _uuid1,
                  _getCollator(_ns1),
                  options,
                  doc2,
                  CombineWithInsertsFromOtherClients::kAllow);
    ASSERT_OK(result2->getStatus());
    auto* context2 = get_if<ReopeningContext>(&result2->getValue());
    ASSERT(context2);
    auto* oid2 = get_if<OID>(&context2->candidate);
    ASSERT(oid2);
    ASSERT_EQ(*oid2, id2.oid);
}

TEST_F(BucketCatalogTest, ArchivingAndClosingUnderSideBucketCatalogMemoryPressure) {
    // Initialize the side bucket catalog.
    auto sideBucketCatalog = std::make_unique<timeseries::bucket_catalog::BucketCatalog>(
        1, getTimeseriesSideBucketCatalogMemoryUsageThresholdBytes);
    ClosedBuckets closedBuckets;

    // Create dummy bucket and populate bucket state registry.
    TrackingContext trackingContext;
    auto dummyUUID = UUID::gen();
    auto dummyBucketId = BucketId(dummyUUID, OID());
    auto dummyBucketKey =
        BucketKey(dummyUUID, BucketMetadata(trackingContext, BSONElement{}, nullptr, boost::none));
    sideBucketCatalog->bucketStateRegistry.bucketStates.emplace(dummyBucketId,
                                                                BucketState::kNormal);
    auto dummyBucket = std::make_unique<Bucket>(trackingContext,
                                                dummyBucketId,
                                                dummyBucketKey.cloneAsUntracked(),
                                                "time",
                                                Date_t(),
                                                sideBucketCatalog->bucketStateRegistry);

    // Create and populate stripe.
    auto& stripe = *sideBucketCatalog->stripes[0];
    stripe.openBucketsById.try_emplace(
        dummyBucketId,
        make_unique_tracked<Bucket>(sideBucketCatalog->trackingContext,
                                    sideBucketCatalog->trackingContext,
                                    dummyBucketId,
                                    dummyBucketKey.cloneAsUntracked(),
                                    "time",
                                    Date_t(),
                                    sideBucketCatalog->bucketStateRegistry));
    stripe.openBucketsByKey[dummyBucketKey.cloneAsUntracked()].emplace(dummyBucket.get());
    stripe.idleBuckets.push_front(dummyBucket.get());
    stdx::lock_guard stripeLock{stripe.mutex};

    // Create execution stats controller.
    auto collectionStats = std::make_shared<bucket_catalog::ExecutionStats>();
    auto statsController =
        ExecutionStatsController(collectionStats, sideBucketCatalog->globalExecutionStats);

    // Ensure we start out with no buckets archived or closed due to memory pressure.
    ASSERT_EQ(0, collectionStats->numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(0, collectionStats->numBucketsClosedDueToMemoryThreshold.load());
    ASSERT_EQ(
        0, sideBucketCatalog->globalExecutionStats.numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(0,
              sideBucketCatalog->globalExecutionStats.numBucketsClosedDueToMemoryThreshold.load());


    // Set the catalog memory usage to be above the memory usage threshold by the amount of memory
    // used by the idle bucket.
    sideBucketCatalog->trackingContext.stats().bytesAllocated(
        getTimeseriesSideBucketCatalogMemoryUsageThresholdBytes() -
        sideBucketCatalog->trackingContext.allocated() + trackingContext.allocated());

    // When we exceed the memory usage threshold we will first try to archive idle buckets to try
    // to get below the threshold. If this does not get us beneath the threshold, we will then try
    // to close archived buckets, until we hit the global expiry max count limit (or if we run out
    // of idle buckets in this stripe). Then, we try to close any archived buckets. In this
    // particular execution we should expect not to close any buckets, but we should archive one.
    internal::expireIdleBuckets(_makeOperationContext().second.get(),
                                *sideBucketCatalog,
                                stripe,
                                stripeLock,
                                statsController,
                                closedBuckets);

    ASSERT_EQ(1, collectionStats->numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(
        1, sideBucketCatalog->globalExecutionStats.numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(0, collectionStats->numBucketsClosedDueToMemoryThreshold.load());
    ASSERT_EQ(0,
              sideBucketCatalog->globalExecutionStats.numBucketsClosedDueToMemoryThreshold.load());

    // Clears the list of idle buckets - usually this is done within the expire idle buckets
    // function, but that requires setting up more state with the bucket's idleListEntry. This gets
    // around that for testing purposes.
    stripe.idleBuckets.clear();

    // Set the memory usage to be back at the threshold. Now, when we run expire idle buckets again,
    // because there are no idle buckets left to archive, we will close the bucket that we
    // previously archived.
    sideBucketCatalog->trackingContext.stats().bytesAllocated(
        getTimeseriesSideBucketCatalogMemoryUsageThresholdBytes() + 1);
    internal::expireIdleBuckets(_makeOperationContext().second.get(),
                                *sideBucketCatalog,
                                stripe,
                                stripeLock,
                                statsController,
                                closedBuckets);

    ASSERT_EQ(1, collectionStats->numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(
        1, sideBucketCatalog->globalExecutionStats.numBucketsArchivedDueToMemoryThreshold.load());
    ASSERT_EQ(1, collectionStats->numBucketsClosedDueToMemoryThreshold.load());
    ASSERT_EQ(1,
              sideBucketCatalog->globalExecutionStats.numBucketsClosedDueToMemoryThreshold.load());
}

TEST_F(BucketCatalogTest, GetCacheDerivedBucketMaxSize) {
    auto [effectiveMaxSize, cacheDerivedBucketMaxSize] = internal::getCacheDerivedBucketMaxSize(
        /*storageCacheSize=*/128 * 1000 * 1000, /*workloadCardinality=*/1000);
    ASSERT_EQ(effectiveMaxSize, 64 * 1000);
    ASSERT_EQ(cacheDerivedBucketMaxSize, 64 * 1000);

    std::tie(effectiveMaxSize, cacheDerivedBucketMaxSize) = internal::getCacheDerivedBucketMaxSize(
        /*storageCacheSize=*/0, /*workloadCardinality=*/1000);
    ASSERT_EQ(effectiveMaxSize, gTimeseriesBucketMinSize.load());
    ASSERT_EQ(cacheDerivedBucketMaxSize, gTimeseriesBucketMinSize.load());

    std::tie(effectiveMaxSize, cacheDerivedBucketMaxSize) = internal::getCacheDerivedBucketMaxSize(
        /*storageCacheSize=*/128 * 1000 * 1000, /*workloadCardinality=*/0);
    ASSERT_EQ(effectiveMaxSize, gTimeseriesBucketMaxSize);
    ASSERT_EQ(cacheDerivedBucketMaxSize, INT_MAX);
}

TEST_F(BucketCatalogTest, GetCacheDerivedBucketMaxSizeRespectsAbsoluteMax) {
    auto [effectiveMaxSize, cacheDerivedBucketMaxSize] = internal::getCacheDerivedBucketMaxSize(
        /*storageCacheSize=*/gTimeseriesBucketMaxSize * 10, /*workloadCardinality=*/1);
    ASSERT_EQ(effectiveMaxSize, gTimeseriesBucketMaxSize);
    ASSERT_EQ(cacheDerivedBucketMaxSize, gTimeseriesBucketMaxSize * 5);
}

TEST_F(BucketCatalogTest, GetCacheDerivedBucketMaxSizeRespectsAbsoluteMin) {
    auto [effectiveMaxSize, cacheDerivedBucketMaxSize] = internal::getCacheDerivedBucketMaxSize(
        /*storageCacheSize=*/1, /*workloadCardinality=*/1);
    ASSERT_EQ(effectiveMaxSize, gTimeseriesBucketMinSize.load());
    ASSERT_EQ(cacheDerivedBucketMaxSize, gTimeseriesBucketMinSize.load());
}

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog
