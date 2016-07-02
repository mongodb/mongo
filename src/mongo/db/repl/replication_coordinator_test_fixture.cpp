/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

ReplicaSetConfig ReplCoordTest::assertMakeRSConfig(const BSONObj& configBson) {
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(configBson));
    ASSERT_OK(config.validate());
    return config;
}

ReplicaSetConfig ReplCoordTest::assertMakeRSConfigV0(const BSONObj& configBson) {
    return assertMakeRSConfig(addProtocolVersion(configBson, 0));
}

BSONObj ReplCoordTest::addProtocolVersion(const BSONObj& configDoc, int protocolVersion) {
    BSONObjBuilder builder;
    builder << "protocolVersion" << protocolVersion;
    builder.appendElementsUnique(configDoc);
    return builder.obj();
}


void ReplCoordTest::setUp() {
    _settings.setReplSetString("mySet/node1:12345,node2:54321");
    _settings.setMajorityReadConcernEnabled(true);
}

void ReplCoordTest::tearDown() {
    if (_externalState) {
        _externalState->setStoreLocalConfigDocumentToHang(false);
    }
    if (_callShutdown) {
        auto txn = makeOperationContext();
        shutdown(txn.get());
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

    auto serviceContext = getGlobalServiceContext();
    StorageInterface* storageInterface = new StorageInterfaceMock();
    StorageInterface::set(serviceContext, std::unique_ptr<StorageInterface>(storageInterface));
    ASSERT_TRUE(storageInterface == StorageInterface::get(serviceContext));
    // PRNG seed for tests.
    const int64_t seed = 0;

    TopologyCoordinatorImpl::Options settings;
    _topo = new TopologyCoordinatorImpl(settings);
    stdx::function<bool()> _durablityLambda = [this]() -> bool { return _isStorageEngineDurable; };
    _net = new NetworkInterfaceMock;
    _replExec = stdx::make_unique<ReplicationExecutor>(_net, seed);
    _externalState = new ReplicationCoordinatorExternalStateMock;
    _repl.reset(new ReplicationCoordinatorImpl(_settings,
                                               _externalState,
                                               _topo,
                                               storageInterface,
                                               _replExec.get(),
                                               seed,
                                               &_durablityLambda));
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
    invariant(!_callShutdown);
    // if we haven't initialized yet, do that first.
    if (!_repl) {
        init();
    }

    const auto txn = makeOperationContext();
    _repl->startup(txn.get());
    _repl->waitForStartUpComplete();
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

ResponseStatus ReplCoordTest::makeResponseStatus(const BSONObj& doc, Milliseconds millis) {
    return makeResponseStatus(doc, BSONObj(), millis);
}

ResponseStatus ReplCoordTest::makeResponseStatus(const BSONObj& doc,
                                                 const BSONObj& metadata,
                                                 Milliseconds millis) {
    log() << "Responding with " << doc << " (metadata: " << metadata << "; elapsed: " << millis
          << ")";
    return ResponseStatus(RemoteCommandResponse(doc, metadata, millis));
}

void ReplCoordTest::simulateEnoughHeartbeatsForAllNodesUp() {
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ReplicaSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    for (int i = 0; i < rsConfig.getNumMembers() - 1; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        ReplSetHeartbeatArgsV1 hbArgs;
        ReplSetHeartbeatArgs hbArgsPV0;
        if (hbArgs.initialize(request.cmdObj).isOK() ||
            hbArgsPV0.initialize(request.cmdObj).isOK()) {
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(rsConfig.getReplSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            hbResp.setConfigVersion(rsConfig.getConfigVersion());
            hbResp.setAppliedOpTime(OpTime(Timestamp(100, 2), 0));
            BSONObjBuilder respObj;
            net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON(true)));
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
    stdx::function<void(const RemoteCommandRequest& request)> onDryRunRequest) {
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    ReplicaSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
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
            net->scheduleResponse(noi,
                                  net->now(),
                                  makeResponseStatus(BSON("ok" << 1 << "reason"
                                                               << ""
                                                               << "term"
                                                               << request.cmdObj["term"].Long()
                                                               << "voteGranted"
                                                               << true)));
            voteRequests++;
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
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    NetworkInterfaceMock* net = getNet();

    auto electionTimeoutWhen = replCoord->getElectionTimeout_forTest();
    ASSERT_NOT_EQUALS(Date_t(), electionTimeoutWhen);
    log() << "Election timeout scheduled at " << electionTimeoutWhen << " (simulator time)";

    ReplicaSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    ASSERT(replCoord->getMemberState().secondary()) << replCoord->getMemberState().toString();
    bool hasReadyRequests = true;
    // Process requests until we're primary and consume the heartbeats for the notification
    // of election win.
    while (!replCoord->getMemberState().primary() || hasReadyRequests) {
        log() << "Waiting on network in state " << replCoord->getMemberState();
        getNet()->enterNetwork();
        if (net->now() < electionTimeoutWhen) {
            net->runUntil(electionTimeoutWhen);
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
            hbResp.setConfigVersion(rsConfig.getConfigVersion());
            net->scheduleResponse(noi, net->now(), makeResponseStatus(hbResp.toBSON(true)));
        } else if (request.cmdObj.firstElement().fieldNameStringData() == "replSetRequestVotes") {
            net->scheduleResponse(noi,
                                  net->now(),
                                  makeResponseStatus(BSON("ok" << 1 << "reason"
                                                               << ""
                                                               << "term"
                                                               << request.cmdObj["term"].Long()
                                                               << "voteGranted"
                                                               << true)));
        } else {
            error() << "Black holing unexpected request to " << request.target << ": "
                    << request.cmdObj;
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
        hasReadyRequests = net->hasReadyRequests();
        getNet()->exitNetwork();
    }
    ASSERT(replCoord->isWaitingForApplierToDrain());
    ASSERT(replCoord->getMemberState().primary()) << replCoord->getMemberState().toString();

    IsMasterResponse imResponse;
    replCoord->fillIsMasterForReplSet(&imResponse);
    ASSERT_FALSE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_TRUE(imResponse.isSecondary()) << imResponse.toBSON().toString();
    {
        auto txn = makeOperationContext();
        replCoord->signalDrainComplete(txn.get());
    }
    replCoord->fillIsMasterForReplSet(&imResponse);
    ASSERT_TRUE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_FALSE(imResponse.isSecondary()) << imResponse.toBSON().toString();

    ASSERT(replCoord->getMemberState().primary()) << replCoord->getMemberState().toString();
}

void ReplCoordTest::simulateSuccessfulElection() {
    ReplicationCoordinatorImpl* replCoord = getReplCoord();
    NetworkInterfaceMock* net = getNet();
    ReplicaSetConfig rsConfig = replCoord->getReplicaSetConfig_forTest();
    ASSERT(replCoord->getMemberState().secondary()) << replCoord->getMemberState().toString();
    bool hasReadyRequests = true;
    // Process requests until we're primary and consume the heartbeats for the notification
    // of election win.
    while (!replCoord->getMemberState().primary() || hasReadyRequests) {
        log() << "Waiting on network in state " << replCoord->getMemberState();
        getNet()->enterNetwork();
        const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        log() << request.target.toString() << " processing " << request.cmdObj;
        ReplSetHeartbeatArgs hbArgs;
        if (hbArgs.initialize(request.cmdObj).isOK()) {
            ReplSetHeartbeatResponse hbResp;
            hbResp.setSetName(rsConfig.getReplSetName());
            hbResp.setState(MemberState::RS_SECONDARY);
            hbResp.setConfigVersion(rsConfig.getConfigVersion());
            BSONObjBuilder respObj;
            respObj << "ok" << 1;
            hbResp.addToBSON(&respObj, false);
            net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
        } else if (request.cmdObj.firstElement().fieldNameStringData() == "replSetFresh") {
            net->scheduleResponse(
                noi,
                net->now(),
                makeResponseStatus(BSON(
                    "ok" << 1 << "fresher" << false << "opTime" << Date_t() << "veto" << false)));
        } else if (request.cmdObj.firstElement().fieldNameStringData() == "replSetElect") {
            net->scheduleResponse(noi,
                                  net->now(),
                                  makeResponseStatus(BSON("ok" << 1 << "vote" << 1 << "round"
                                                               << request.cmdObj["round"].OID())));
        } else {
            error() << "Black holing unexpected request to " << request.target << ": "
                    << request.cmdObj;
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
        hasReadyRequests = net->hasReadyRequests();
        getNet()->exitNetwork();
    }
    ASSERT(replCoord->isWaitingForApplierToDrain());
    ASSERT(replCoord->getMemberState().primary()) << replCoord->getMemberState().toString();

    IsMasterResponse imResponse;
    replCoord->fillIsMasterForReplSet(&imResponse);
    ASSERT_FALSE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_TRUE(imResponse.isSecondary()) << imResponse.toBSON().toString();
    {
        auto txn = makeOperationContext();
        replCoord->signalDrainComplete(txn.get());
    }
    replCoord->fillIsMasterForReplSet(&imResponse);
    ASSERT_TRUE(imResponse.isMaster()) << imResponse.toBSON().toString();
    ASSERT_FALSE(imResponse.isSecondary()) << imResponse.toBSON().toString();

    ASSERT(replCoord->getMemberState().primary()) << replCoord->getMemberState().toString();
}

void ReplCoordTest::shutdown(OperationContext* txn) {
    invariant(_callShutdown);
    _net->exitNetwork();
    _repl->shutdown(txn);
    _callShutdown = false;
}

void ReplCoordTest::replyToReceivedHeartbeat() {
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    const ReplicaSetConfig rsConfig = getReplCoord()->getReplicaSetConfig_forTest();
    repl::ReplSetHeartbeatArgs hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    repl::ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName(rsConfig.getReplSetName());
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    BSONObjBuilder respObj;
    respObj << "ok" << 1;
    hbResp.addToBSON(&respObj, false);
    net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
    net->runReadyNetworkOperations();
    getNet()->exitNetwork();
}

void ReplCoordTest::replyToReceivedHeartbeatV1() {
    NetworkInterfaceMock* net = getNet();
    net->enterNetwork();
    const NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const RemoteCommandRequest& request = noi->getRequest();
    const ReplicaSetConfig rsConfig = getReplCoord()->getReplicaSetConfig_forTest();
    repl::ReplSetHeartbeatArgsV1 hbArgs;
    ASSERT_OK(hbArgs.initialize(request.cmdObj));
    repl::ReplSetHeartbeatResponse hbResp;
    hbResp.setSetName(rsConfig.getReplSetName());
    hbResp.setState(MemberState::RS_SECONDARY);
    hbResp.setConfigVersion(rsConfig.getConfigVersion());
    BSONObjBuilder respObj;
    respObj << "ok" << 1;
    hbResp.addToBSON(&respObj, false);
    net->scheduleResponse(noi, net->now(), makeResponseStatus(respObj.obj()));
    net->runReadyNetworkOperations();
    getNet()->exitNetwork();
}

void ReplCoordTest::disableReadConcernMajoritySupport() {
    _externalState->setIsReadCommittedEnabled(false);
}

void ReplCoordTest::disableSnapshots() {
    _externalState->setAreSnapshotsEnabled(false);
}

}  // namespace repl
}  // namespace mongo
