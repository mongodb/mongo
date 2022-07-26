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


#include <memory>

#include "mongo/client/connection_string.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sdam/server_description_builder.h"
#include "mongo/client/streamable_replica_set_monitor_for_testing.h"
#include "mongo/db/catalog/database_holder_mock.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/serverless/shard_split_donor_op_observer.h"
#include "mongo/db/serverless/shard_split_donor_service.h"
#include "mongo/db/serverless/shard_split_state_machine_gen.h"
#include "mongo/db/serverless/shard_split_test_utils.h"
#include "mongo/db/serverless/shard_split_utils.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

/**
 * Returns the state doc matching the document with shardSplitId from the disk if it
 * exists.
 *
 * If the stored state doc on disk contains invalid BSON, the 'InvalidBSON' error code is
 * returned.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'shardSplitId' is found.
 */
namespace {
StatusWith<ShardSplitDonorDocument> getStateDocument(OperationContext* opCtx,
                                                     const UUID& shardSplitId) {
    // Use kLastApplied so that we can read the state document as a secondary.
    ReadSourceScope readSourceScope(opCtx, RecoveryUnit::ReadSource::kLastApplied);
    AutoGetCollectionForRead collection(opCtx, NamespaceString::kShardSplitDonorsNamespace);
    if (!collection) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Collection not found looking for state document: "
                                    << NamespaceString::kShardSplitDonorsNamespace.ns());
    }

    BSONObj result;
    auto foundDoc = Helpers::findOne(opCtx,
                                     collection.getCollection(),
                                     BSON(ShardSplitDonorDocument::kIdFieldName << shardSplitId),
                                     result);

    if (!foundDoc) {
        return Status(ErrorCodes::NoMatchingDocument,
                      str::stream()
                          << "No matching state doc found with shard split id: " << shardSplitId);
    }

    try {
        return ShardSplitDonorDocument::parse(IDLParserContext("shardSplitStateDocument"), result);
    } catch (DBException& ex) {
        return ex.toStatus(str::stream()
                           << "Invalid BSON found for matching document with shard split id: "
                           << shardSplitId << " , res: " << result);
    }
}
}  // namespace

class MockReplReconfigCommandInvocation : public CommandInvocation {
public:
    MockReplReconfigCommandInvocation(const Command* command) : CommandInvocation(command) {}
    void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) final {
        result->setCommandReply(BSON("ok" << 1));
    }

    NamespaceString ns() const final {
        return NamespaceString::kSystemReplSetNamespace;
    }

    bool supportsWriteConcern() const final {
        return true;
    }

private:
    void doCheckAuthorization(OperationContext* opCtx) const final {}
};

class MockReplReconfigCommand : public Command {
public:
    MockReplReconfigCommand() : Command("replSetReconfig") {}

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& request) final {
        stdx::lock_guard<Latch> lg(_mutex);
        _hasBeenCalled = true;
        _msg = request.body;
        return std::make_unique<MockReplReconfigCommandInvocation>(this);
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const final {
        return AllowedOnSecondary::kNever;
    }

    BSONObj getLatestConfig() {
        stdx::lock_guard<Latch> lg(_mutex);
        ASSERT_TRUE(_hasBeenCalled);
        return _msg;
    }

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("MockReplReconfigCommand::_mutex");
    bool _hasBeenCalled{false};
    BSONObj _msg;
} mockReplSetReconfigCmd;

namespace {
sdam::TopologyDescriptionPtr makeRecipientTopologyDescription(const MockReplicaSet& set) {
    std::shared_ptr<TopologyDescription> topologyDescription =
        std::make_shared<sdam::TopologyDescription>(sdam::SdamConfiguration(
            set.getHosts(), sdam::TopologyType::kReplicaSetNoPrimary, set.getSetName()));

    for (auto& server : set.getHosts()) {
        auto serverDescription = sdam::ServerDescriptionBuilder()
                                     .withAddress(server)
                                     .withSetName(set.getSetName())
                                     .instance();
        topologyDescription->installServerDescription(serverDescription);
    }

    return topologyDescription;
}

}  // namespace

std::ostream& operator<<(std::ostream& builder, mongo::ShardSplitDonorStateEnum state) {
    switch (state) {
        case mongo::ShardSplitDonorStateEnum::kUninitialized:
            builder << "kUninitialized";
            break;
        case mongo::ShardSplitDonorStateEnum::kAbortingIndexBuilds:
            builder << "kAbortingIndexBuilds";
            break;
        case mongo::ShardSplitDonorStateEnum::kAborted:
            builder << "kAborted";
            break;
        case mongo::ShardSplitDonorStateEnum::kBlocking:
            builder << "kBlocking";
            break;
        case mongo::ShardSplitDonorStateEnum::kCommitted:
            builder << "kCommitted";
            break;
    }

    return builder;
}

