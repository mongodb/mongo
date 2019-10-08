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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/replication_coordinator_test_fixture.h"

#include <functional>
#include <memory>

#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using ApplierState = OplogApplier::ApplierState;

executor::TaskExecutor* ReplCoordTest::getReplExec() {
    return _replExec;
}

ReplSetConfig ReplCoordTest::assertMakeRSConfig(const BSONObj& configBson) {
    ReplSetConfig config;
    ASSERT_OK(config.initialize(configBson));
    ASSERT_OK(config.validate());
    return config;
}

BSONObj ReplCoordTest::addProtocolVersion(const BSONObj& configDoc, int protocolVersion) {
    BSONObjBuilder builder;
    builder << "protocolVersion" << protocolVersion;
    builder.appendElementsUnique(configDoc);
    return builder.obj();
}

ReplCoordTest::ReplCoordTest() {
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

    auto service = getGlobalServiceContext();
    _storageInterface = new StorageInterfaceMock();
    StorageInterface::set(service, std::unique_ptr<StorageInterface>(_storageInterface));
    ASSERT_TRUE(_storageInterface == StorageInterface::get(service));

    ReplicationProcess::set(
        service,
        std::make_unique<ReplicationProcess>(_storageInterface,
                                             std::make_unique<ReplicationConsistencyMarkersMock>(),
                                             std::make_unique<ReplicationRecoveryMock>()));
    auto replicationProcess = ReplicationProcess::get(service);

    // PRNG seed for tests.
    const int64_t seed = 0;

    auto logicalClock = std::make_unique<LogicalClock>(service);
    LogicalClock::set(service, std::move(logicalClock));

    TopologyCoordinator::Options settings;
    auto topo = std::make_unique<TopologyCoordinator>(settings);
    _topo = topo.get();
    auto net = std::make_unique<NetworkInterfaceMock>();
    _net = net.get();
    auto externalState = std::make_unique<ReplicationCoordinatorExternalStateMock>();
    _externalState = externalState.get();
    executor::ThreadPoolMock::Options tpOptions;
    tpOptions.onCreateThread = []() { Client::initThread("replexec"); };
    auto pool = std::make_unique<executor::ThreadPoolMock>(_net, seed, tpOptions);
    auto replExec =
        std::make_unique<executor::ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
    _replExec = replExec.get();
    _repl = std::make_unique<ReplicationCoordinatorImpl>(service,
                                                         _settings,
                                                         std::move(externalState),
                                                         std::move(replExec),
                                                         std::move(topo),
                                                         replicationProcess,
                                                         _storageInterface,
                                                         seed);
    service->setFastClockSource(std::make_unique<executor::NetworkInterfaceMockClockSource>(_net));
    service->setPreciseClockSource(
        std::make_unique<executor::NetworkInterfaceMockClockSource>(_net));
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
    invariant(!_callShutdown);
    // if we haven't initialized yet, do that first.
    if (!_repl) {
        init();
    }

    const auto opCtx = makeOperationContext();
    _repl->startup(opCtx.get());
    _repl->waitForStartUpComplete_forTest();
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
    log() << "Responding with " << doc << " (elapsed: " << millis << ")";
    return RemoteCommandResponse(doc, millis);
}

