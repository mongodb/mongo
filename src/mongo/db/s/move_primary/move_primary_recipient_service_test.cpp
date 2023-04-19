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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/move_primary/move_primary_recipient_cmds_gen.h"
#include "mongo/db/s/move_primary/move_primary_recipient_service.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"
#include "mongo/db/s/resharding/resharding_service_test_helpers.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_mongod_test_fixture.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

namespace {

using MovePrimaryRecipientStateTransitionController =
    resharding_service_test_helpers::StateTransitionController<MovePrimaryRecipientStateEnum>;
using OpObserverForTest = resharding_service_test_helpers::StateTransitionControllerOpObserver<
    MovePrimaryRecipientStateEnum,
    MovePrimaryRecipientDocument>;
using PauseDuringStateTransition =
    resharding_service_test_helpers::PauseDuringStateTransitions<MovePrimaryRecipientStateEnum>;

const std::vector<ShardType> kShardList = {ShardType("shard0", "host0"),
                                           ShardType("shard1", "host1")};

static HostAndPort makeHostAndPort(const ShardId& shardId) {
    return HostAndPort(str::stream() << shardId << ":123");
}
}  // namespace

class TestClonerImpl : public ClonerImpl {
    Status copyDb(OperationContext* opCtx,
                  const std::string& dBName,
                  const std::string& masterHost,
                  const std::vector<NamespaceString>& shardedColls,
                  std::set<std::string>* clonedColls) override {
        return Status::OK();
    }

    Status setupConn(OperationContext* opCtx,
                     const std::string& dBName,
                     const std::string& masterHost) override {
        return Status::OK();
    }

    StatusWith<std::vector<BSONObj>> getListOfCollections(OperationContext* opCtx,
                                                          const std::string& dBName,
                                                          const std::string& masterHost) override {
        std::vector<BSONObj> colls;
        return colls;
    }
};

class MovePrimaryRecipientExternalStateForTest : public MovePrimaryRecipientExternalState {

    std::vector<AsyncRequestsSender::Response> sendCommandToShards(
        OperationContext* opCtx,
        StringData dbName,
        const BSONObj& command,
        const std::vector<ShardId>& shardIds,
        const std::shared_ptr<executor::TaskExecutor>& executor) {
        auto opTimeBase = repl::OpTimeBase(Timestamp::min());
        opTimeBase.setTerm(0);
        BSONArrayBuilder bab;
        bab.append(opTimeBase.toBSON());
        executor::RemoteCommandResponse kOkResponse{
            BSON("ok" << 1 << "cursor" << BSON("firstBatch" << bab.done())), Microseconds(0)};
        std::vector<AsyncRequestsSender::Response> shardResponses{
            {kShardList.front().getName(),
             kOkResponse,
             makeHostAndPort(kShardList.front().getName())}};
        return shardResponses;
    };
};

class MovePrimaryRecipientServiceForTest : public MovePrimaryRecipientService {
public:
    static constexpr StringData kServiceName = "MovePrimaryRecipientServiceForTest"_sd;

    explicit MovePrimaryRecipientServiceForTest(ServiceContext* serviceContext)
        : MovePrimaryRecipientService(serviceContext) {}

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialStateDoc) override {
        auto recipientStateDoc = MovePrimaryRecipientDocument::parse(
            IDLParserContext("MovePrimaryRecipientServiceForTest::constructInstance"),
            std::move(initialStateDoc));

        return std::make_shared<MovePrimaryRecipientService::MovePrimaryRecipient>(
            this,
            recipientStateDoc,
            std::make_shared<MovePrimaryRecipientExternalStateForTest>(),
            _serviceContext,
            std::make_unique<Cloner>(std::make_unique<TestClonerImpl>()));
    }

    StringData getServiceName() const {
        return kServiceName;
    }
};

class MovePrimaryRecipientServiceTest : public ShardServerTestFixture {