void fastForwardCommittedSnapshotOpTime(
    std::shared_ptr<ShardSplitDonorService::DonorStateMachine> instance,
    ServiceContext* serviceContext,
    OperationContext* opCtx,
    const UUID& uuid) {
    // When a state document is transitioned to kAborted, the ShardSplitDonorOpObserver will
    // transition tenant access blockers to a kAborted state if, and only if, the abort timestamp
    // is less than or equal to the currentCommittedSnapshotOpTime. Since we are using the
    // ReplicationCoordinatorMock, we must manually manage the currentCommittedSnapshotOpTime
    // using this method.
    auto replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
        repl::ReplicationCoordinator::get(serviceContext));

    auto foundStateDoc = uassertStatusOK(getStateDocument(opCtx, uuid));
    invariant(foundStateDoc.getCommitOrAbortOpTime());

    replCoord->setCurrentCommittedSnapshotOpTime(*foundStateDoc.getCommitOrAbortOpTime());
    serviceContext->getOpObserver()->onMajorityCommitPointUpdate(
        serviceContext, *foundStateDoc.getCommitOrAbortOpTime());
}

bool hasActiveSplitForTenants(OperationContext* opCtx,
                              const std::vector<std::string>& tenantNames) {
    return std::all_of(tenantNames.begin(), tenantNames.end(), [&](const auto& tenantName) {
        return tenant_migration_access_blocker::hasActiveTenantMigration(
            opCtx, StringData(tenantName + "_db"));
    });
}

std::pair<bool, executor::RemoteCommandRequest> checkRemoteNameEquals(
    const std::string& commandName, const executor::RemoteCommandRequest& request) {
    auto&& cmdObj = request.cmdObj;
    ASSERT_FALSE(cmdObj.isEmpty());

    if (commandName == cmdObj.firstElementFieldName()) {
        return std::make_pair(true, request);
    }

    return std::make_pair<bool, executor::RemoteCommandRequest>(false, {});
}

executor::RemoteCommandRequest assertRemoteCommandIn(
    std::vector<std::string> commandNames, const executor::RemoteCommandRequest& request) {
    for (const auto& name : commandNames) {
        if (auto res = checkRemoteNameEquals(name, request); res.first) {
            return res.second;
        }
    }

    std::stringstream ss;
    ss << "Expected one of the following commands : [\"";
    for (const auto& name : commandNames) {
        ss << name << "\",";
    }
    ss << "] in remote command request but found \"" << request.cmdObj.firstElementFieldName()
       << "\" instead: " << request.toString();

    FAIL(ss.str());

    return request;
}

executor::RemoteCommandRequest assertRemoteCommandNameEquals(
    const std::string& cmdName, const executor::RemoteCommandRequest& request) {
    auto&& cmdObj = request.cmdObj;
    ASSERT_FALSE(cmdObj.isEmpty());
    if (auto res = checkRemoteNameEquals(cmdName, request); res.first) {
        return res.second;
    } else {
        std::string msg = str::stream()
            << "Expected command name \"" << cmdName << "\" in remote command request but found \""
            << cmdObj.firstElementFieldName() << "\" instead: " << request.toString();
        FAIL(msg);
        return {};
    }
}

bool processReplSetStepUpRequest(executor::NetworkInterfaceMock* net,
                                 MockReplicaSet* replSet,
                                 Status statusToReturn) {
    const std::string commandName{"replSetStepUp"};

    ASSERT(net->hasReadyRequests());
    net->runReadyNetworkOperations();
    auto noi = net->getNextReadyRequest();
    auto request = noi->getRequest();

    // The command can also be `isMaster`
    assertRemoteCommandIn({"replSetStepUp", "isMaster"}, request);

    auto&& cmdObj = request.cmdObj;
    auto requestHost = request.target.toString();
    const auto node = replSet->getNode(requestHost);
    if (node->isRunning()) {
        if (commandName == cmdObj.firstElementFieldName() && !statusToReturn.isOK()) {
            net->scheduleErrorResponse(noi, statusToReturn);
        } else {
            const auto opmsg = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
            const auto reply = node->runCommand(request.id, opmsg)->getCommandReply();
            net->scheduleSuccessfulResponse(
                noi, executor::RemoteCommandResponse(reply, Milliseconds(0)));
        }
    } else {
        net->scheduleErrorResponse(noi, Status(ErrorCodes::HostUnreachable, "generated by test"));
    }

    return commandName == cmdObj.firstElementFieldName();
}

void processHelloRequest(executor::NetworkInterfaceMock* net, MockReplicaSet* replSet) {
    ASSERT(net->hasReadyRequests());
    net->runReadyNetworkOperations();
    auto noi = net->getNextReadyRequest();
    auto request = noi->getRequest();


    assertRemoteCommandNameEquals("isMaster", request);
    auto requestHost = request.target.toString();
    const auto node = replSet->getNode(requestHost);
    if (node->isRunning()) {
        const auto opmsg = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto reply = node->runCommand(request.id, opmsg)->getCommandReply();
        net->scheduleSuccessfulResponse(noi,
                                        executor::RemoteCommandResponse(reply, Milliseconds(0)));
    } else {
        net->scheduleErrorResponse(noi, Status(ErrorCodes::HostUnreachable, ""));
    }
}