void ReplCoordTest::simulateEnoughHeartbeatsForAllNodesUp() {
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ReplSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    for (int i = 0; i < rsConfig.getNumMembers() - 1; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        ReplSetHeartbeatArgsV1 hbArgs;
        if (hbArgs.initialize(request.cmdObj).isOK()) {
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(rsConfig.getReplSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            hbResp.setConfigVersion(rsConfig.getConfigVersion());
            hbResp.setAppliedOpTimeAndWallTime(
                {OpTime(Timestamp(100, 2), 0), Date_t() + Seconds(100)});
            hbResp.setDurableOpTimeAndWallTime(
                {OpTime(Timestamp(100, 2), 0), Date_t() + Seconds(100)});
            BSONObjBuilder respObj;
            net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON()));
        } else {
            error() << "Black holing unexpected request to " << request.target << ": "
                    << request.cmdObj;
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
    log() << "Election timeout scheduled at " << electionTimeoutWhen << " (simulator time)";

    int voteRequests = 0;
    int votesExpected = rsConfig.getNumMembers() / 2;
    log() << "Simulating dry run responses - expecting " << votesExpected
          << " replSetRequestVotes requests";
    net->enterNetwork();
    while (voteRequests < votesExpected) {
        if (net->now() < electionTimeoutWhen) {
            net->runUntil(electionTimeoutWhen);
        }
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
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
            error() << "Black holing unexpected request to " << request.target << ": "
                    << request.cmdObj;
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
    }
    net->exitNetwork();
    log() << "Simulating dry run responses - scheduled " << voteRequests
          << " replSetRequestVotes responses";
    getReplCoord()->waitForElectionDryRunFinish_forTest();
    log() << "Simulating dry run responses - dry run completed";
}

void ReplCoordTest::simulateSuccessfulDryRun() {
    auto onDryRunRequest = [](const RemoteCommandRequest& request) {};
    simulateSuccessfulDryRun(onDryRunRequest);
}

void ReplCoordTest::simulateSuccessfulV1Election() {
    auto electionTimeoutWhen = getReplCoord()->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    log() << "Election timeout scheduled at " << electionTimeoutWhen << " (simulator time)";

    simulateSuccessfulV1ElectionAt(electionTimeoutWhen);
}

void ReplCoordTest::simulateSuccessfulV1ElectionWithoutExitingDrainMode(Date_t electionTime) {
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    NetworkInterfaceMock* net = getNet();

    ReplSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    ASSERT(replCoord->getMemberState().secondary()) << replCoord->getMemberState().toString();
    bool hasReadyRequests = true;
    // Process requests until we're primary and consume the heartbeats for the notification
    // of election win.
    while (!replCoord->getMemberState().primary() || hasReadyRequests) {
        log() << "Waiting on network in state " << replCoord->getMemberState();
        getNet()->enterNetwork();
        if (net->now() < electionTime) {
            net->runUntil(electionTime);
        }
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        ReplSetHeartbeatArgsV1 hbArgs;
        Status status = hbArgs.initialize(request.cmdObj);
        if (hbArgs.initialize(request.cmdObj).isOK()) {
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(rsConfig.getReplSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            // The smallest valid optime in PV1.
            OpTime opTime(Timestamp(), 0);
            hbResp.setAppliedOpTimeAndWallTime({opTime, Date_t() + Seconds(opTime.getSecs())});
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
            error() << "Black holing unexpected request to " << request.target << ": "
                    << request.cmdObj;
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
        hasReadyRequests = net->hasReadyRequests();
        getNet()->exitNetwork();
    }
    ASSERT(getExternalState()->getApplierState() == ApplierState::Draining);
    ASSERT(replCoord->getMemberState().primary()) << replCoord->getMemberState().toString();

    IsMasterResponse imResponse;
    replCoord->fillIsMasterForReplSet(&imResponse, {});
    ASSERT_FALSE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_TRUE(imResponse.isSecondary()) << imResponse.toBSON().toString();
}

void ReplCoordTest::simulateSuccessfulV1ElectionAt(Date_t electionTime) {
    simulateSuccessfulV1ElectionWithoutExitingDrainMode(electionTime);
    ReplicationCoordinatorImpl* replCoord = getReplCoord();

    {
        auto opCtx = makeOperationContext();
        signalDrainComplete(opCtx.get());
    }
    ASSERT(getExternalState()->getApplierState() == ApplierState::Stopped);
    IsMasterResponse imResponse;
    replCoord->fillIsMasterForReplSet(&imResponse, {});
    ASSERT_TRUE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_FALSE(imResponse.isSecondary()) << imResponse.toBSON().toString();

    ASSERT(replCoord->getMemberState().primary()) << replCoord->getMemberState().toString();
}

void ReplCoordTest::signalDrainComplete(OperationContext* opCtx) {
    // Writes that occur in code paths that call signalDrainComplete are expected to be excluded
    // from Flow Control.
    opCtx->setShouldParticipateInFlowControl(false);
    getExternalState()->setFirstOpTimeOfMyTerm(OpTime(Timestamp(1, 1), getReplCoord()->getTerm()));
    getReplCoord()->signalDrainComplete(opCtx, getReplCoord()->getTerm());
}

void ReplCoordTest::runSingleNodeElection(OperationContext* opCtx) {
    replCoordSetMyLastAppliedOpTime(OpTime(Timestamp(1, 1), 0), Date_t() + Seconds(1));
    replCoordSetMyLastDurableOpTime(OpTime(Timestamp(1, 1), 0), Date_t() + Seconds(1));
    ASSERT_OK(getReplCoord()->setFollowerMode(MemberState::RS_SECONDARY));
    getReplCoord()->waitForElectionFinish_forTest();

    ASSERT(getExternalState()->getApplierState() == ApplierState::Draining);
    ASSERT(getReplCoord()->getMemberState().primary())
        << getReplCoord()->getMemberState().toString();

    signalDrainComplete(opCtx);
}

void ReplCoordTest::shutdown(OperationContext* opCtx) {
    invariant(_callShutdown);
    _net->exitNetwork();
    _repl->shutdown(opCtx);
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
    hbResp.setAppliedOpTimeAndWallTime({lastApplied, Date_t() + Seconds(lastApplied.getSecs())});
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
        log() << "Black holing request to " << request.target.toString() << " : " << request.cmdObj;
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