    void setUp() override {
        ShardServerTestFixture::setUp();

        _serviceCtx = getServiceContext();
        WaitForMajorityService::get(_serviceCtx).startup(_serviceCtx);

        auto opCtx = operationContext();

        for (const auto& shardType : kShardList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(opCtx, shardType.getName()))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardType.getName()));
        }

        _controller = std::make_shared<MovePrimaryRecipientStateTransitionController>();

        _opObserverRegistry = checked_cast<OpObserverRegistry*>(_serviceCtx->getOpObserver());

        invariant(_opObserverRegistry);

        _opObserverRegistry->addObserver(
            std::make_unique<repl::PrimaryOnlyServiceOpObserver>(_serviceCtx));

        _opObserverRegistry->addObserver(std::make_unique<OpObserverForTest>(
            _controller,
            NamespaceString::kMovePrimaryRecipientNamespace,
            [](const MovePrimaryRecipientDocument& stateDoc) { return stateDoc.getState(); }));

        _registry = repl::PrimaryOnlyServiceRegistry::get(_serviceCtx);
        auto service = std::make_unique<MovePrimaryRecipientServiceForTest>(_serviceCtx);

        auto serviceName = service->getServiceName();
        _registry->registerService(std::move(service));

        _service = _registry->lookupServiceByName(serviceName);


        _registry->onStartup(opCtx);
        _stepUpPOS();
    }

    void tearDown() override {
        globalFailPointRegistry().disableAllFailpoints();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        _registry->onShutdown();

        ShardServerTestFixture::tearDown();
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        auto client = std::make_unique<StaticCatalogClient>(kShardList);
        return client;
    }

protected:
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

    MovePrimaryRecipientDocument createRecipientDoc() {
        UUID migrationId = UUID::gen();
        MovePrimaryCommonMetadata metadata(migrationId,
                                           NamespaceString{"foo"},
                                           kShardList.front().getName(),
                                           kShardList.back().getName());

        MovePrimaryRecipientDocument doc;
        doc.setMetadata(metadata);
        doc.setId(migrationId);

        return doc;
    }

    MovePrimaryRecipientDocument getRecipientDoc(OperationContext* opCtx, UUID migrationId) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kMovePrimaryRecipientNamespace,
                                  BSON("_id" << migrationId));
        ASSERT_FALSE(doc.isEmpty());
        return MovePrimaryRecipientDocument::parse(
            IDLParserContext("MovePrimaryRecipientServiceTest::getRecipientDoc"), doc);
    }

    std::shared_ptr<MovePrimaryRecipientService::MovePrimaryRecipient> lookupRecipient(
        OperationContext* opCtx, repl::PrimaryOnlyService::InstanceID instanceId) {
        auto recipientOpt =
            MovePrimaryRecipientService::MovePrimaryRecipient::lookup(opCtx, _service, instanceId);
        return recipientOpt ? recipientOpt.get() : nullptr;
    }

    void waitUntilDocDeleted(OperationContext* opCtx, NamespaceString nss, UUID migrationId) {
        DBDirectClient client(opCtx);
        int cnt = 1000;
        while (cnt--) {
            DBDirectClient client(opCtx);
            auto recipientDoc = client.findOne(nss, BSON("_id" << migrationId));
            if (recipientDoc.isEmpty()) {
                return;
            }

            sleepmillis(60);
        }
        FAIL(str::stream() << "Timed out waiting for delete of doc with migrationId: "
                           << migrationId);
    }

    void _stepUpPOS() {
        auto opCtx = operationContext();
        auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());
        WriteUnitOfWork wuow{operationContext()};
        auto newOpTime = repl::getNextOpTime(operationContext());
        wuow.commit();
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        replCoord->setMyLastAppliedOpTimeAndWallTime({newOpTime, {}});

        _registry->onStepUpComplete(opCtx, _term);
    }

    OpObserverRegistry* _opObserverRegistry = nullptr;
    repl::PrimaryOnlyServiceRegistry* _registry = nullptr;
    repl::PrimaryOnlyService* _service = nullptr;
    std::shared_ptr<MovePrimaryRecipientStateTransitionController> _controller;
    ServiceContext* _serviceCtx = nullptr;
    long long _term = 0;
};