void waitForReadyRequest(executor::NetworkInterfaceMock* net) {
    while (!net->hasReadyRequests()) {
        net->advanceTime(net->now() + Milliseconds{1});
    }
}
class ShardSplitDonorServiceTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        // The database needs to be open before using shard split donor service.
        {
            auto opCtx = cc().makeOperationContext();
            AutoGetDb autoDb(
                opCtx.get(), NamespaceString::kShardSplitDonorsNamespace.dbName(), MODE_X);
            auto db = autoDb.ensureDbExists(opCtx.get());
            ASSERT_TRUE(db);
        }

        // Timestamps of "0 seconds" are not allowed, so we must advance our clock mock to the first
        // real second. Don't save an instance, since this just internally modified the global
        // immortal ClockSourceMockImpl.
        ClockSourceMock clockSource;
        clockSource.advance(Milliseconds(1000));

        // setup mock networking for split acceptance
        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _net = network.get();
        _executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
            std::make_unique<executor::ThreadPoolMock>(
                _net, 1, executor::ThreadPoolMock::Options{}),
            std::move(network));
        _executor->startup();

        ShardSplitDonorService::DonorStateMachine::setSplitAcceptanceTaskExecutor_forTest(
            _executor);
    }

    void tearDown() override {
        _net->exitNetwork();
        _executor->shutdown();
        _executor->join();

        repl::PrimaryOnlyServiceMongoDTest::tearDown();
    }

protected:
    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        return std::make_unique<ShardSplitDonorService>(serviceContext);
    }

    void setUpOpObserverRegistry(OpObserverRegistry* opObserverRegistry) override {
        opObserverRegistry->addObserver(std::make_unique<ShardSplitDonorOpObserver>());
    }

    ShardSplitDonorDocument defaultStateDocument() const {
        return ShardSplitDonorDocument::parse(
            {"donor.document"},
            BSON("_id" << _uuid << "tenantIds" << _tenantIds << "recipientTagName"
                       << _recipientTagName << "recipientSetName" << _recipientSetName));
    }

    /**
     * Wait for replSetStepUp command, enqueue hello response, and ignore heartbeats.
     */
    void waitForReplSetStepUp(Status statusToReturn) {
        _net->enterNetwork();
        do {
            waitForReadyRequest(_net);
        } while (!processReplSetStepUpRequest(_net, &_recipientSet, statusToReturn));
        _net->runReadyNetworkOperations();
        _net->exitNetwork();
    }

    /**
     * Wait for monitors to start, and enqueue successfull hello responses
     */
    void waitForMonitorAndProcessHello() {
        _net->enterNetwork();
        waitForReadyRequest(_net);
        processHelloRequest(_net, &_recipientSet);
        waitForReadyRequest(_net);
        processHelloRequest(_net, &_recipientSet);
        waitForReadyRequest(_net);
        processHelloRequest(_net, &_recipientSet);
        _net->runReadyNetworkOperations();
        _net->exitNetwork();
    }

    UUID _uuid = UUID::gen();
    MockReplicaSet _replSet{
        "donorSetForTest", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};
    MockReplicaSet _recipientSet{
        "recipientSetForTest", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};
    const NamespaceString _nss{"testDB2", "testColl2"};
    std::vector<std::string> _tenantIds = {"tenant1", "tenantAB"};
    std::string _recipientTagName{"$recipientNode"};
    std::string _recipientSetName{_recipientSet.getURI().getSetName()};

    std::unique_ptr<FailPointEnableBlock> _skipAcceptanceFP =
        std::make_unique<FailPointEnableBlock>("skipShardSplitWaitForSplitAcceptance");


    // for mocking split acceptance
    executor::NetworkInterfaceMock* _net;
    TaskExecutorPtr _executor;
};

void setUpCommandResult(MockReplicaSet* replSet,
                        const HostAndPort& target,
                        const std::string& command,
                        BSONObj result) {
    auto node = replSet->getNode(target.toString());

    node->setCommandReply(command, result);
}


void setUpReplSetUpCommandResult(MockReplicaSet* replSet) {
    // Due to use of std::shuffle to select the recipient to target, any of the node can receive the
    // command.

    for (auto hostAndPort : replSet->getHosts()) {
        auto node = replSet->getNode(hostAndPort.toString());

        node->setCommandReply("replSetStepUp", BSON("ok" << 1));
    }
}

