/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/move_primary/move_primary_recipient_cmds_gen.h"
#include "mongo/db/s/move_primary/move_primary_recipient_service.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_mongod_test_fixture.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include <memory>

namespace mongo {

namespace {
const std::vector<ShardType> kShardList = {ShardType("shard0", "host0"),
                                           ShardType("shard1", "host1")};

static HostAndPort makeHostAndPort(const ShardId& shardId) {
    return HostAndPort(str::stream() << shardId << ":123");
}
}  // namespace

class MovePrimaryRecipientExternalStateForTest : public MovePrimaryRecipientExternalState {

    std::vector<AsyncRequestsSender::Response> sendCommandToShards(
        OperationContext* opCtx,
        StringData dbName,
        const BSONObj& command,
        const std::vector<ShardId>& shardIds,
        const std::shared_ptr<executor::TaskExecutor>& executor) {
        executor::RemoteCommandResponse kOkResponse{repl::OpTimeBase(Timestamp::min()).toBSON(),
                                                    Milliseconds(0)};
        std::vector<AsyncRequestsSender::Response> shardResponses{
            {kShardList.front().getName(),
             kOkResponse,
             makeHostAndPort(kShardList.front().getName())}};
        return shardResponses;
    };
};

class MovePrimaryRecipientServiceForTest : public MovePrimaryRecipientService {
public:
    explicit MovePrimaryRecipientServiceForTest(ServiceContext* serviceContext)
        : MovePrimaryRecipientService(serviceContext) {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialStateDoc) override {
        auto recipientStateDoc = MovePrimaryRecipientDocument::parse(
            IDLParserContext("MovePrimaryRecipientService::constructInstance"),
            std::move(initialStateDoc));

        return std::make_shared<MovePrimaryRecipientService::MovePrimaryRecipient>(
            this,
            recipientStateDoc,
            std::make_shared<MovePrimaryRecipientExternalStateForTest>(),
            _serviceContext);
    }
};

class MovePrimaryRecipientServiceTest : public ShardServerTestFixture {

    void setUp() override {
        ShardServerTestFixture::setUp();

        auto serviceContext = getServiceContext();
        WaitForMajorityService::get(serviceContext).startup(serviceContext);

        auto opCtx = operationContext();

        for (const auto& shardType : kShardList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(opCtx, shardType.getName()))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardType.getName()));
        }

        _opObserverRegistry = checked_cast<OpObserverRegistry*>(serviceContext->getOpObserver());

        invariant(_opObserverRegistry);

        _opObserverRegistry->addObserver(
            std::make_unique<repl::PrimaryOnlyServiceOpObserver>(serviceContext));

        _registry = repl::PrimaryOnlyServiceRegistry::get(serviceContext);
        auto service = std::make_unique<MovePrimaryRecipientServiceForTest>(serviceContext);
        auto serviceName = service->getServiceName();
        _registry->registerService(std::move(service));

        _service = _registry->lookupServiceByName(serviceName);

        _registry->onStartup(opCtx);
        stepUpPOS();
    }

    void stepUpPOS() {
        auto opCtx = operationContext();
        auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());
        WriteUnitOfWork wuow{operationContext()};
        auto newOpTime = repl::getNextOpTime(operationContext());
        wuow.commit();
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        replCoord->setMyLastAppliedOpTimeAndWallTime({newOpTime, {}});

        _registry->onStepUpComplete(opCtx, _term);
    }

    void tearDown() override {
        globalFailPointRegistry().disableAllFailpoints();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        _registry->onShutdown();

        ShardServerTestFixture::tearDown();
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardType> shards) : _shards(std::move(shards)) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
                return repl::OpTimeWith<std::vector<ShardType>>(_shards);
            }

            std::vector<CollectionType> getCollections(OperationContext* opCtx,
                                                       StringData dbName,
                                                       repl::ReadConcernLevel readConcernLevel,
                                                       const BSONObj& sort) override {
                return _colls;
            }

            std::vector<NamespaceString> getAllShardedCollectionsForDb(
                OperationContext* opCtx,
                StringData dbName,
                repl::ReadConcernLevel readConcern,
                const BSONObj& sort = BSONObj()) override {
                return _shardedColls;
            }

            void setCollections(std::vector<CollectionType> colls) {
                _colls = std::move(colls);
            }

        private:
            const std::vector<ShardType> _shards;
            std::vector<CollectionType> _colls;
            std::vector<NamespaceString> _shardedColls;
        };

        return std::make_unique<StaticCatalogClient>(kShardList);
    }

protected:
    MovePrimaryRecipientDocument createRecipientDoc() {
        UUID migrationId = UUID::gen();
        MovePrimaryRecipientDocument doc(migrationId);

        MovePrimaryRecipientMetadata metadata(migrationId, "foo", kShardList.front().getName());
        doc.setMovePrimaryRecipientMetadata(metadata);

        return doc;
    }

    MovePrimaryRecipientDocument getRecipientDoc(OperationContext* opCtx) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kMovePrimaryRecipientNamespace, BSONObj{});
        return MovePrimaryRecipientDocument::parse(
            IDLParserContext("MovePrimaryRecipientServiceTest::getRecipientDoc"), doc);
    }

    OpObserverRegistry* _opObserverRegistry = nullptr;
    repl::PrimaryOnlyServiceRegistry* _registry = nullptr;
    repl::PrimaryOnlyService* _service = nullptr;
    long long _term = 0;
};