TEST_F(MovePrimaryRecipientServiceTest, CanRunToCompletion) {
    auto doc = createRecipientDoc();
    auto opCtx = operationContext();

    auto recipient = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());
    auto future = recipient->onReceiveForgetMigration();
    future.get(opCtx);
}

TEST_F(MovePrimaryRecipientServiceTest, TransitionsThroughEachStateInRunToCompletion) {
    const std::vector<MovePrimaryRecipientStateEnum> recipientStates{
        MovePrimaryRecipientStateEnum::kInitializing,
        MovePrimaryRecipientStateEnum::kCloning,
        MovePrimaryRecipientStateEnum::kApplying,
        MovePrimaryRecipientStateEnum::kBlocking,
        MovePrimaryRecipientStateEnum::kPrepared,
        MovePrimaryRecipientStateEnum::kDone};

    const std::vector<std::pair<MovePrimaryRecipientStateEnum, std::string>> stateFPNames{
        {MovePrimaryRecipientStateEnum::kInitializing,
         "movePrimaryRecipientPauseAfterInsertingStateDoc"},
        {MovePrimaryRecipientStateEnum::kCloning, "movePrimaryRecipientPauseAfterCloningState"},
        {MovePrimaryRecipientStateEnum::kApplying, "movePrimaryRecipientPauseAfterApplyingState"},
        {MovePrimaryRecipientStateEnum::kBlocking, "movePrimaryRecipientPauseAfterBlockingState"},
        {MovePrimaryRecipientStateEnum::kPrepared, "movePrimaryRecipientPauseAfterPreparedState"},
        {MovePrimaryRecipientStateEnum::kDone, "movePrimaryRecipientPauseBeforeDeletingStateDoc"}};

    std::map<MovePrimaryRecipientStateEnum, std::pair<FailPoint*, int>> stateFailPointMap;

    PauseDuringStateTransition guard(_controller.get(), recipientStates);

    for (const auto& stateFPName : stateFPNames) {
        auto state = stateFPName.first;
        auto fp = globalFailPointRegistry().find(stateFPName.second);
        auto cnt = fp->setMode(FailPoint::alwaysOn, 0);
        stateFailPointMap[state] = {fp, cnt};
    }

    auto movePrimaryRecipientPauseBeforeCompletion =
        globalFailPointRegistry().find("movePrimaryRecipientPauseBeforeCompletion");
    auto timesEnteredPauseBeforeCompletionFailPoint =
        movePrimaryRecipientPauseBeforeCompletion->setMode(FailPoint::alwaysOn, 0);

    auto doc = createRecipientDoc();
    auto opCtx = operationContext();

    auto recipient = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());

    for (const auto& stateFPName : stateFPNames) {
        auto state = stateFPName.first;
        guard.wait(state);
        guard.unset(state);
        auto fp = stateFailPointMap[state].first;
        auto cnt = stateFailPointMap[state].second;
        fp->waitForTimesEntered(cnt + 1);
        fp->setMode(FailPoint::off, 0);

        if (state == MovePrimaryRecipientStateEnum::kPrepared) {
            (void)recipient->onReceiveForgetMigration();
        }
    }

    movePrimaryRecipientPauseBeforeCompletion->waitForTimesEntered(
        timesEnteredPauseBeforeCompletionFailPoint + 1);

    recipient->getCompletionFuture().get();
    movePrimaryRecipientPauseBeforeCompletion->setMode(FailPoint::off, 0);
    waitUntilDocDeleted(
        opCtx, NamespaceString::kMovePrimaryRecipientNamespace, doc.getMigrationId());
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

    auto persistedDoc = getRecipientDoc(opCtx, doc.getMigrationId());
    ASSERT_BSONOBJ_EQ(persistedDoc.getMetadata().toBSON(), doc.getMetadata().toBSON());

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

    ASSERT_NE(doc.getMigrationId(), conflictingDoc.getMigrationId());

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

    MovePrimaryCommonMetadata metadata(doc.getMigrationId(),
                                       NamespaceString{"bar"},
                                       "second/localhost:27018",
                                       "first/localhost:27019");
    MovePrimaryRecipientDocument conflictingDoc;
    conflictingDoc.setId(doc.getMigrationId());
    conflictingDoc.setMetadata(metadata);

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