TEST_F(ShardSplitDonorServiceTest, BasicShardSplitDonorServiceInstanceCreation) {
    auto opCtx = makeOperationContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    // Shard split service will send a stepUp request to the first node in the vector.
    setUpReplSetUpCommandResult(&_recipientSet);

    // We disable this failpoint to test complete functionality. waitForMonitorAndProcessHello()
    // returns hello responses that makes split acceptance pass.
    _skipAcceptanceFP.reset();

    // Create and start the instance.
    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, defaultStateDocument().toBSON());
    ASSERT(serviceInstance.get());
    ASSERT_EQ(_uuid, serviceInstance->getId());

    waitForMonitorAndProcessHello();
    waitForReplSetStepUp(Status(ErrorCodes::OK, ""));

    auto result = serviceInstance->decisionFuture().get();
    ASSERT_TRUE(hasActiveSplitForTenants(opCtx.get(), _tenantIds));
    ASSERT(!result.abortReason);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kCommitted);

    serviceInstance->tryForget();
    auto completionFuture = serviceInstance->completionFuture();
    completionFuture.wait();

    ASSERT_OK(serviceInstance->completionFuture().getNoThrow());
    ASSERT_TRUE(serviceInstance->isGarbageCollectable());
}

TEST_F(ShardSplitDonorServiceTest, ReplSetStepUpTryAgain) {
    auto opCtx = makeOperationContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    // Shard split service will send a stepUp request to the first node in the vector. When it fails
    // it will send it to the next node.
    setUpReplSetUpCommandResult(&_recipientSet);

    // We disable this failpoint to test complete functionality. waitForMonitorAndProcessHello()
    // returns hello responses that makes split acceptance pass.
    _skipAcceptanceFP.reset();

    // Create and start the instance.
    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, defaultStateDocument().toBSON());
    ASSERT(serviceInstance.get());
    ASSERT_EQ(_uuid, serviceInstance->getId());

    waitForMonitorAndProcessHello();

    // We expect to get two replSetStepUp commands. The first will fail and the service will resend
    // the command to the next node.
    waitForReplSetStepUp(Status(ErrorCodes::OperationFailed, ""));
    waitForReplSetStepUp(Status(ErrorCodes::OK, ""));

    auto result = serviceInstance->decisionFuture().get();

    ASSERT(!result.abortReason);

    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kCommitted);
}

TEST_F(ShardSplitDonorServiceTest, ReplSetStepUpFails) {
    // Test that, even if both replSetStepUp fail, the split will succeed as long as all nodes have
    // joined the recipient set.

    auto opCtx = makeOperationContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    // Shard split service will send a stepUp request to the first node in the vector. When it fails
    // it will send it to the next node.
    setUpReplSetUpCommandResult(&_recipientSet);

    // We disable this failpoint to test complete functionality. waitForMonitorAndProcessHello()
    // returns hello responses that makes split acceptance pass.
    _skipAcceptanceFP.reset();

    // Create and start the instance.
    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, defaultStateDocument().toBSON());
    ASSERT(serviceInstance.get());
    ASSERT_EQ(_uuid, serviceInstance->getId());

    waitForMonitorAndProcessHello();

    // Shard split will retry the command once. If both
    waitForReplSetStepUp(Status(ErrorCodes::OperationFailed, ""));
    waitForReplSetStepUp(Status(ErrorCodes::OperationFailed, ""));

    auto result = serviceInstance->decisionFuture().get();

    ASSERT(!result.abortReason);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kCommitted);
}

TEST_F(ShardSplitDonorServiceTest, ReplSetStepUpRetryable) {
    auto opCtx = makeOperationContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    // Shard split service will send a stepUp request to the first node in the vector. When it fails
    // it will send it to the next node.
    setUpReplSetUpCommandResult(&_recipientSet);

    // We disable this failpoint to test complete functionality. waitForMonitorAndProcessHello()
    // returns hello responses that makes split acceptance pass.
    _skipAcceptanceFP.reset();

    // Create and start the instance.
    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, defaultStateDocument().toBSON());
    ASSERT(serviceInstance.get());
    ASSERT_EQ(_uuid, serviceInstance->getId());

    waitForMonitorAndProcessHello();

    // Shard split will retry the command indefinitely for timeout/retriable errors.
    waitForReplSetStepUp(Status(ErrorCodes::NetworkTimeout, "test-generated retryable error"));
    waitForReplSetStepUp(Status(ErrorCodes::SocketException, "test-generated retryable error"));
    waitForReplSetStepUp(
        Status(ErrorCodes::ConnectionPoolExpired, "test-generated retryable error"));
    waitForReplSetStepUp(Status(ErrorCodes::ExceededTimeLimit, "test-generated retryable error"));
    waitForReplSetStepUp(Status(ErrorCodes::OK, "test-generated retryable error"));

    auto result = serviceInstance->decisionFuture().get();

    ASSERT(!result.abortReason);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kCommitted);
}