TEST_F(MovePrimaryRecipientServiceTest, MovePrimaryRecipientInstanceCreation) {
    auto doc = createRecipientDoc();
    auto opCtx = operationContext();

    auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());

    ASSERT(instance.get());
}

TEST_F(MovePrimaryRecipientServiceTest, PersistsStateDocument) {
    auto doc = createRecipientDoc();

    auto movePrimaryRecipientPauseAfterInsertingStateDoc =
        globalFailPointRegistry().find("movePrimaryRecipientPauseAfterInsertingStateDoc");
    auto timesEnteredFailPoint =
        movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::alwaysOn, 0);

    auto opCtx = operationContext();
    auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());

    movePrimaryRecipientPauseAfterInsertingStateDoc->waitForTimesEntered(timesEnteredFailPoint + 1);

    ASSERT(instance.get());
    ASSERT_EQ(doc.getMigrationId(), instance->getMigrationId());
    ASSERT(instance->getRecipientDocDurableFuture().isReady());

    auto persistedDoc = getRecipientDoc(opCtx);
    ASSERT_BSONOBJ_EQ(persistedDoc.getMovePrimaryRecipientMetadata().toBSON(),
                      doc.getMovePrimaryRecipientMetadata().toBSON());

    movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::off, 0);
}

TEST_F(MovePrimaryRecipientServiceTest, ThrowsWithConflictingOperation) {
    auto doc = createRecipientDoc();

    auto movePrimaryRecipientPauseAfterInsertingStateDoc =
        globalFailPointRegistry().find("movePrimaryRecipientPauseAfterInsertingStateDoc");
    auto timesEnteredFailPoint =
        movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::alwaysOn, 0);

    auto opCtx = operationContext();
    auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());

    movePrimaryRecipientPauseAfterInsertingStateDoc->waitForTimesEntered(timesEnteredFailPoint + 1);

    auto conflictingDoc = createRecipientDoc();

    ASSERT_NE(doc.getId(), conflictingDoc.getId());

    // Asserts that a movePrimary op on same database fails with MovePrimaryInProgress
    ASSERT_THROWS_CODE(MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
                           opCtx, _service, conflictingDoc.toBSON()),
                       DBException,
                       ErrorCodes::MovePrimaryInProgress);

    movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::off, 0);
}


TEST_F(MovePrimaryRecipientServiceTest, ThrowsWithConflictingOptions) {
    auto doc = createRecipientDoc();

    auto movePrimaryRecipientPauseAfterInsertingStateDoc =
        globalFailPointRegistry().find("movePrimaryRecipientPauseAfterInsertingStateDoc");
    auto timesEnteredFailPoint =
        movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::alwaysOn, 0);

    auto opCtx = operationContext();
    auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());

    movePrimaryRecipientPauseAfterInsertingStateDoc->waitForTimesEntered(timesEnteredFailPoint + 1);

    MovePrimaryRecipientDocument conflictingDoc(doc.getMigrationId());
    MovePrimaryRecipientMetadata metadata(doc.getMigrationId(), "bar", "second/localhost:27018");
    conflictingDoc.setMovePrimaryRecipientMetadata(metadata);

    // Asserts that a movePrimary op with a different fromShard fails with MovePrimaryInProgress
    ASSERT_THROWS_CODE(MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
                           opCtx, _service, conflictingDoc.toBSON()),
                       DBException,
                       ErrorCodes::MovePrimaryInProgress);

    // Asserts that a movePrimary op with a different databaseName fails with MovePrimaryInProgress
    ASSERT_THROWS_CODE(MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
                           opCtx, _service, conflictingDoc.toBSON()),
                       DBException,
                       ErrorCodes::MovePrimaryInProgress);

    movePrimaryRecipientPauseAfterInsertingStateDoc->setMode(FailPoint::off, 0);
}

TEST_F(MovePrimaryRecipientServiceTest, CanTransitionTokCloningState) {
    auto doc = createRecipientDoc();

    auto movePrimaryRecipientPauseAfterTransitionToCloningState =
        globalFailPointRegistry().find("movePrimaryRecipientPauseAfterTransitionToCloningState");
    auto timesEnteredFailPoint =
        movePrimaryRecipientPauseAfterTransitionToCloningState->setMode(FailPoint::alwaysOn, 0);

    auto opCtx = operationContext();
    auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());

    movePrimaryRecipientPauseAfterTransitionToCloningState->waitForTimesEntered(
        timesEnteredFailPoint + 1);

    ASSERT(instance->getRecipientDocDurableFuture().isReady());
    ASSERT(instance->getRecipientDataClonedFuture().isReady());

    auto persistedDoc = getRecipientDoc(opCtx);
    ASSERT_EQ(persistedDoc.getState(), MovePrimaryRecipientState::kCloning);
    ASSERT(persistedDoc.getStartApplyingDonorOpTime().is_initialized());

    movePrimaryRecipientPauseAfterTransitionToCloningState->setMode(FailPoint::off, 0);
}

}  // namespace mongo
