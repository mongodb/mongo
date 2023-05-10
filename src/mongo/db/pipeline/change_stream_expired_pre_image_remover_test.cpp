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

#include "mongo/platform/basic.h"

#include "mongo/db/change_stream_options_manager.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/change_stream_pre_images_truncate_markers.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/change_streams_cluster_parameter_gen.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/oplog_writer_impl.h"
#include "mongo/db/pipeline/change_stream_expired_pre_image_remover.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
std::unique_ptr<ChangeStreamOptions> populateChangeStreamPreImageOptions(
    stdx::variant<std::string, std::int64_t> expireAfterSeconds) {
    PreAndPostImagesOptions preAndPostImagesOptions;
    preAndPostImagesOptions.setExpireAfterSeconds(expireAfterSeconds);

    auto changeStreamOptions = std::make_unique<ChangeStreamOptions>();
    changeStreamOptions->setPreAndPostImages(std::move(preAndPostImagesOptions));

    return changeStreamOptions;
}

void setChangeStreamOptionsToManager(OperationContext* opCtx,
                                     ChangeStreamOptions& changeStreamOptions) {
    auto& changeStreamOptionsManager = ChangeStreamOptionsManager::get(opCtx);
    ASSERT_EQ(changeStreamOptionsManager.setOptions(opCtx, changeStreamOptions).getStatus(),
              ErrorCodes::OK);
}

class ChangeStreamPreImageExpirationPolicyTest : public ServiceContextTest {
public:
    ChangeStreamPreImageExpirationPolicyTest() {
        ChangeStreamOptionsManager::create(getServiceContext());
    }
};

TEST_F(ChangeStreamPreImageExpirationPolicyTest, getPreImageExpirationTimeWithValidIntegralValue) {
    auto opCtx = cc().makeOperationContext();
    const int64_t expireAfterSeconds = 10;

    auto changeStreamOptions = populateChangeStreamPreImageOptions(expireAfterSeconds);
    setChangeStreamOptionsToManager(opCtx.get(), *changeStreamOptions.get());

    auto currentTime = Date_t::now();
    auto receivedExpireAfterSeconds =
        change_stream_pre_image_util::getPreImageExpirationTime(opCtx.get(), currentTime);
    ASSERT(receivedExpireAfterSeconds);
    ASSERT_EQ(*receivedExpireAfterSeconds, currentTime - Seconds(expireAfterSeconds));
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest, getPreImageExpirationTimeWithUnsetValue) {
    auto opCtx = cc().makeOperationContext();

    auto currentTime = Date_t::now();
    auto receivedExpireAfterSeconds =
        change_stream_pre_image_util::getPreImageExpirationTime(opCtx.get(), currentTime);
    ASSERT_FALSE(receivedExpireAfterSeconds);
}

TEST_F(ChangeStreamPreImageExpirationPolicyTest, getPreImageExpirationTimeWithOffValue) {
    auto opCtx = cc().makeOperationContext();

    auto changeStreamOptions = populateChangeStreamPreImageOptions("off");
    setChangeStreamOptionsToManager(opCtx.get(), *changeStreamOptions.get());

    auto currentTime = Date_t::now();
    auto receivedExpireAfterSeconds =
        change_stream_pre_image_util::getPreImageExpirationTime(opCtx.get(), currentTime);
    ASSERT_FALSE(receivedExpireAfterSeconds);
}
}  // namespace

class PreImagesTruncateMarkersPerCollectionTest : public ServiceContextMongoDTest {
protected:
    explicit PreImagesTruncateMarkersPerCollectionTest() : ServiceContextMongoDTest() {
        ChangeStreamOptionsManager::create(getServiceContext());
    }

    virtual void setUp() override {
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        auto opCtx = cc().makeOperationContext();

        // Use the full StorageInterfaceImpl so the earliest oplog entry Timestamp is not the
        // minimum Timestamp.
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());