TEST_F(ShardSplitDonorServiceTest, ShardSplitDonorServiceTimeout) {
    FailPointEnableBlock fp("pauseShardSplitAfterBlocking");

    auto opCtx = makeOperationContext();
    auto serviceContext = getServiceContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        serviceContext, _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    auto stateDocument = defaultStateDocument();

    // Set a timeout of 200 ms, and make sure we reset after this test is run
    RAIIServerParameterControllerForTest controller{"shardSplitTimeoutMS", 200};

    // Create and start the instance.
    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, stateDocument.toBSON());
    ASSERT(serviceInstance.get());
    ASSERT_EQ(_uuid, serviceInstance->getId());

    auto result = serviceInstance->decisionFuture().get();

    ASSERT(result.abortReason);
    ASSERT_EQ(result.abortReason->code(), ErrorCodes::ExceededTimeLimit);

    fastForwardCommittedSnapshotOpTime(serviceInstance, serviceContext, opCtx.get(), _uuid);
    serviceInstance->tryForget();

    ASSERT_OK(serviceInstance->completionFuture().getNoThrow());
    ASSERT_TRUE(serviceInstance->isGarbageCollectable());
}

TEST_F(ShardSplitDonorServiceTest, ReconfigToRemoveSplitConfig) {
    auto opCtx = makeOperationContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    // Shard split service will send a stepUp request to the first node in the vector.
    setUpReplSetUpCommandResult(&_recipientSet);

    _skipAcceptanceFP.reset();

    auto fpPtr = std::make_unique<FailPointEnableBlock>("pauseShardSplitBeforeSplitConfigRemoval");
    auto initialTimesEntered = fpPtr->initialTimesEntered();

    // Create and start the instance.
    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, defaultStateDocument().toBSON());
    ASSERT(serviceInstance.get());
    ASSERT_EQ(_uuid, serviceInstance->getId());

    waitForMonitorAndProcessHello();

    waitForReplSetStepUp(Status::OK());

    auto result = serviceInstance->decisionFuture().get();
    ASSERT(!result.abortReason);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kCommitted);

    (*fpPtr)->waitForTimesEntered(initialTimesEntered + 1);

    // Validate we currently have a splitConfig and set it as the mock's return value.
    BSONObj splitConfigBson = mockReplSetReconfigCmd.getLatestConfig();
    auto splitConfig = repl::ReplSetConfig::parse(splitConfigBson["replSetReconfig"].Obj());
    ASSERT(splitConfig.isSplitConfig());
    auto replCoord = repl::ReplicationCoordinator::get(getServiceContext());
    dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord)->setGetConfigReturnValue(
        splitConfig);

    // Clear the failpoint and wait for completion.
    fpPtr.reset();
    serviceInstance->tryForget();

    auto completionFuture = serviceInstance->completionFuture();
    completionFuture.wait();

    BSONObj finalConfigBson = mockReplSetReconfigCmd.getLatestConfig();
    ASSERT_TRUE(finalConfigBson.hasField("replSetReconfig"));
    auto finalConfig = repl::ReplSetConfig::parse(finalConfigBson["replSetReconfig"].Obj());
    ASSERT(!finalConfig.isSplitConfig());
}

// Abort scenario : abortSplit called before startSplit.
TEST_F(ShardSplitDonorServiceTest, CreateInstanceInAbortedState) {
    auto opCtx = makeOperationContext();
    auto serviceContext = getServiceContext();

    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        serviceContext, _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    auto stateDocument = defaultStateDocument();
    stateDocument.setState(ShardSplitDonorStateEnum::kAborted);

    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, stateDocument.toBSON());
    ASSERT(serviceInstance.get());

    auto result = serviceInstance->decisionFuture().get(opCtx.get());

    ASSERT(!!result.abortReason);
    ASSERT_EQ(result.abortReason->code(), ErrorCodes::TenantMigrationAborted);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kAborted);

    serviceInstance->tryForget();

    ASSERT_OK(serviceInstance->completionFuture().getNoThrow());
    ASSERT_TRUE(serviceInstance->isGarbageCollectable());
}

// Abort scenario : instance created through startSplit then calling abortSplit.
TEST_F(ShardSplitDonorServiceTest, CreateInstanceThenAbort) {
    auto opCtx = makeOperationContext();
    auto serviceContext = getServiceContext();

    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        serviceContext, _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    std::shared_ptr<ShardSplitDonorService::DonorStateMachine> serviceInstance;
    {
        FailPointEnableBlock fp("pauseShardSplitAfterBlocking");
        auto initialTimesEntered = fp.initialTimesEntered();

        serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
            opCtx.get(), _service, defaultStateDocument().toBSON());
        ASSERT(serviceInstance.get());

        fp->waitForTimesEntered(initialTimesEntered + 1);

        serviceInstance->tryAbort();
    }

    auto result = serviceInstance->decisionFuture().get(opCtx.get());

    ASSERT(!!result.abortReason);
    ASSERT_EQ(result.abortReason->code(), ErrorCodes::TenantMigrationAborted);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kAborted);

    fastForwardCommittedSnapshotOpTime(serviceInstance, serviceContext, opCtx.get(), _uuid);
    serviceInstance->tryForget();

    ASSERT_OK(serviceInstance->completionFuture().getNoThrow());
    ASSERT_TRUE(serviceInstance->isGarbageCollectable());
}