TEST_F(MovePrimaryRecipientServiceTest, CanAbortBeforePersistingStateDoc) {
    auto doc = createRecipientDoc();

    auto movePrimaryRecipientPauseBeforeRunning =
        globalFailPointRegistry().find("movePrimaryRecipientPauseBeforeRunning");
    auto timesEnteredPauseBeforeRunningFailPoint =
        movePrimaryRecipientPauseBeforeRunning->setMode(FailPoint::alwaysOn, 0);

    auto movePrimaryRecipientPauseBeforeCompletion =
        globalFailPointRegistry().find("movePrimaryRecipientPauseBeforeCompletion");
    auto timesEnteredPauseBeforeCompletionFailPoint =
        movePrimaryRecipientPauseBeforeCompletion->setMode(FailPoint::alwaysOn, 0);

    auto opCtx = operationContext();
    auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());

    movePrimaryRecipientPauseBeforeRunning->waitForTimesEntered(
        timesEnteredPauseBeforeRunningFailPoint + 1);

    instance->abort();
    movePrimaryRecipientPauseBeforeRunning->setMode(FailPoint::off, 0);

    movePrimaryRecipientPauseBeforeCompletion->waitForTimesEntered(
        timesEnteredPauseBeforeCompletionFailPoint + 1);

    ASSERT(instance->getCompletionFuture().isReady());
    ASSERT(!instance->getCompletionFuture().getNoThrow().isOK());
}

TEST_F(MovePrimaryRecipientServiceTest, FulfillsDataClonedFutureAfterCloning) {
    auto movePrimaryRecipientPauseAfterCloningState =
        globalFailPointRegistry().find("movePrimaryRecipientPauseAfterCloningState");
    auto timesEnteredPauseAfterCloningStateFailPoint =
        movePrimaryRecipientPauseAfterCloningState->setMode(FailPoint::alwaysOn, 0);

    auto doc = createRecipientDoc();
    auto opCtx = operationContext();

    auto recipient = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());

    movePrimaryRecipientPauseAfterCloningState->waitForTimesEntered(
        timesEnteredPauseAfterCloningStateFailPoint + 1);

    auto dataClonedFuture = recipient->getDataClonedFuture();
    ASSERT_TRUE(dataClonedFuture.getNoThrow(opCtx).isOK());

    movePrimaryRecipientPauseAfterCloningState->setMode(FailPoint::off, 0);

    ASSERT_TRUE(recipient->onReceiveForgetMigration().getNoThrow(opCtx).isOK());
}