        // Set up ReplicationCoordinator and create oplog. The earliest oplog entry Timestamp is
        // required for computing whether a truncate marker is expired.
        repl::ReplicationCoordinator::set(
            service, std::make_unique<repl::ReplicationCoordinatorMock>(service));
        repl::createOplog(opCtx.get());

        // Ensure that we are primary.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    }

    void tearDown() override {
        serverGlobalParams.clusterRole = ClusterRole::None;
    }

    void serverlessSetExpireAfterSeconds(const TenantId& tenantId, int64_t expireAfterSeconds) {
        auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
        auto* changeStreamsParam =
            clusterParameters
                ->get<ClusterParameterWithStorage<ChangeStreamsClusterParameterStorage>>(
                    "changeStreams");

        auto oldSettings = changeStreamsParam->getValue(tenantId);
        oldSettings.setExpireAfterSeconds(expireAfterSeconds);
        changeStreamsParam->setValue(oldSettings, tenantId).ignore();
    }

    RecordId generatePreImageRecordId(Timestamp timestamp) {
        const UUID uuid{UUID::gen()};
        ChangeStreamPreImageId preImageId(uuid, timestamp, 0);
        return change_stream_pre_image_util::toRecordId(preImageId);
    }


    RecordId generatePreImageRecordId(Date_t wallTime) {
        const UUID uuid{UUID::gen()};
        Timestamp timestamp{wallTime};
        ChangeStreamPreImageId preImageId(uuid, timestamp, 0);
        return change_stream_pre_image_util::toRecordId(preImageId);
    }

    bool hasExcessMarkers(OperationContext* opCtx, PreImagesTruncateMarkersPerCollection& markers) {
        return markers._hasExcessMarkers(opCtx);
    }

    // The oplog must be populated in order to produce an earliest Timestamp. Creates then
    // performs an insert on an arbitrary collection in order to populate the oplog.
    void initEarliestOplogTSWithInsert(OperationContext* opCtx) {
        NamespaceString arbitraryNss =
            NamespaceString::createNamespaceString_forTest("test", "coll");

        writeConflictRetry(opCtx, "createCollection", arbitraryNss.ns(), [&] {
            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection collRaii(opCtx, arbitraryNss, MODE_X);
            invariant(!collRaii);
            auto db = collRaii.ensureDbExists(opCtx);
            invariant(db->createCollection(opCtx, arbitraryNss, {}));
            wunit.commit();
        });

        std::vector<InsertStatement> insert;
        insert.emplace_back(BSON("_id" << 0 << "data"
                                       << "x"));
        WriteUnitOfWork wuow(opCtx);
        AutoGetCollection autoColl(opCtx, arbitraryNss, MODE_IX);
        OpObserverRegistry opObserver;
        opObserver.addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OplogWriterImpl>()));
        opObserver.onInserts(opCtx,
                             *autoColl,
                             insert.begin(),
                             insert.end(),
                             /*fromMigrate=*/std::vector<bool>(insert.size(), false),
                             /*defaultFromMigrate=*/false);
        wuow.commit();
    }
};