TEST_F(ShardSplitDonorServiceTest, StepDownTest) {
    auto opCtx = makeOperationContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    std::shared_ptr<ShardSplitDonorService::DonorStateMachine> serviceInstance;

    {
        FailPointEnableBlock fp("pauseShardSplitAfterBlocking");
        auto initialTimesEntered = fp.initialTimesEntered();

        serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
            opCtx.get(), _service, defaultStateDocument().toBSON());
        ASSERT(serviceInstance.get());

        fp->waitForTimesEntered(initialTimesEntered + 1);

        stepDown();
    }

    auto result = serviceInstance->decisionFuture().getNoThrow();
    ASSERT_FALSE(result.isOK());
    ASSERT_EQ(ErrorCodes::InterruptedDueToReplStateChange, result.getStatus());

    ASSERT_EQ(serviceInstance->completionFuture().getNoThrow(),
              ErrorCodes::InterruptedDueToReplStateChange);
    ASSERT_FALSE(serviceInstance->isGarbageCollectable());
}

TEST_F(ShardSplitDonorServiceTest, DeleteStateDocMarkedGarbageCollectable) {
    // Instance building (from inserted state document) is done in a separate thread. This failpoint
    // disable it to ensure there's no race condition with the insertion of the state document.
    FailPointEnableBlock fp("PrimaryOnlyServiceSkipRebuildingInstances");

    auto opCtx = makeOperationContext();

    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    auto stateDocument = defaultStateDocument();
    stateDocument.setState(ShardSplitDonorStateEnum::kAborted);
    stateDocument.setCommitOrAbortOpTime(repl::OpTime(Timestamp(1, 1), 1));

    Status status(ErrorCodes::CallbackCanceled, "Split has been aborted");
    BSONObjBuilder bob;
    status.serializeErrorToBSON(&bob);
    stateDocument.setAbortReason(bob.obj());

    boost::optional<mongo::Date_t> expireAt = getServiceContext()->getFastClockSource()->now() +
        Milliseconds{repl::shardSplitGarbageCollectionDelayMS.load()};
    stateDocument.setExpireAt(expireAt);

    // insert the document for the first time.
    ASSERT_OK(serverless::insertStateDoc(opCtx.get(), stateDocument));

    // deletes a document that was marked as garbage collectable and succeeds.
    StatusWith<bool> deleted = serverless::deleteStateDoc(opCtx.get(), stateDocument.getId());

    ASSERT_OK(deleted.getStatus());
    ASSERT_TRUE(deleted.getValue());

    ASSERT_EQ(getStateDocument(opCtx.get(), _uuid).getStatus().code(),
              ErrorCodes::NoMatchingDocument);
}

TEST_F(ShardSplitDonorServiceTest, AbortDueToRecipientNodesValidation) {
    auto opCtx = makeOperationContext();
    auto serviceContext = getServiceContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());

    // Matching recipientSetName to the replSetName to fail validation and abort shard split.
    test::shard_split::reconfigToAddRecipientNodes(
        serviceContext, _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    auto stateDocument = defaultStateDocument();
    stateDocument.setRecipientSetName("donor"_sd);

    // Create and start the instance.
    auto serviceInstance = ShardSplitDonorService::DonorStateMachine::getOrCreate(
        opCtx.get(), _service, stateDocument.toBSON());
    ASSERT(serviceInstance.get());
    ASSERT_EQ(_uuid, serviceInstance->getId());

    auto result = serviceInstance->decisionFuture().get();

    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kAborted);
    ASSERT(result.abortReason);
    ASSERT_EQ(result.abortReason->code(), ErrorCodes::BadValue);
    ASSERT_TRUE(serviceInstance->isGarbageCollectable());

    auto statusWithDoc = getStateDocument(opCtx.get(), stateDocument.getId());
    ASSERT_OK(statusWithDoc.getStatus());

    ASSERT_EQ(statusWithDoc.getValue().getState(), ShardSplitDonorStateEnum::kAborted);
}

TEST(RecipientAcceptSplitListenerTest, FutureReady) {
    MockReplicaSet donor{"donor", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};
    auto listener =
        mongo::serverless::RecipientAcceptSplitListener(donor.getURI().connectionString());

    for (const auto& host : donor.getHosts()) {
        ASSERT_FALSE(listener.getFuture().isReady());
        listener.onServerHeartbeatSucceededEvent(host, BSON("setName" << donor.getSetName()));
    }

    ASSERT_TRUE(listener.getFuture().isReady());
}

TEST(RecipientAcceptSplitListenerTest, FutureReadyNameChange) {
    MockReplicaSet donor{"donor", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};
    auto listener =
        mongo::serverless::RecipientAcceptSplitListener(donor.getURI().connectionString());

    for (const auto& host : donor.getHosts()) {
        listener.onServerHeartbeatSucceededEvent(host,
                                                 BSON("setName"
                                                      << "invalidSetName"));
    }

    ASSERT_FALSE(listener.getFuture().isReady());

    for (const auto& host : donor.getHosts()) {
        listener.onServerHeartbeatSucceededEvent(host, BSON("setName" << donor.getSetName()));
    }

    ASSERT_TRUE(listener.getFuture().isReady());
}

