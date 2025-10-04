/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/repl/replication_coordinator_test_fixture.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/hello/hello_response.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#include <cstdint>
#include <list>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace repl {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

executor::TaskExecutor* ReplCoordTest::getReplExec() {
    return _replExec;
}

ReplSetConfig ReplCoordTest::assertMakeRSConfig(const BSONObj& configBson) {
    auto config = ReplSetConfig::parse(configBson);
    ASSERT_OK(config.validate());
    return config;
}

BSONObj ReplCoordTest::addProtocolVersion(const BSONObj& configDoc, int protocolVersion) {
    BSONObjBuilder builder;
    builder << "protocolVersion" << protocolVersion;
    builder.appendElementsUnique(configDoc);
    return builder.obj();
}

ReplCoordTest::ReplCoordTest(Options options) : ServiceContextMongoDTest(std::move(options)) {
    _settings.setReplSetString("mySet/node1:12345,node2:54321");
}

ReplCoordTest::~ReplCoordTest() {
    globalFailPointRegistry().find("blockHeartbeatReconfigFinish")->setMode(FailPoint::off);

    if (_callShutdown) {
        auto opCtx = makeOperationContext();
        shutdown(opCtx.get());
    }
}

void ReplCoordTest::assertRunUntil(Date_t newTime) {
    this->_net->runUntil(newTime);
    ASSERT_EQUALS(newTime, getNet()->now());
}

void ReplCoordTest::enterNetwork() {
    getNet()->enterNetwork();
}

void ReplCoordTest::exitNetwork() {
    getNet()->exitNetwork();
}

void ReplCoordTest::addSelf(const HostAndPort& selfHost) {
    getExternalState()->addSelf(selfHost);
}

void ReplCoordTest::init() {
    invariant(!_repl);
    invariant(!_callShutdown);
    cc().setOperationUnkillable_ForTest();

    auto service = getGlobalServiceContext();
    _storageInterface = new StorageInterfaceMock();
    StorageInterface::set(service, std::unique_ptr<StorageInterface>(_storageInterface));
    ASSERT_TRUE(_storageInterface == StorageInterface::get(service));
    // We define these two function mocks for _storageInterface so we can successfully store the
    // FCV document in the replica set initiate command code path.
    _storageInterface->insertDocumentFn = [this](OperationContext* opCtx,
                                                 const NamespaceStringOrUUID& nsOrUUID,
                                                 const TimestampedBSONObj& doc,
                                                 long long term) {
        return Status::OK();
    };

    _storageInterface->createCollFn = [this](OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionOptions& options) {
        return Status::OK();
    };

    ReplicationProcess::set(
        service,
        std::make_unique<ReplicationProcess>(_storageInterface,
                                             std::make_unique<ReplicationConsistencyMarkersMock>(),
                                             std::make_unique<ReplicationRecoveryMock>()));
    auto replicationProcess = ReplicationProcess::get(service);

    // PRNG seed for tests.
    const int64_t seed = 0;

    // The ReadWriteConcernDefaults decoration on the service context won't always be created,
    // so we should manually instantiate it to ensure it exists in our tests.
    ReadWriteConcernDefaults::create(service->getService(), lookupMock.getFetchDefaultsFn());

    TopologyCoordinator::Options settings;
    auto topo = std::make_unique<TopologyCoordinator>(settings);
    auto net = std::make_shared<NetworkInterfaceMock>();
    auto externalState = std::make_unique<ReplicationCoordinatorExternalStateMock>();
    executor::ThreadPoolMock::Options tpOptions;
    tpOptions.onCreateThread = []() {
        Client::initThread("replexec",
                           getGlobalServiceContext()->getService(),
                           Client::noSession(),
                           ClientOperationKillableByStepdown{false});
    };
    auto pool = std::make_unique<executor::ThreadPoolMock>(net.get(), seed, tpOptions);
    auto replExec = executor::ThreadPoolTaskExecutor::create(std::move(pool), net);
    _repl = std::make_unique<ReplicationCoordinatorImpl>(service,
                                                         _settings,
                                                         std::move(externalState),
                                                         std::move(replExec),
                                                         std::move(topo),
                                                         replicationProcess,
                                                         _storageInterface,
                                                         seed);
    // Need to do this after the moves to make the static analyzer happy.
    // The pointer is stored as a ReplicationCoordinatorExternalState pointer, so we need to
    // reinterpret cast during retrieval.
    _externalState =
        dynamic_cast<ReplicationCoordinatorExternalStateMock*>(_repl->getExternalState_forTest());
    invariant(_externalState != nullptr);
    _replExec = _repl->getReplExecutor_forTest();
    invariant(_replExec != nullptr);
    _net = dynamic_cast<NetworkInterfaceMock*>(
        dynamic_cast<executor::ThreadPoolTaskExecutor*>(_replExec)->getNetworkInterface().get());
    invariant(_net != nullptr);
}