// When 'expireAfterSeconds' is off, defaults to comparing the 'lastRecord's Timestamp of oldest
// marker with the Timestamp of the ealiest oplog entry.
//
// When 'expireAfterSeconds' is on, defaults to comparing the 'lastRecord's wallTime with
// the current time - 'expireAfterSeconds',  which is already tested as a part of the
// ChangeStreamPreImageExpirationPolicyTest.
TEST_F(PreImagesTruncateMarkersPerCollectionTest, hasExcessMarkersExpiredAfterSecondsOff) {
    auto opCtxPtr = cc().makeOperationContext();
    auto opCtx = opCtxPtr.get();

    // With no explicit 'expireAfterSeconds', excess markers are determined by whether the Timestamp
    // of the 'lastRecord' in the oldest marker is greater than the Timestamp of the earliest oplog
    // entry.
    auto changeStreamOptions = populateChangeStreamPreImageOptions("off");
    setChangeStreamOptionsToManager(opCtx, *changeStreamOptions.get());

    initEarliestOplogTSWithInsert(opCtx);
    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);

    // Ensure that the generated Timestamp associated with the lastRecord of the marker is less than
    // the earliest oplog entry Timestamp.
    auto ts = currentEarliestOplogEntryTs - 1;
    ASSERT_GT(currentEarliestOplogEntryTs, ts);
    auto wallTime = Date_t::fromMillisSinceEpoch(ts.asInt64());
    auto lastRecordId = generatePreImageRecordId(wallTime);

    auto numRecords = 1;
    auto numBytes = 100;
    std::deque<CollectionTruncateMarkers::Marker> initialMarkers{
        {numRecords, numBytes, lastRecordId, wallTime}};

    PreImagesTruncateMarkersPerCollection markers(
        boost::none /* tenantId */, std::move(initialMarkers), 0, 0, 100);
    bool excessMarkers = hasExcessMarkers(opCtx, markers);
    ASSERT_TRUE(excessMarkers);
}

TEST_F(PreImagesTruncateMarkersPerCollectionTest, hasNoExcessMarkersExpiredAfterSecondsOff) {
    auto opCtxPtr = cc().makeOperationContext();
    auto opCtx = opCtxPtr.get();

    // With no explicit 'expireAfterSeconds', excess markers are determined by whether the Timestamp
    // of the 'lastRecord' in the oldest marker is greater than the Timestamp of the earliest oplog
    // entry.
    auto changeStreamOptions = populateChangeStreamPreImageOptions("off");
    setChangeStreamOptionsToManager(opCtx, *changeStreamOptions.get());

    initEarliestOplogTSWithInsert(opCtx);
    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);

    // Ensure that the generated Timestamp associated with the lastRecord of the marker is less than
    // the earliest oplog entry Timestamp.
    auto ts = currentEarliestOplogEntryTs + 1;
    ASSERT_LT(currentEarliestOplogEntryTs, ts);
    auto wallTime = Date_t::fromMillisSinceEpoch(ts.asInt64());
    auto lastRecordId = generatePreImageRecordId(wallTime);

    auto numRecords = 1;
    auto numBytes = 100;
    std::deque<CollectionTruncateMarkers::Marker> initialMarkers{
        {numRecords, numBytes, lastRecordId, wallTime}};

    PreImagesTruncateMarkersPerCollection markers(
        boost::none /* tenantId */, std::move(initialMarkers), 0, 0, 100);
    bool excessMarkers = hasExcessMarkers(opCtx, markers);
    ASSERT_FALSE(excessMarkers);
}

TEST_F(PreImagesTruncateMarkersPerCollectionTest, serverlessHasNoExcessMarkers) {
    int64_t expireAfterSeconds = 1000;
    auto tenantId = change_stream_serverless_helpers::getTenantIdForTesting();
    serverlessSetExpireAfterSeconds(tenantId, expireAfterSeconds);

    auto opCtxPtr = cc().makeOperationContext();
    auto opCtx = opCtxPtr.get();
    auto wallTime = opCtx->getServiceContext()->getFastClockSource()->now() + Minutes(120);
    auto lastRecordId = generatePreImageRecordId(wallTime);
    auto numRecords = 1;
    auto numBytes = 100;
    std::deque<CollectionTruncateMarkers::Marker> initialMarkers{
        {numRecords, numBytes, lastRecordId, wallTime}};

    PreImagesTruncateMarkersPerCollection markers(tenantId, std::move(initialMarkers), 0, 0, 100);
    bool excessMarkers = hasExcessMarkers(opCtx, markers);
    ASSERT_FALSE(excessMarkers);
}

