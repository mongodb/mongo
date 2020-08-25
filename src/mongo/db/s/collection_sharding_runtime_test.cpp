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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

const NamespaceString kTestNss("TestDB", "TestColl");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);

using CollectionShardingRuntimeTest = ShardServerTestFixture;

CollectionMetadata makeShardedMetadata(OperationContext* opCtx, UUID uuid = UUID::gen()) {
    const OID epoch = OID::gen();
    auto range = ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY));
    auto chunk = ChunkType(kTestNss, std::move(range), ChunkVersion(1, 0, epoch), ShardId("other"));
    ChunkManager cm(
        ShardId("0"),
        DatabaseVersion(UUID::gen(), 1),
        RoutingTableHistory::makeNew(
            kTestNss, uuid, kShardKeyPattern, nullptr, false, epoch, {std::move(chunk)}),
        boost::none);

    if (!OperationShardingState::isOperationVersioned(opCtx)) {
        const auto version = cm.getVersion(ShardId("0"));
        BSONObjBuilder builder;
        version.appendToCommand(&builder);

        auto& oss = OperationShardingState::get(opCtx);
        oss.initializeClientRoutingVersionsFromCommand(kTestNss, builder.obj());
    }
    return CollectionMetadata(std::move(cm), ShardId("0"));
}

TEST_F(CollectionShardingRuntimeTest,
       GetCollectionDescriptionThrowsStaleConfigBeforeSetFilteringMetadataIsCalledAndNoOSSSet) {
    OperationContext* opCtx = operationContext();
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    ASSERT_FALSE(csr.getCollectionDescription(opCtx).isSharded());
    makeShardedMetadata(opCtx);
    ASSERT_THROWS_CODE(csr.getCollectionDescription(opCtx), DBException, ErrorCodes::StaleConfig);
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCollectionDescriptionReturnsUnshardedAfterSetFilteringMetadataIsCalledWithUnshardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    csr.setFilteringMetadata(operationContext(), CollectionMetadata());
    ASSERT_FALSE(csr.getCollectionDescription(operationContext()).isSharded());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCollectionDescriptionReturnsShardedAfterSetFilteringMetadataIsCalledWithShardedMetadata) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    OperationContext* opCtx = operationContext();
    csr.setFilteringMetadata(opCtx, makeShardedMetadata(opCtx));
    ASSERT_TRUE(csr.getCollectionDescription(opCtx).isSharded());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCurrentMetadataIfKnownReturnsNoneBeforeSetFilteringMetadataIsCalled) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCurrentMetadataIfKnownReturnsUnshardedAfterSetFilteringMetadataIsCalledWithUnshardedMetadata) {

    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    csr.setFilteringMetadata(operationContext(), CollectionMetadata());
    const auto optCurrMetadata = csr.getCurrentMetadataIfKnown();
    ASSERT_TRUE(optCurrMetadata);
    ASSERT_FALSE(optCurrMetadata->isSharded());
    ASSERT_EQ(optCurrMetadata->getShardVersion(), ChunkVersion::UNSHARDED());
}

TEST_F(
    CollectionShardingRuntimeTest,
    GetCurrentMetadataIfKnownReturnsShardedAfterSetFilteringMetadataIsCalledWithShardedMetadata) {

    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    const auto optCurrMetadata = csr.getCurrentMetadataIfKnown();
    ASSERT_TRUE(optCurrMetadata);
    ASSERT_TRUE(optCurrMetadata->isSharded());
    ASSERT_EQ(optCurrMetadata->getShardVersion(), metadata.getShardVersion());
}

TEST_F(CollectionShardingRuntimeTest,
       GetCurrentMetadataIfKnownReturnsNoneAfterClearFilteringMetadataIsCalled) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    OperationContext* opCtx = operationContext();
    csr.setFilteringMetadata(opCtx, makeShardedMetadata(opCtx));
    csr.clearFilteringMetadata(opCtx);
    ASSERT_FALSE(csr.getCurrentMetadataIfKnown());
}

TEST_F(CollectionShardingRuntimeTest, SetFilteringMetadataWithSameUUIDKeepsSameMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    ASSERT_EQ(csr.getNumMetadataManagerChanges_forTest(), 0);
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    // Should create a new MetadataManager object, bumping the count to 1.
    ASSERT_EQ(csr.getNumMetadataManagerChanges_forTest(), 1);
    // Set it again.
    csr.setFilteringMetadata(opCtx, metadata);
    // Should not have reset metadata, so the counter should still be 1.
    ASSERT_EQ(csr.getNumMetadataManagerChanges_forTest(), 1);
}

TEST_F(CollectionShardingRuntimeTest,
       SetFilteringMetadataWithDifferentUUIDReplacesPreviousMetadataManager) {
    CollectionShardingRuntime csr(getServiceContext(), kTestNss, executor());
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, metadata);
    ASSERT_EQ(csr.getNumMetadataManagerChanges_forTest(), 1);

    // Set it again with a different metadata object (UUID is generated randomly in
    // makeShardedMetadata()).
    auto newMetadata = makeShardedMetadata(opCtx);
    csr.setFilteringMetadata(opCtx, newMetadata);

    ASSERT_EQ(csr.getNumMetadataManagerChanges_forTest(), 2);
    ASSERT(
        csr.getCollectionDescription(opCtx).uuidMatches(*newMetadata.getChunkManager()->getUUID()));
}