TEST_F(MovePrimaryRecipientServiceTest, CanAbortInEachAbortableState) {
    // Tests that the movePrimary op aborts in the states below when asked to abort.
    const std::vector<std::pair<MovePrimaryRecipientStateEnum, std::string>> stateFPNames{
        {MovePrimaryRecipientStateEnum::kInitializing,
         "movePrimaryRecipientPauseAfterInsertingStateDoc"},
        {MovePrimaryRecipientStateEnum::kCloning, "movePrimaryRecipientPauseAfterCloningState"},
        {MovePrimaryRecipientStateEnum::kApplying, "movePrimaryRecipientPauseAfterApplyingState"},
        {MovePrimaryRecipientStateEnum::kBlocking, "movePrimaryRecipientPauseAfterBlockingState"},
        {MovePrimaryRecipientStateEnum::kPrepared, "movePrimaryRecipientPauseAfterPreparedState"}};

    for (const auto& stateFPNamePair : stateFPNames) {
        PauseDuringStateTransition stateTransitionsGuard{_controller.get(),
                                                         MovePrimaryRecipientStateEnum::kAborted};
        const auto& state = stateFPNamePair.first;
        auto fp = globalFailPointRegistry().find(stateFPNamePair.second);

        auto cnt = fp->setMode(FailPoint::alwaysOn, 0);

        auto doc = createRecipientDoc();

        auto opCtx = operationContext();

        auto movePrimaryRecipientPauseBeforeCompletion =
            globalFailPointRegistry().find("movePrimaryRecipientPauseBeforeCompletion");
        auto timesEnteredPauseBeforeCompletionFailPoint =
            movePrimaryRecipientPauseBeforeCompletion->setMode(FailPoint::alwaysOn, 0);

        auto recipient = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
            opCtx, _service, doc.toBSON());

        auto instanceId = BSON("_id" << recipient->getMigrationId());

        ASSERT_FALSE(recipient->getCompletionFuture().isReady());

        LOGV2(7306904, "Running CanAbortInEachAbortableState", "state"_attr = state);

        fp->waitForTimesEntered(cnt + 1);

        recipient->abort();

        fp->setMode(FailPoint::off, 0);

        stateTransitionsGuard.wait(MovePrimaryRecipientStateEnum::kAborted);
        stateTransitionsGuard.unset(MovePrimaryRecipientStateEnum::kAborted);

        movePrimaryRecipientPauseBeforeCompletion->waitForTimesEntered(
            timesEnteredPauseBeforeCompletionFailPoint + 1);

        ASSERT_THROWS_CODE(recipient->getCompletionFuture().get(opCtx),
                           DBException,
                           ErrorCodes::MovePrimaryAborted);
        movePrimaryRecipientPauseBeforeCompletion->setMode(FailPoint::off, 0);

        recipient.reset();

        waitUntilDocDeleted(
            opCtx, NamespaceString::kMovePrimaryRecipientNamespace, doc.getMigrationId());

        _service->releaseInstance(instanceId, Status::OK());

        LOGV2(7306905, "Finished running CanAbortInEachAbortableState", "state"_attr = state);
    }
}

TEST_F(MovePrimaryRecipientServiceTest, StepUpStepDownEachPersistedStateLifecycleFlagEnabled) {
    // Tests that the movePrimary op aborts on stepdown-stepup for first four states and goes to
    // completion for the rest of the states below.
    const std::vector<std::pair<MovePrimaryRecipientStateEnum, std::string>> stateFPNames{
        {MovePrimaryRecipientStateEnum::kInitializing,
         "movePrimaryRecipientPauseAfterInsertingStateDoc"},
        {MovePrimaryRecipientStateEnum::kCloning, "movePrimaryRecipientPauseAfterCloningState"},
        {MovePrimaryRecipientStateEnum::kApplying, "movePrimaryRecipientPauseAfterApplyingState"},
        {MovePrimaryRecipientStateEnum::kBlocking, "movePrimaryRecipientPauseAfterBlockingState"},
        {MovePrimaryRecipientStateEnum::kPrepared, "movePrimaryRecipientPauseAfterPreparedState"},
        {MovePrimaryRecipientStateEnum::kDone, "movePrimaryRecipientPauseBeforeDeletingStateDoc"}};

    for (const auto& stateFPNamePair : stateFPNames) {
        PauseDuringStateTransition stateTransitionsGuard{_controller.get(),
                                                         MovePrimaryRecipientStateEnum::kAborted};
        const auto& state = stateFPNamePair.first;
        auto fp = globalFailPointRegistry().find(stateFPNamePair.second);

        auto doc = createRecipientDoc();

        auto opCtx = operationContext();

        auto movePrimaryRecipientPauseBeforeCompletion =
            globalFailPointRegistry().find("movePrimaryRecipientPauseBeforeCompletion");
        auto timesEnteredPauseBeforeCompletionFailPoint =
            movePrimaryRecipientPauseBeforeCompletion->setMode(FailPoint::alwaysOn, 0);

        auto cnt = fp->setMode(FailPoint::alwaysOn, 0);

        auto recipient = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
            opCtx, _service, doc.toBSON());

        auto instanceId = BSON("_id" << recipient->getMigrationId());

        ASSERT_FALSE(recipient->getCompletionFuture().isReady());

        LOGV2(7306906,
              "Running StepUpStepDownEachPersistedStateLifecycleFlagEnabled",
              "stepdown"_attr = state);

        if (state == MovePrimaryRecipientStateEnum::kDone) {
            (void)recipient->onReceiveForgetMigration();
        }

        fp->waitForTimesEntered(cnt + 1);

        stepDown(_serviceCtx, _registry);

        fp->setMode(FailPoint::off, 0);

        movePrimaryRecipientPauseBeforeCompletion->waitForTimesEntered(
            timesEnteredPauseBeforeCompletionFailPoint + 1);
        ASSERT_EQ(recipient->getCompletionFuture().getNoThrow(), ErrorCodes::CallbackCanceled);
        movePrimaryRecipientPauseBeforeCompletion->setMode(FailPoint::off, 0);

        stepUp(opCtx, _serviceCtx, _registry, _term);

        recipient = lookupRecipient(opCtx, instanceId);

        if (state < MovePrimaryRecipientStateEnum::kPrepared) {
            stateTransitionsGuard.wait(MovePrimaryRecipientStateEnum::kAborted);
            stateTransitionsGuard.unset(MovePrimaryRecipientStateEnum::kAborted);
            ASSERT_THROWS_CODE(recipient->getCompletionFuture().get(opCtx),
                               DBException,
                               ErrorCodes::MovePrimaryAborted);
        } else {
            stateTransitionsGuard.unset(MovePrimaryRecipientStateEnum::kAborted);
            (void)recipient->onReceiveForgetMigration();
            recipient->getCompletionFuture().get(opCtx);
        }

        recipient.reset();

        waitUntilDocDeleted(
            opCtx, NamespaceString::kMovePrimaryRecipientNamespace, doc.getMigrationId());

        _service->releaseInstance(instanceId, Status::OK());

        LOGV2(7306907,
              "Finished running StepUpStepDownEachPersistedStateLifecycleFlagEnabled",
              "stepdown"_attr = state);
    }
}