TEST(RecipientAcceptSplitListenerTest, FutureNotReadyMissingNodes) {
    MockReplicaSet donor{"donor", 3, false /* hasPrimary */, false /* dollarPrefixHosts */};
    auto listener =
        mongo::serverless::RecipientAcceptSplitListener(donor.getURI().connectionString());


    for (size_t i = 0; i < donor.getHosts().size() - 1; ++i) {
        listener.onServerHeartbeatSucceededEvent(donor.getHosts()[i],
                                                 BSON("setName" << donor.getSetName()));
    }

    ASSERT_FALSE(listener.getFuture().isReady());
    listener.onServerHeartbeatSucceededEvent(donor.getHosts()[donor.getHosts().size() - 1],
                                             BSON("setName" << donor.getSetName()));

    ASSERT_TRUE(listener.getFuture().isReady());
}

TEST(RecipientAcceptSplitListenerTest, FutureNotReadyNoSetName) {
    MockReplicaSet donor{"donor", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};
    auto listener =
        mongo::serverless::RecipientAcceptSplitListener(donor.getURI().connectionString());

    for (size_t i = 0; i < donor.getHosts().size() - 1; ++i) {
        listener.onServerHeartbeatSucceededEvent(donor.getHosts()[i], BSONObj());
    }

    ASSERT_FALSE(listener.getFuture().isReady());
}

TEST(RecipientAcceptSplitListenerTest, FutureNotReadyWrongSet) {
    MockReplicaSet donor{"donor", 3, true /* hasPrimary */, false /* dollarPrefixHosts */};
    auto listener =
        mongo::serverless::RecipientAcceptSplitListener(donor.getURI().connectionString());

    for (const auto& host : donor.getHosts()) {
        listener.onServerHeartbeatSucceededEvent(host,
                                                 BSON("setName"
                                                      << "wrongSetName"));
    }

    ASSERT_FALSE(listener.getFuture().isReady());
}

TEST_F(ShardSplitDonorServiceTest, ResumeAfterStepdownTest) {
    auto opCtx = makeOperationContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());
    test::shard_split::reconfigToAddRecipientNodes(
        getServiceContext(), _recipientTagName, _replSet.getHosts(), _recipientSet.getHosts());

    setUpReplSetUpCommandResult(&_recipientSet);

    auto firstSplitInstance = [&]() {
        FailPointEnableBlock fp("pauseShardSplitAfterBlocking");
        auto initialTimesEntered = fp.initialTimesEntered();

        std::shared_ptr<ShardSplitDonorService::DonorStateMachine> serviceInstance =
            ShardSplitDonorService::DonorStateMachine::getOrCreate(
                opCtx.get(), _service, defaultStateDocument().toBSON());
        ASSERT(serviceInstance.get());

        fp->waitForTimesEntered(initialTimesEntered + 1);
        return serviceInstance;
    }();

    stepDown();
    auto result = firstSplitInstance->completionFuture().getNoThrow();
    ASSERT_FALSE(result.isOK());
    ASSERT_EQ(ErrorCodes::InterruptedDueToReplStateChange, result.code());

    auto secondSplitInstance = [&]() {
        FailPointEnableBlock fp("pauseShardSplitAfterBlocking");
        stepUp(opCtx.get());
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);

        ASSERT_OK(getStateDocument(opCtx.get(), _uuid).getStatus());
        auto serviceInstance = ShardSplitDonorService::DonorStateMachine::lookup(
            opCtx.get(), _service, BSON("_id" << _uuid));
        ASSERT(serviceInstance);
        return *serviceInstance;
    }();

    waitForReplSetStepUp(Status(ErrorCodes::OK, ""));

    ASSERT_OK(secondSplitInstance->decisionFuture().getNoThrow().getStatus());

    secondSplitInstance->tryForget();
    ASSERT_OK(secondSplitInstance->completionFuture().getNoThrow());
    ASSERT_TRUE(secondSplitInstance->isGarbageCollectable());
}

class ShardSplitPersistenceTest : public ShardSplitDonorServiceTest {
public:
    void setUpPersistence(OperationContext* opCtx) override {

        // We need to allow writes during the test's setup.
        auto replCoord = dynamic_cast<repl::ReplicationCoordinatorMock*>(
            repl::ReplicationCoordinator::get(opCtx->getServiceContext()));
        replCoord->alwaysAllowWrites(true);

        replCoord->setGetConfigReturnValue(initialDonorConfig());

        _recStateDoc = initialStateDocument();
        uassertStatusOK(serverless::insertStateDoc(opCtx, _recStateDoc));

        _pauseBeforeRecipientCleanupFp =
            std::make_unique<FailPointEnableBlock>("pauseShardSplitBeforeRecipientCleanup");

        _initialTimesEntered = _pauseBeforeRecipientCleanupFp->initialTimesEntered();
    }

    virtual repl::ReplSetConfig initialDonorConfig() = 0;