void ReplCoordTest::init(const ReplSettings& settings) {
    _settings = settings;
    init();
}

void ReplCoordTest::init(const std::string& replSet) {
    _settings.setReplSetString(replSet);
    init();
}

void ReplCoordTest::start() {
    // Skip reconstructing prepared transactions at the end of startup because ReplCoordTest doesn't
    // construct ServiceEntryPoint and this causes a segmentation fault when
    // reconstructPreparedTransactions uses DBDirectClient to call into ServiceEntryPoint.
    FailPointEnableBlock skipReconstructPreparedTransactions("skipReconstructPreparedTransactions");
    // Skip recovering user writes critical sections for the same reason as the above.
    FailPointEnableBlock skipRecoverUserWriteCriticalSections(
        "skipRecoverUserWriteCriticalSections");
    invariant(!_callShutdown);
    // if we haven't initialized yet, do that first.
    if (!_repl) {
        init();
    }

    const auto opCtx = makeOperationContext();
    _repl->startup(opCtx.get(), StorageEngine::LastShutdownState::kClean);
    _repl->waitForStartUpComplete_forTest();
    // _rsConfig should be written down at this point, so populate _memberData accordingly.
    _repl->getTopologyCoordinator_forTest()->populateAllMembersConfigVersionAndTerm_forTest();
    _callShutdown = true;
}

void ReplCoordTest::start(const BSONObj& configDoc, const HostAndPort& selfHost) {
    if (!_repl) {
        init();
    }
    _externalState->setLocalConfigDocument(StatusWith<BSONObj>(configDoc));
    _externalState->addSelf(selfHost);
    start();
}

void ReplCoordTest::start(const HostAndPort& selfHost) {
    if (!_repl) {
        init();
    }
    _externalState->addSelf(selfHost);
    start();
}