TEST_F(MovePrimaryRecipientServiceTest, CleansUpPersistedMetadataOnCompletion) {
    auto doc = createRecipientDoc();
    auto opCtx = operationContext();

    auto recipient = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());
    auto future = recipient->onReceiveForgetMigration();
    future.get(opCtx);
    waitUntilDocDeleted(
        opCtx, NamespaceString::kMovePrimaryRecipientNamespace, doc.getMigrationId());
    waitUntilDocDeleted(
        opCtx, NamespaceString::kMovePrimaryApplierProgressNamespace, doc.getMigrationId());
    waitUntilDocDeleted(opCtx,
                        NamespaceString::makeMovePrimaryOplogBufferNSS(doc.getMigrationId()),
                        doc.getMigrationId());
}

TEST_F(MovePrimaryRecipientServiceTest, OnReceiveSyncData) {
    auto movePrimaryRecipientPauseAfterPreparedState =
        globalFailPointRegistry().find("movePrimaryRecipientPauseAfterPreparedState");
    auto timesEnteredPauseAfterPreparedStateFailPoint =
        movePrimaryRecipientPauseAfterPreparedState->setMode(FailPoint::alwaysOn, 0);

    auto doc = createRecipientDoc();
    auto opCtx = operationContext();

    auto recipient = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());

    movePrimaryRecipientPauseAfterPreparedState->waitForTimesEntered(
        timesEnteredPauseAfterPreparedStateFailPoint + 1);

    auto preparedFuture = recipient->onReceiveSyncData(Timestamp());
    ASSERT_TRUE(preparedFuture.getNoThrow(opCtx).isOK());

    movePrimaryRecipientPauseAfterPreparedState->setMode(FailPoint::off, 0);

    ASSERT_TRUE(recipient->onReceiveForgetMigration().getNoThrow(opCtx).isOK());
}