    virtual ShardSplitDonorDocument initialStateDocument() = 0;

protected:
    ShardSplitDonorDocument _recStateDoc;
    std::unique_ptr<FailPointEnableBlock> _pauseBeforeRecipientCleanupFp;
    FailPoint::EntryCountT _initialTimesEntered;
};

class ShardSplitRecipientCleanupTest : public ShardSplitPersistenceTest {
public:
    repl::ReplSetConfig initialDonorConfig() override {
        BSONArrayBuilder members;
        members.append(BSON("_id" << 1 << "host"
                                  << "node1"
                                  << "tags" << BSON("recipientTagName" << UUID::gen().toString())));

        return repl::ReplSetConfig::parse(BSON("_id" << _recipientSetName << "version" << 1
                                                     << "protocolVersion" << 1 << "members"
                                                     << members.arr()));
    }

    ShardSplitDonorDocument initialStateDocument() override {

        auto stateDocument = defaultStateDocument();
        stateDocument.setBlockTimestamp(Timestamp(1, 1));
        stateDocument.setState(ShardSplitDonorStateEnum::kBlocking);
        stateDocument.setRecipientConnectionString(ConnectionString::forLocal());

        return stateDocument;
    }
};

TEST_F(ShardSplitRecipientCleanupTest, ShardSplitRecipientCleanup) {
    auto opCtx = makeOperationContext();
    test::shard_split::ScopedTenantAccessBlocker scopedTenants(_tenantIds, opCtx.get());

    ASSERT_OK(getStateDocument(opCtx.get(), _uuid).getStatus());

    ASSERT_FALSE(hasActiveSplitForTenants(opCtx.get(), _tenantIds));

    auto decisionFuture = [&]() {
        ASSERT(_pauseBeforeRecipientCleanupFp);
        (*(_pauseBeforeRecipientCleanupFp.get()))->waitForTimesEntered(_initialTimesEntered + 1);

        tenant_migration_access_blocker::recoverTenantMigrationAccessBlockers(opCtx.get());

        auto splitService = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                                ->lookupServiceByName(ShardSplitDonorService::kServiceName);
        auto optionalDonor = ShardSplitDonorService::DonorStateMachine::lookup(
            opCtx.get(), splitService, BSON("_id" << _uuid));

        ASSERT_TRUE(hasActiveSplitForTenants(opCtx.get(), _tenantIds));

        ASSERT_TRUE(optionalDonor);
        auto serviceInstance = optionalDonor.get();
        ASSERT(serviceInstance.get());

        _pauseBeforeRecipientCleanupFp.reset();

        return serviceInstance->decisionFuture();
    }();

    auto result = decisionFuture.get();

    // We set the promise before the future chain. Cleanup will return kCommitted as a result.
    ASSERT(!result.abortReason);
    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kCommitted);

    // deleted the local state doc so this should return NoMatchingDocument
    ASSERT_EQ(getStateDocument(opCtx.get(), _uuid).getStatus().code(),
              ErrorCodes::NoMatchingDocument);
}

class ShardSplitAbortedStepUpTest : public ShardSplitPersistenceTest {
public:
    repl::ReplSetConfig initialDonorConfig() override {
        BSONArrayBuilder members;
        members.append(BSON("_id" << 1 << "host"
                                  << "node1"));

        return repl::ReplSetConfig::parse(BSON("_id"
                                               << "donorSetName"
                                               << "version" << 1 << "protocolVersion" << 1
                                               << "members" << members.arr()));
    }

    ShardSplitDonorDocument initialStateDocument() override {

        auto stateDocument = defaultStateDocument();

        stateDocument.setState(mongo::ShardSplitDonorStateEnum::kAborted);
        stateDocument.setBlockTimestamp(Timestamp(1, 1));
        stateDocument.setCommitOrAbortOpTime(repl::OpTime(Timestamp(1, 1), 1));

        Status status(ErrorCodes::InternalError, abortReason);
        BSONObjBuilder bob;
        status.serializeErrorToBSON(&bob);
        stateDocument.setAbortReason(bob.obj());

        return stateDocument;
    }

    std::string abortReason{"Testing simulated error"};
};

TEST_F(ShardSplitAbortedStepUpTest, ShardSplitAbortedStepUp) {
    auto opCtx = makeOperationContext();
    auto splitService = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                            ->lookupServiceByName(ShardSplitDonorService::kServiceName);
    auto optionalDonor = ShardSplitDonorService::DonorStateMachine::lookup(
        opCtx.get(), splitService, BSON("_id" << _uuid));

    ASSERT(optionalDonor);
    auto result = optionalDonor->get()->decisionFuture().get();

    ASSERT_EQ(result.state, mongo::ShardSplitDonorStateEnum::kAborted);
    ASSERT_TRUE(!!result.abortReason);
    ASSERT_EQ(result.abortReason->code(), ErrorCodes::InternalError);
    ASSERT_EQ(result.abortReason->reason(), abortReason);
}

}  // namespace mongo