TEST_F(PreImagesTruncateMarkersPerCollectionTest, serverlessHasExcessMarkers) {
    int64_t expireAfterSeconds = 1;
    auto tenantId = change_stream_serverless_helpers::getTenantIdForTesting();
    serverlessSetExpireAfterSeconds(tenantId, expireAfterSeconds);

    auto opCtxPtr = cc().makeOperationContext();
    auto opCtx = opCtxPtr.get();
    auto wallTime = opCtx->getServiceContext()->getFastClockSource()->now() - Minutes(120);
    auto lastRecordId = generatePreImageRecordId(wallTime);
    auto numRecords = 1;
    auto numBytes = 100;
    std::deque<CollectionTruncateMarkers::Marker> initialMarkers{
        {numRecords, numBytes, lastRecordId, wallTime}};

    PreImagesTruncateMarkersPerCollection markers(tenantId, std::move(initialMarkers), 0, 0, 100);
    bool excessMarkers = hasExcessMarkers(opCtx, markers);
    ASSERT_TRUE(excessMarkers);
}

TEST_F(PreImagesTruncateMarkersPerCollectionTest, RecordIdToPreImageTimstampRetrieval) {
    // Basic case.
    {
        Timestamp ts0(Date_t::now());
        int64_t applyOpsIndex = 0;

        ChangeStreamPreImageId preImageId(UUID::gen(), ts0, applyOpsIndex);
        auto preImageRecordId = change_stream_pre_image_util::toRecordId(preImageId);

        auto ts1 = change_stream_pre_image_util::getPreImageTimestamp(preImageRecordId);
        ASSERT_EQ(ts0, ts1);
    }

    // Min Timestamp.
    {
        Timestamp ts0 = Timestamp::min();
        int64_t applyOpsIndex = 0;

        ChangeStreamPreImageId preImageId(UUID::gen(), ts0, applyOpsIndex);
        auto preImageRecordId = change_stream_pre_image_util::toRecordId(preImageId);

        auto ts1 = change_stream_pre_image_util::getPreImageTimestamp(preImageRecordId);
        ASSERT_EQ(ts0, ts1);
    }

    // Max Timestamp
    {
        Timestamp ts0 = Timestamp::max();
        int64_t applyOpsIndex = 0;

        ChangeStreamPreImageId preImageId(UUID::gen(), ts0, applyOpsIndex);
        auto preImageRecordId = change_stream_pre_image_util::toRecordId(preImageId);

        auto ts1 = change_stream_pre_image_util::getPreImageTimestamp(preImageRecordId);
        ASSERT_EQ(ts0, ts1);
    }

    // Extra large 'applyOpsIndex'.
    //
    // Parsing a RecordId with an underlying KeyString representation into BSON discards type bits.
    // Since the 'applyOpsIndex' is the only field in 'ChangeStreamPreImageId' that requires type
    // bits to generate the original value from KeyString, ensure different numeric values of
    // 'applyOpsIndex' don't impact the Timestamp retrieval.
    {
        Timestamp ts0(Date_t::now());
        int64_t applyOpsIndex = std::numeric_limits<int64_t>::max();

        ChangeStreamPreImageId preImageId(UUID::gen(), ts0, applyOpsIndex);
        auto preImageRecordId = change_stream_pre_image_util::toRecordId(preImageId);

        auto ts1 = change_stream_pre_image_util::getPreImageTimestamp(preImageRecordId);
        ASSERT_EQ(ts0, ts1);
    }

    // Extra large 'applyOpsIndex' with Timestamp::max().
    {
        Timestamp ts0 = Timestamp::max();
        int64_t applyOpsIndex = std::numeric_limits<int64_t>::max();

        ChangeStreamPreImageId preImageId(UUID::gen(), ts0, applyOpsIndex);
        auto preImageRecordId = change_stream_pre_image_util::toRecordId(preImageId);

        auto ts1 = change_stream_pre_image_util::getPreImageTimestamp(preImageRecordId);
        ASSERT_EQ(ts0, ts1);
    }
}

}  // namespace mongo