void ReplCoordTest::assertStartSuccessWithData(const BSONObj& configDoc,
                                               const HostAndPort& selfHost,
                                               const LastVote& lastVote,
                                               const OpTime& topOfOplog) {
    if (!_repl) {
        init();
    }

    _externalState->setLocalLastVoteDocument(lastVote);
    _externalState->setLastOpTimeAndWallTime(topOfOplog, Date_t() + Seconds(10));
    _externalState->setLocalConfigDocument(StatusWith<BSONObj>(configDoc));
    _externalState->addSelf(selfHost);
    getReplCoord()->setConsistentDataAvailable_forTest();
    start();
    ASSERT_NE(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

void ReplCoordTest::assertStartSuccess(const BSONObj& configDoc, const HostAndPort& selfHost) {
    // Set default protocol version to 1.
    if (!configDoc.hasField("protocolVersion")) {
        start(addProtocolVersion(configDoc, 1), selfHost);
    } else {
        start(configDoc, selfHost);
    }
    ASSERT_NE(MemberState::RS_STARTUP, getReplCoord()->getMemberState().s);
}

executor::RemoteCommandResponse ReplCoordTest::makeResponseStatus(const BSONObj& doc,
                                                                  Milliseconds millis) {
    LOGV2(21515,
          "Responding with {doc} (elapsed: {millis})",
          "doc"_attr = doc,
          "millis"_attr = millis);
    return RemoteCommandResponse::make_forTest(doc, millis);
}

void ReplCoordTest::simulateEnoughHeartbeatsForAllNodesUp() {
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ReplSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    for (int i = 0; i < rsConfig.getNumMembers() - 1; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(21516,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        ReplSetHeartbeatArgsV1 hbArgs;
        if (hbArgs.initialize(request.cmdObj).isOK()) {
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(rsConfig.getReplSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            hbResp.setConfigVersion(rsConfig.getConfigVersion());
            hbResp.setAppliedOpTimeAndWallTime(
                {OpTime(Timestamp(100, 2), 0), Date_t() + Seconds(100)});
            hbResp.setWrittenOpTimeAndWallTime(
                {OpTime(Timestamp(100, 2), 0), Date_t() + Seconds(100)});
            hbResp.setDurableOpTimeAndWallTime(
                {OpTime(Timestamp(100, 2), 0), Date_t() + Seconds(100)});
            BSONObjBuilder respObj;
            net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON()));
        } else {
            LOGV2_ERROR(21526,
                        "Black holing unexpected request to {request_target}: {request_cmdObj}",
                        "request_target"_attr = request.target,
                        "request_cmdObj"_attr = request.cmdObj);
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
}

void ReplCoordTest::simulateSuccessfulDryRun(
    std::function<void(const RemoteCommandRequest& request)> onDryRunRequest) {
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ReplSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    NetworkInterfaceMock* net = getNet();

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    LOGV2(21517,
          "Election timeout scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);

    int voteRequests = 0;
    int votesExpected = rsConfig.getNumMembers() / 2;
    LOGV2(21518,
          "Simulating dry run responses - expecting {votesExpected} replSetRequestVotes requests",
          "votesExpected"_attr = votesExpected);
    net->enterNetwork();
    while (voteRequests < votesExpected) {
        if (net->now() < electionTimeoutWhen) {
            net->runUntil(electionTimeoutWhen);
        }
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(21519,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        if (request.cmdObj.firstElement().fieldNameStringData() == "replSetRequestVotes") {
            ASSERT_TRUE(request.cmdObj.getBoolField("dryRun"));
            onDryRunRequest(request);
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(BSON("ok" << 1 << "reason"
                                             << ""
                                             << "term" << request.cmdObj["term"].Long()
                                             << "voteGranted" << true)));
            voteRequests++;
        } else if (consumeHeartbeatV1(noi)) {
            // The heartbeat has been consumed.
        } else {
            LOGV2_ERROR(21527,
                        "Black holing unexpected request to {request_target}: {request_cmdObj}",
                        "request_target"_attr = request.target,
                        "request_cmdObj"_attr = request.cmdObj);
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
    LOGV2(21520,
          "Simulating dry run responses - scheduled {voteRequests} replSetRequestVotes responses",
          "voteRequests"_attr = voteRequests);
    getReplCoord()->waitForElectionDryRunFinish_forTest();
    LOGV2(21521, "Simulating dry run responses - dry run completed");
}

void ReplCoordTest::simulateSuccessfulDryRun() {
    auto onDryRunRequest = [](const RemoteCommandRequest& request) {
    };
    simulateSuccessfulDryRun(onDryRunRequest);
}

void ReplCoordTest::simulateSuccessfulV1Election() {
    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    LOGV2(21522,
          "Election timeout scheduled at {electionTimeoutWhen} (simulator time)",
          "electionTimeoutWhen"_attr = electionTimeoutWhen);

    auto opCtx{makeOperationContext()};
    simulateSuccessfulV1ElectionAt(opCtx.get(), electionTimeoutWhen);
}

void ReplCoordTest::simulateSuccessfulV1ElectionWithoutExitingDrainMode(Date_t electionTime,
                                                                        OperationContext* opCtx) {
    RAIIServerParameterControllerForTest controller("featureFlagReduceMajorityWriteLatency", true);
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    NetworkInterfaceMock* net = getNet();

    ReplSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    ASSERT(replCoord->getMemberState().secondary()) << replCoord->getMemberState().toString();
    bool hasReadyRequests = true;
    // Process requests until we're primary and consume the heartbeats for the notification
    // of election win.
    while (!replCoord->getMemberState().primary() || hasReadyRequests) {
        LOGV2(21523,
              "Waiting on network in state {replCoord_getMemberState}",
              "replCoord_getMemberState"_attr = replCoord->getMemberState());
        getNet()->enterNetwork();
        if (net->now() < electionTime) {
            net->runUntil(electionTime);
        }
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        LOGV2(21524,
              "{request_target} processing {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        ReplSetHeartbeatArgsV1 hbArgs;
        Status status = hbArgs.initialize(request.cmdObj);
        if (status.isOK()) {
            if (replCoord->getMemberState().primary()) {
                ASSERT_EQ(hbArgs.getPrimaryId(), replCoord->getMyId());
            }
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(rsConfig.getReplSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            // The smallest valid optime in PV1.
            OpTime opTime(Timestamp(), 0);
            hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t() + Seconds(opTime.getSecs())});
            hbResp.setWrittenOpTimeAndWallTime({opTime, Date_t() + Seconds(opTime.getSecs())});
            hbResp.setDurableOpTimeAndWallTime({opTime, Date_t() + Seconds(opTime.getSecs())});
            hbResp.setConfigVersion(rsConfig.getConfigVersion());
            net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON()));
        } else if (request.cmdObj.firstElement().fieldNameStringData() == "replSetRequestVotes") {
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(BSON("ok" << 1 << "reason"
                                             << ""
                                             << "term" << request.cmdObj["term"].Long()
                                             << "voteGranted" << true)));
        } else {
            LOGV2_ERROR(21528,
                        "Black holing unexpected request to {request_target}: {request_cmdObj}",
                        "request_target"_attr = request.target,
                        "request_cmdObj"_attr = request.cmdObj);
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
        hasReadyRequests = net->hasReadyRequests();
        getNet()->exitNetwork();
    }
    ASSERT(replCoord->getOplogSyncState() ==
           ReplicationCoordinator::OplogSyncState::WriterDraining);
    ASSERT(replCoord->getMemberState().primary()) << replCoord->getMemberState().toString();

    auto helloResponse = replCoord->awaitHelloResponse(opCtx, {}, boost::none, boost::none);
    ASSERT_FALSE(helloResponse->isWritablePrimary()) << helloResponse->toBSON().toString();
    ASSERT_TRUE(helloResponse->isSecondary()) << helloResponse->toBSON().toString();
}

void ReplCoordTest::simulateSuccessfulV1ElectionAt(OperationContext* opCtx, Date_t electionTime) {
    simulateSuccessfulV1ElectionWithoutExitingDrainMode(electionTime, opCtx);
    ReplicationCoordinatorImpl* replCoord = getReplCoord();

    signalWriterDrainComplete(opCtx);
    signalApplierDrainComplete(opCtx);

    auto helloResponse = replCoord->awaitHelloResponse(opCtx, {}, boost::none, boost::none);
    ASSERT_TRUE(helloResponse->isWritablePrimary()) << helloResponse->toBSON().toString();
    ASSERT_FALSE(helloResponse->isSecondary()) << helloResponse->toBSON().toString();

    ASSERT(replCoord->getMemberState().primary()) << replCoord->getMemberState().toString();
}

void ReplCoordTest::signalWriterDrainComplete(OperationContext* opCtx) noexcept {
    getReplCoord()->signalWriterDrainComplete(opCtx, getReplCoord()->getTerm());
    ASSERT(getReplCoord()->getOplogSyncState() ==
           ReplicationCoordinator::OplogSyncState::ApplierDraining);
}

void ReplCoordTest::signalApplierDrainComplete(OperationContext* opCtx) noexcept {
    // Writes that occur in code paths that call signalApplierDrainComplete are expected to have
    // Immediate priority.
    ScopedAdmissionPriority<ExecutionAdmissionContext> priority(
        opCtx, AdmissionContext::Priority::kExempt);
    getExternalState()->setFirstOpTimeOfMyTerm(OpTime(Timestamp(1, 1), getReplCoord()->getTerm()));
    getReplCoord()->signalApplierDrainComplete(opCtx, getReplCoord()->getTerm());
    ASSERT(getReplCoord()->getOplogSyncState() == ReplicationCoordinator::OplogSyncState::Stopped);
}

void ReplCoordTest::runSingleNodeElection(OperationContext* opCtx) {
    RAIIServerParameterControllerForTest controller("featureFlagReduceMajorityWriteLatency", true);
    replCoordSetMyLastWrittenAndAppliedAndDurableOpTime(OpTime(Timestamp(1, 1), 0),
                                                        Date_t() + Seconds(1));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->waitForElectionFinish_forTest();

    ASSERT(getReplCoord()->getOplogSyncState() ==
           ReplicationCoordinator::OplogSyncState::WriterDraining);
    ASSERT(getReplCoord()->getMemberState().primary())
        << getReplCoord()->getMemberState().toString();

    signalWriterDrainComplete(opCtx);
    signalApplierDrainComplete(opCtx);
}

void ReplCoordTest::shutdown(OperationContext* opCtx) {
    invariant(_callShutdown);
    _callShutdown = false;

    if (!_repl->getSettings().isReplSet()) {
        return;
    }

    _net->exitNetwork();

    // ReplCoordinator shutdown shuts down the ThreadPoolTaskExecutor and joins it, but we need to
    // advance the NetworkInterfaceMock "thread" to complete cancellation tasks created during
    // shutdown. To achieve this, we shut down the coordinator in a separate thread. We use the fail
    // point as signal for when the main thread can consume networking tasks.
    boost::optional<FailPointEnableBlock> fp("tpteHangsBeforeDrainingCallbacks");
    auto shutdownThread =
        stdx::thread([&] { _repl->shutdown(opCtx, nullptr /* shutdownTimeElapsedBuilder */); });
    ON_BLOCK_EXIT([&] { shutdownThread.join(); });
    (*fp)->waitForTimesEntered(fp->initialTimesEntered() + 1);
    NetworkInterfaceMock::InNetworkGuard(getNet())->runReadyNetworkOperations();
    fp.reset();
    _callShutdown = false;
}

void ReplCoordTest::replyToReceivedHeartbeatV1() {
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    ASSERT(consumeHeartbeatV1(net->getNextReadyRequest()));
    net->runReadyNetworkOperations();
    getNet()->exitNetwork();
}

bool ReplCoordTest::consumeHeartbeatV1(const NetworkInterfaceMock::NetworkOperationIterator& noi) {
    auto net = getNet();
    auto& request = noi->getRequest();

    ReplSetHeartbeatArgsV1 args;
    if (!args.initialize(request.cmdObj).isOK())
        return false;

    OpTime lastApplied(Timestamp(100, 1), 0);
    ReplSetHeartbeatResponse hbResp;
    auto rsConfig = getReplCoord()->getReplicaSetConfig_forTest();
    hbResp.setSetName(rsConfig.getReplSetName());
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    hbResp.setConfigTerm(rsConfig.getConfigTerm());
    hbResp.setAppliedOpTimeAndWallTime({lastApplied, Date_t() + Seconds(lastApplied.getSecs())});
    hbResp.setWrittenOpTimeAndWallTime({lastApplied, Date_t() + Seconds(lastApplied.getSecs())});
    hbResp.setDurableOpTimeAndWallTime({lastApplied, Date_t() + Seconds(lastApplied.getSecs())});
    BSONObjBuilder respObj;
    net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON()));
    return true;
}

void ReplCoordTest::disableReadConcernMajoritySupport() {
    _externalState->setIsReadCommittedEnabled(false);
}

void ReplCoordTest::disableSnapshots() {
    _externalState->setAreSnapshotsEnabled(false);
}

void ReplCoordTest::simulateCatchUpAbort() {
    NetworkInterfaceMock* net = getNet();
    auto heartbeatTimeoutWhen =
        net->now() + getReplCoord()->getConfig().getHeartbeatTimeoutPeriodMillis();
    bool hasRequest = false;
    net->enterNetwork();
    if (net->now() < heartbeatTimeoutWhen) {
        net->runUntil(heartbeatTimeoutWhen);
    }
    hasRequest = net->hasReadyRequests();
    while (hasRequest) {
        auto noi = net->getNextReadyRequest();
        auto request = noi->getRequest();
        // Black hole heartbeat requests caused by time advance.
        LOGV2(21525,
              "Black holing request to {request_target} : {request_cmdObj}",
              "request_target"_attr = request.target.toString(),
              "request_cmdObj"_attr = request.cmdObj);
        net->blackHole(noi);
        if (net->now() < heartbeatTimeoutWhen) {
            net->runUntil(heartbeatTimeoutWhen);
        } else {
            net->runReadyNetworkOperations();
        }
        hasRequest = net->hasReadyRequests();
    }
    net->exitNetwork();
}

}  // namespace repl
}  // namespace mongo