/**
 * Fixture for when range deletion functionality is required in CollectionShardingRuntime tests.
 */
class CollectionShardingRuntimeWithRangeDeleterTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();
        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());
        // Set up replication coordinator to be primary and have no replication delay.
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext());
        replCoord->setCanAcceptNonLocalWrites(true);
        std::ignore = replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY);
        // Make waitForWriteConcern return immediately.
        replCoord->setAwaitReplicationReturnValueFunction([this](OperationContext* opCtx,
                                                                 const repl::OpTime& opTime) {
            return repl::ReplicationCoordinator::StatusAndDuration(Status::OK(), Milliseconds(0));
        });
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));

        DBDirectClient client(operationContext());
        client.createCollection(kTestNss.ns());
        AutoGetCollection autoColl(operationContext(), kTestNss, MODE_IX);
        _uuid = autoColl.getCollection()->uuid();
    }

    void tearDown() override {
        DBDirectClient client(operationContext());
        client.dropCollection(kTestNss.ns());

        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardServerTestFixture::tearDown();
    }

    CollectionShardingRuntime& csr() {
        // Creates the CSR if it does not exist and stashes it in the CollectionShardingStateMap.
        // This is required for waitForClean tests which use CollectionShardingRuntime::get().
        return *CollectionShardingRuntime::get_UNSAFE(getServiceContext(), kTestNss);
    }

    UUID uuid() {
        return _uuid;
    }

private:
    UUID _uuid{UUID::gen()};
};


TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsErrorIfMetadataManagerDoesNotExist) {
    auto status = CollectionShardingRuntime::waitForClean(
        operationContext(),
        kTestNss,
        uuid(),
        ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)));
    ASSERT_EQ(status.code(), ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsErrorIfCollectionUUIDDoesNotMatchFilteringMetadata) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr().setFilteringMetadata(opCtx, metadata);
    auto randomUuid = UUID::gen();

    auto status = CollectionShardingRuntime::waitForClean(
        opCtx,
        kTestNss,
        randomUuid,
        ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)));
    ASSERT_EQ(status.code(), ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsOKIfNoDeletionsAreScheduled) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr().setFilteringMetadata(opCtx, metadata);

    auto status = CollectionShardingRuntime::waitForClean(
        opCtx, kTestNss, uuid(), ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)));

    ASSERT_OK(status);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanBlocksBehindOneScheduledDeletion) {
    // Enable fail point to suspendRangeDeletion.
    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::alwaysOn);
    OperationContext* opCtx = operationContext();

    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr().setFilteringMetadata(opCtx, metadata);

    auto cleanupComplete =
        csr().cleanUpRange(ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
                           boost::none,
                           CollectionShardingRuntime::CleanWhen::kNow);

    opCtx->setDeadlineAfterNowBy(Milliseconds(100), ErrorCodes::MaxTimeMSExpired);
    auto status = CollectionShardingRuntime::waitForClean(
        opCtx, kTestNss, uuid(), ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)));

    ASSERT_EQ(status.code(), ErrorCodes::MaxTimeMSExpired);

    globalFailPointRegistry().find("suspendRangeDeletion")->setMode(FailPoint::off);
    cleanupComplete.get();
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanBlocksBehindAllScheduledDeletions) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr().setFilteringMetadata(opCtx, metadata);

    const auto middleKey = 5;

    auto cleanupCompleteFirst =
        csr().cleanUpRange(ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << middleKey)),
                           boost::none,
                           CollectionShardingRuntime::CleanWhen::kNow);

    auto cleanupCompleteSecond =
        csr().cleanUpRange(ChunkRange(BSON(kShardKey << middleKey), BSON(kShardKey << MAXKEY)),
                           boost::none,
                           CollectionShardingRuntime::CleanWhen::kNow);

    auto status = CollectionShardingRuntime::waitForClean(
        opCtx, kTestNss, uuid(), ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)));

    // waitForClean should block until both cleanup tasks have run. This is a best-effort check,
    // since even if it did not block, it is possible that the cleanup tasks could complete before
    // reaching these lines.
    ASSERT(cleanupCompleteFirst.isReady());
    ASSERT(cleanupCompleteSecond.isReady());

    ASSERT_OK(status);
}

TEST_F(CollectionShardingRuntimeWithRangeDeleterTest,
       WaitForCleanReturnsOKAfterSuccessfulDeletion) {
    OperationContext* opCtx = operationContext();
    auto metadata = makeShardedMetadata(opCtx, uuid());
    csr().setFilteringMetadata(opCtx, metadata);

    auto cleanupComplete =
        csr().cleanUpRange(ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
                           boost::none,
                           CollectionShardingRuntime::CleanWhen::kNow);

    auto status = CollectionShardingRuntime::waitForClean(
        opCtx, kTestNss, uuid(), ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)));

    ASSERT_OK(status);
    ASSERT(cleanupComplete.isReady());
}

}  // namespace

}  // namespace mongo