TEST_F(MovePrimaryRecipientServiceTest, AbortsOnUnrecoverableClonerError) {
    // Step Down to register new test POS defined below before Step Up.
    stepDown(_serviceCtx, _registry);

    class FailingCloner : public TestClonerImpl {
        Status copyDb(OperationContext* opCtx,
                      const std::string& dBName,
                      const std::string& masterHost,
                      const std::vector<NamespaceString>& shardedColls,
                      std::set<std::string>* clonedColls) override {
            return Status(ErrorCodes::NamespaceExists, "namespace exists");
        }
    };

    class MovePrimaryRecipientServiceWithBadCloner : public MovePrimaryRecipientService {
    public:
        explicit MovePrimaryRecipientServiceWithBadCloner(ServiceContext* serviceContext)
            : MovePrimaryRecipientService(serviceContext){};

        std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
            BSONObj initialStateDoc) override {
            auto recipientStateDoc = MovePrimaryRecipientDocument::parse(
                IDLParserContext("MovePrimaryRecipientServiceWithBadCloner::constructInstance"),
                std::move(initialStateDoc));

            return std::make_shared<MovePrimaryRecipientService::MovePrimaryRecipient>(
                this,
                recipientStateDoc,
                std::make_shared<MovePrimaryRecipientExternalStateForTest>(),
                _serviceContext,
                std::make_unique<Cloner>(std::make_unique<FailingCloner>()));
        }

        StringData getServiceName() const override {
            return "MovePrimaryRecipientServiceWithBadCloner"_sd;
        }

        NamespaceString getStateDocumentsNS() const override {
            return NamespaceString::createNamespaceString_forTest(
                "config.movePrimaryRecipientsWithBadCloner");
        }
    };

    auto opCtx = operationContext();
    auto service = std::make_unique<MovePrimaryRecipientServiceWithBadCloner>(_serviceCtx);
    auto serviceName = service->getServiceName();
    _registry->registerService(std::move(service));
    auto pos = _registry->lookupServiceByName(serviceName);
    pos->startup(opCtx);
    stepUp(opCtx, _serviceCtx, _registry, _term);

    auto doc = createRecipientDoc();

    auto movePrimaryRecipientPauseBeforeCompletion =
        globalFailPointRegistry().find("movePrimaryRecipientPauseBeforeCompletion");
    auto timesEnteredPauseBeforeCompletionFailPoint =
        movePrimaryRecipientPauseBeforeCompletion->setMode(FailPoint::alwaysOn, 0);

    auto recipient =
        MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(opCtx, pos, doc.toBSON());
    ASSERT_THROWS_CODE(
        recipient->getDataClonedFuture().get(), DBException, ErrorCodes::NamespaceExists);

    movePrimaryRecipientPauseBeforeCompletion->waitForTimesEntered(
        timesEnteredPauseBeforeCompletionFailPoint + 1);

    ASSERT_THROWS_CODE(
        recipient->getCompletionFuture().get(), DBException, ErrorCodes::MovePrimaryAborted);
    movePrimaryRecipientPauseBeforeCompletion->setMode(FailPoint::off, 0);
}

TEST_F(MovePrimaryRecipientServiceTest, DropsTemporaryCollectionsMatchingPrefix) {
    auto opCtx = operationContext();
    auto doc = createRecipientDoc();

    const auto& prefix =
        NamespaceString::makeMovePrimaryTempCollectionsPrefix(doc.getMigrationId()).toString();
    const auto& fooNs = NamespaceString(prefix + "foo");
    const auto& barNs = NamespaceString(prefix + "bar");

    DBDirectClient client(opCtx);
    ASSERT_TRUE(client.createCollection(fooNs));
    ASSERT_TRUE(client.createCollection(barNs));
    // Create other collections which shouldn't be deleted.
    ASSERT_TRUE(client.createCollection(NamespaceString::kSessionTransactionsTableNamespace));
    ASSERT_TRUE(client.createCollection(NamespaceString::kMigrationRecipientsNamespace));

    auto recipient = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
        opCtx, _service, doc.toBSON());
    auto future = recipient->onReceiveForgetMigration();
    future.get(opCtx);
    {
        AutoGetDb autoDb(opCtx, DatabaseName::kConfig, MODE_S);
        ASSERT(!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, fooNs));
        ASSERT(!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, barNs));
        ASSERT(CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
            opCtx, NamespaceString::kSessionTransactionsTableNamespace));
        ASSERT(CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
            opCtx, NamespaceString::kMigrationRecipientsNamespace));
    }
}

}  // namespace mongo
