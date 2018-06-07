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

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/check_quorum_for_config_change.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

#define ASSERT_REASON_CONTAINS(STATUS, PATTERN)                                                 \
    do {                                                                                        \
        const mongo::Status s_ = (STATUS);                                                      \
        ASSERT_FALSE(s_.reason().find(PATTERN) == std::string::npos) << #STATUS ".reason() == " \
                                                                     << s_.reason();            \
    } while (false)

#define ASSERT_NOT_REASON_CONTAINS(STATUS, PATTERN)                                            \
    do {                                                                                       \
        const mongo::Status s_ = (STATUS);                                                     \
        ASSERT_TRUE(s_.reason().find(PATTERN) == std::string::npos) << #STATUS ".reason() == " \
                                                                    << s_.reason();            \
    } while (false)

namespace mongo {
namespace repl {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

class CheckQuorumTest : public executor::ThreadPoolExecutorTest {
protected:
    CheckQuorumTest();

    void startQuorumCheck(const ReplSetConfig& config, int myIndex);
    Status waitForQuorumCheck();
    bool isQuorumCheckDone();

private:
    void setUp() {
        executor::ThreadPoolExecutorTest::setUp();
        launchExecutorThread();
    }
    void _runQuorumCheck(const ReplSetConfig& config, int myIndex);
    virtual Status _runQuorumCheckImpl(const ReplSetConfig& config, int myIndex) = 0;

    std::unique_ptr<stdx::thread> _quorumCheckThread;
    Status _quorumCheckStatus;
    stdx::mutex _mutex;
    bool _isQuorumCheckDone;
};

CheckQuorumTest::CheckQuorumTest()
    : _quorumCheckStatus(ErrorCodes::InternalError, "Not executed") {}

void CheckQuorumTest::startQuorumCheck(const ReplSetConfig& config, int myIndex) {
    ASSERT_FALSE(_quorumCheckThread);
    _isQuorumCheckDone = false;
    _quorumCheckThread.reset(
        new stdx::thread(stdx::bind(&CheckQuorumTest::_runQuorumCheck, this, config, myIndex)));
}

Status CheckQuorumTest::waitForQuorumCheck() {
    ASSERT_TRUE(_quorumCheckThread);
    _quorumCheckThread->join();
    return _quorumCheckStatus;
}

bool CheckQuorumTest::isQuorumCheckDone() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _isQuorumCheckDone;
}

void CheckQuorumTest::_runQuorumCheck(const ReplSetConfig& config, int myIndex) {
    _quorumCheckStatus = _runQuorumCheckImpl(config, myIndex);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _isQuorumCheckDone = true;
}

class CheckQuorumForInitiate : public CheckQuorumTest {
private:
    virtual Status _runQuorumCheckImpl(const ReplSetConfig& config, int myIndex) {
        return checkQuorumForInitiate(&getExecutor(), config, myIndex, 0);
    }
};

class CheckQuorumForReconfig : public CheckQuorumTest {
protected:
    virtual Status _runQuorumCheckImpl(const ReplSetConfig& config, int myIndex) {
        return checkQuorumForReconfig(&getExecutor(), config, myIndex, 0);
    }
};

ReplSetConfig assertMakeRSConfig(const BSONObj& configBson) {
    ReplSetConfig config;
    ASSERT_OK(config.initialize(configBson));
    ASSERT_OK(config.validate());
    return config;
}

TEST_F(CheckQuorumForInitiate, ValidSingleNodeSet) {
    ReplSetConfig config = assertMakeRSConfig(BSON("_id"
                                                   << "rs0"
                                                   << "version"
                                                   << 1
                                                   << "members"
                                                   << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                            << "h1"))));
    startQuorumCheck(config, 0);
    ASSERT_OK(waitForQuorumCheck());
}

TEST_F(CheckQuorumForInitiate, QuorumCheckCanceledByShutdown) {
    getExecutor().shutdown();
    ReplSetConfig config = assertMakeRSConfig(BSON("_id"
                                                   << "rs0"
                                                   << "version"
                                                   << 1
                                                   << "members"
                                                   << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                            << "h1"))));
    startQuorumCheck(config, 0);
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, waitForQuorumCheck());
}

TEST_F(CheckQuorumForInitiate, QuorumCheckFailedDueToSeveralDownNodes) {
    // In this test, "we" are host "h3:1".  All other nodes time out on
    // their heartbeat request, and so the quorum check for initiate
    // will fail because some members were unavailable.
    ReplSetConfig config = assertMakeRSConfig(BSON("_id"
                                                   << "rs0"
                                                   << "version"
                                                   << 1
                                                   << "members"
                                                   << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                                            << "h1:1")
                                                                 << BSON("_id" << 2 << "host"
                                                                               << "h2:1")
                                                                 << BSON("_id" << 3 << "host"
                                                                               << "h3:1")
                                                                 << BSON("_id" << 4 << "host"
                                                                               << "h4:1")
                                                                 << BSON("_id" << 5 << "host"
                                                                               << "h5:1"))));
    startQuorumCheck(config, 2);
    getNet()->enterNetwork();
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = config.getNumMembers() - 1;
    for (int i = 0; i < numCommandsExpected; ++i) {
        getNet()->scheduleResponse(getNet()->getNextReadyRequest(),
                                   startDate + Milliseconds(10),
                                   {ErrorCodes::NoSuchKey, "No reply"});
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_EQUALS(startDate + Milliseconds(10), getNet()->now());
    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
    ASSERT_REASON_CONTAINS(
        status, "replSetInitiate quorum check failed because not all proposed set members");
    ASSERT_REASON_CONTAINS(status, "h1:1");
    ASSERT_REASON_CONTAINS(status, "h2:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
    ASSERT_REASON_CONTAINS(status, "h4:1");
    ASSERT_REASON_CONTAINS(status, "h5:1");
}

const BSONObj makeHeartbeatRequest(const ReplSetConfig& rsConfig, int myConfigIndex) {
    const MemberConfig& myConfig = rsConfig.getMemberAt(myConfigIndex);
    ReplSetHeartbeatArgsV1 hbArgs;
    hbArgs.setSetName(rsConfig.getReplSetName());
    hbArgs.setConfigVersion(rsConfig.getConfigVersion());
    if (rsConfig.getConfigVersion() == 1) {
        hbArgs.setCheckEmpty();
    }
    hbArgs.setSenderHost(myConfig.getHostAndPort());
    hbArgs.setSenderId(myConfig.getId());
    hbArgs.setTerm(0);
    return hbArgs.toBSON();
}

TEST_F(CheckQuorumForInitiate, QuorumCheckSuccessForFiveNodes) {
    // In this test, "we" are host "h3:1".  All nodes respond successfully to their heartbeat
    // requests, and the quorum check succeeds.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 1
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1")
                                              << BSON("_id" << 4 << "host"
                                                            << "h4:1")
                                              << BSON("_id" << 5 << "host"
                                                            << "h5:1"))));
    const int myConfigIndex = 2;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        getNet()->scheduleResponse(
            noi,
            startDate + Milliseconds(10),
            (RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(8))));
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_OK(waitForQuorumCheck());
}

TEST_F(CheckQuorumForInitiate, QuorumCheckFailedDueToOneDownNode) {
    // In this test, "we" are host "h3:1".  All nodes except "h2:1" respond
    // successfully to their heartbeat requests, but quorum check fails because
    // all nodes must be available for initiate.  This is so even though "h2"
    // is neither voting nor electable.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 1
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1"
                                                            << "priority"
                                                            << 0
                                                            << "votes"
                                                            << 0)
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1")
                                              << BSON("_id" << 4 << "host"
                                                            << "h4:1")
                                              << BSON("_id" << 5 << "host"
                                                            << "h5:1")
                                              << BSON("_id" << 6 << "host"
                                                            << "h6:1"))));
    const int myConfigIndex = 2;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        if (request.target == HostAndPort("h2", 1)) {
            getNet()->scheduleResponse(
                noi, startDate + Milliseconds(10), {ErrorCodes::NoSuchKey, "No response"});
        } else {
            getNet()->scheduleResponse(
                noi,
                startDate + Milliseconds(10),
                (RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(8))));
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
    ASSERT_REASON_CONTAINS(
        status, "replSetInitiate quorum check failed because not all proposed set members");
    ASSERT_NOT_REASON_CONTAINS(status, "h1:1");
    ASSERT_REASON_CONTAINS(status, "h2:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h4:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h5:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h6:1");
}

TEST_F(CheckQuorumForInitiate, QuorumCheckFailedDueToSetNameMismatch) {
    // In this test, "we" are host "h3:1".  All nodes respond
    // successfully to their heartbeat requests, but quorum check fails because
    // "h4" declares that the requested replica set name was not what it expected.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 1
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1")
                                              << BSON("_id" << 4 << "host"
                                                            << "h4:1")
                                              << BSON("_id" << 5 << "host"
                                                            << "h5:1"))));
    const int myConfigIndex = 2;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        if (request.target == HostAndPort("h4", 1)) {
            getNet()->scheduleResponse(noi,
                                       startDate + Milliseconds(10),
                                       (RemoteCommandResponse(BSON("ok" << 0 << "mismatch" << true),
                                                              BSONObj(),
                                                              Milliseconds(8))));
        } else {
            getNet()->scheduleResponse(
                noi,
                startDate + Milliseconds(10),
                (RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(8))));
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
    ASSERT_REASON_CONTAINS(status, "Our set name did not match");
    ASSERT_NOT_REASON_CONTAINS(status, "h1:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h2:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
    ASSERT_REASON_CONTAINS(status, "h4:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h5:1");
}

TEST_F(CheckQuorumForInitiate, QuorumCheckFailedDueToSetIdMismatch) {
    // In this test, "we" are host "h3:1".  All nodes respond
    // successfully to their heartbeat requests, but quorum check fails because
    // "h4" declares that the requested replica set ID was not what it expected.

    const auto replicaSetId = OID::gen();
    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 1
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1")
                                              << BSON("_id" << 4 << "host"
                                                            << "h4:1")
                                              << BSON("_id" << 5 << "host"
                                                            << "h5:1"))
                                << "settings"
                                << BSON("replicaSetId" << replicaSetId)));
    const int myConfigIndex = 2;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    HostAndPort incompatibleHost("h4", 1);
    OID unexpectedId = OID::gen();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT_BSONOBJ_EQ(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        if (request.target == incompatibleHost) {
            OpTime opTime{Timestamp{10, 10}, 10};
            rpc::ReplSetMetadata metadata(opTime.getTerm(),
                                          opTime,
                                          opTime,
                                          rsConfig.getConfigVersion(),
                                          unexpectedId,
                                          rpc::ReplSetMetadata::kNoPrimary,
                                          -1);
            BSONObjBuilder metadataBuilder;
            metadata.writeToMetadata(&metadataBuilder).transitional_ignore();

            getNet()->scheduleResponse(
                noi,
                startDate + Milliseconds(10),
                (RemoteCommandResponse(BSON("ok" << 1), metadataBuilder.obj(), Milliseconds(8))));
        } else {
            getNet()->scheduleResponse(
                noi,
                startDate + Milliseconds(10),
                (RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(8))));
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
    ASSERT_REASON_CONTAINS(status,
                           str::stream() << "Our replica set ID of " << replicaSetId
                                         << " did not match that of "
                                         << incompatibleHost.toString()
                                         << ", which is "
                                         << unexpectedId);
    ASSERT_NOT_REASON_CONTAINS(status, "h1:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h2:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
    ASSERT_REASON_CONTAINS(status, "h4:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h5:1");
}

TEST_F(CheckQuorumForInitiate, QuorumCheckFailedDueToInitializedNode) {
    // In this test, "we" are host "h3:1".  All nodes respond
    // successfully to their heartbeat requests, but quorum check fails because
    // "h5" declares that it is already initialized.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 1
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1")
                                              << BSON("_id" << 4 << "host"
                                                            << "h4:1")
                                              << BSON("_id" << 5 << "host"
                                                            << "h5:1"))));
    const int myConfigIndex = 2;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        if (request.target == HostAndPort("h5", 1)) {
            getNet()->scheduleResponse(noi,
                                       startDate + Milliseconds(10),
                                       (RemoteCommandResponse(BSON("ok" << 0 << "set"
                                                                        << "rs0"
                                                                        << "v"
                                                                        << 1),
                                                              BSONObj(),
                                                              Milliseconds(8))));
        } else {
            getNet()->scheduleResponse(
                noi,
                startDate + Milliseconds(10),
                (RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(8))));
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
    ASSERT_REASON_CONTAINS(status, "Our config version of");
    ASSERT_REASON_CONTAINS(status, "is no larger than the version");
    ASSERT_NOT_REASON_CONTAINS(status, "h1:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h2:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h4:1");
    ASSERT_REASON_CONTAINS(status, "h5:1");
}

TEST_F(CheckQuorumForInitiate, QuorumCheckFailedDueToInitializedNodeOnlyOneRespondent) {
    // In this test, "we" are host "h3:1".  Only node "h5" responds before the test completes,
    // and quorum check fails because "h5" declares that it is already initialized.
    //
    // Compare to QuorumCheckFailedDueToInitializedNode, above.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 1
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1")
                                              << BSON("_id" << 4 << "host"
                                                            << "h4:1")
                                              << BSON("_id" << 5 << "host"
                                                            << "h5:1"))));
    const int myConfigIndex = 2;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        if (request.target == HostAndPort("h5", 1)) {
            getNet()->scheduleResponse(noi,
                                       startDate + Milliseconds(10),
                                       (RemoteCommandResponse(BSON("ok" << 0 << "set"
                                                                        << "rs0"
                                                                        << "v"
                                                                        << 1),
                                                              BSONObj(),
                                                              Milliseconds(8))));
        } else {
            getNet()->blackHole(noi);
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
    ASSERT_REASON_CONTAINS(status, "Our config version of");
    ASSERT_REASON_CONTAINS(status, "is no larger than the version");
    ASSERT_NOT_REASON_CONTAINS(status, "h1:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h2:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h4:1");
    ASSERT_REASON_CONTAINS(status, "h5:1");
}

TEST_F(CheckQuorumForInitiate, QuorumCheckFailedDueToNodeWithData) {
    // In this test, "we" are host "h3:1".  Only node "h5" responds before the test completes,
    // and quorum check fails because "h5" declares that it has data already.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 1
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1")
                                              << BSON("_id" << 4 << "host"
                                                            << "h4:1")
                                              << BSON("_id" << 5 << "host"
                                                            << "h5:1"))));
    const int myConfigIndex = 2;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        ReplSetHeartbeatResponse hbResp;
        hbResp.setConfigVersion(0);
        hbResp.noteHasData();
        if (request.target == HostAndPort("h5", 1)) {
            getNet()->scheduleResponse(
                noi,
                startDate + Milliseconds(10),
                (RemoteCommandResponse(hbResp.toBSON(false), BSONObj(), Milliseconds(8))));
        } else {
            getNet()->blackHole(noi);
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::CannotInitializeNodeWithData, status);
    ASSERT_REASON_CONTAINS(status, "has data already");
    ASSERT_NOT_REASON_CONTAINS(status, "h1:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h2:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h4:1");
    ASSERT_REASON_CONTAINS(status, "h5:1");
}
TEST_F(CheckQuorumForReconfig, QuorumCheckVetoedDueToHigherConfigVersion) {
    // In this test, "we" are host "h3:1".  The request to "h2" does not arrive before the end
    // of the test, and the request to "h1" comes back indicating a higher config version.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 2
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1"))));
    const int myConfigIndex = 2;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        if (request.target == HostAndPort("h1", 1)) {
            getNet()->scheduleResponse(noi,
                                       startDate + Milliseconds(10),
                                       (RemoteCommandResponse(BSON("ok" << 0 << "set"
                                                                        << "rs0"
                                                                        << "v"
                                                                        << 5),
                                                              BSONObj(),
                                                              Milliseconds(8))));
        } else {
            getNet()->blackHole(noi);
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
    ASSERT_REASON_CONTAINS(status, "Our config version of");
    ASSERT_REASON_CONTAINS(status, "is no larger than the version");
    ASSERT_REASON_CONTAINS(status, "h1:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h2:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
}

TEST_F(CheckQuorumForReconfig, QuorumCheckVetoedDueToIncompatibleSetName) {
    // In this test, "we" are host "h3:1".  The request to "h1" times out,
    // and the request to "h2" comes back indicating an incompatible set name.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 2
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1"))));
    const int myConfigIndex = 2;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        if (request.target == HostAndPort("h2", 1)) {
            getNet()->scheduleResponse(noi,
                                       startDate + Milliseconds(10),
                                       (RemoteCommandResponse(BSON("ok" << 0 << "mismatch" << true),
                                                              BSONObj(),
                                                              Milliseconds(8))));
        } else {
            getNet()->scheduleResponse(
                noi, startDate + Milliseconds(10), {ErrorCodes::NoSuchKey, "No response"});
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
    ASSERT_REASON_CONTAINS(status, "Our set name did not match");
    ASSERT_NOT_REASON_CONTAINS(status, "h1:1");
    ASSERT_REASON_CONTAINS(status, "h2:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
}

TEST_F(CheckQuorumForReconfig, QuorumCheckFailsDueToInsufficientVoters) {
    // In this test, "we" are host "h4".  Only "h1", "h2" and "h3" are voters,
    // and of the voters, only "h1" responds.  As a result, quorum check fails.
    // "h5" also responds, but because it cannot vote, is irrelevant for the reconfig
    // quorum check.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 2
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1")
                                              << BSON("_id" << 4 << "host"
                                                            << "h4:1"
                                                            << "votes"
                                                            << 0
                                                            << "priority"
                                                            << 0)
                                              << BSON("_id" << 5 << "host"
                                                            << "h5:1"
                                                            << "votes"
                                                            << 0
                                                            << "priority"
                                                            << 0))));
    const int myConfigIndex = 3;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        if (request.target == HostAndPort("h1", 1) || request.target == HostAndPort("h5", 1)) {
            getNet()->scheduleResponse(
                noi,
                startDate + Milliseconds(10),
                (RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(8))));
        } else {
            getNet()->scheduleResponse(
                noi, startDate + Milliseconds(10), {ErrorCodes::NoSuchKey, "No response"});
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
    ASSERT_REASON_CONTAINS(status, "not enough voting nodes responded; required 2 but only");
    ASSERT_REASON_CONTAINS(status, "h1:1");
    ASSERT_REASON_CONTAINS(status, "h2:1 failed with");
    ASSERT_REASON_CONTAINS(status, "h3:1 failed with");
    ASSERT_NOT_REASON_CONTAINS(status, "h4:1");
    ASSERT_NOT_REASON_CONTAINS(status, "h5:1");
}

TEST_F(CheckQuorumForReconfig, QuorumCheckFailsDueToNoElectableNodeResponding) {
    // In this test, "we" are host "h4".  Only "h1", "h2" and "h3" are electable,
    // and none of them respond.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 2
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1")
                                              << BSON("_id" << 4 << "host"
                                                            << "h4:1"
                                                            << "priority"
                                                            << 0)
                                              << BSON("_id" << 5 << "host"
                                                            << "h5:1"
                                                            << "priority"
                                                            << 0))));
    const int myConfigIndex = 3;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        if (request.target == HostAndPort("h5", 1)) {
            getNet()->scheduleResponse(
                noi,
                startDate + Milliseconds(10),
                (RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(8))));
        } else {
            getNet()->scheduleResponse(
                noi, startDate + Milliseconds(10), {ErrorCodes::NoSuchKey, "No response"});
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
    ASSERT_REASON_CONTAINS(status, "no electable nodes responded");
}

TEST_F(CheckQuorumForReconfig, QuorumCheckSucceedsWithAsSoonAsPossible) {
    // In this test, "we" are host "h4".  Only "h1", "h2" and "h3" can vote.
    // This test should succeed as soon as h1 and h2 respond, so we block
    // h3 and h5 from responding or timing out until the test completes.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 2
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1")
                                              << BSON("_id" << 4 << "host"
                                                            << "h4:1"
                                                            << "votes"
                                                            << 0
                                                            << "priority"
                                                            << 0)
                                              << BSON("_id" << 5 << "host"
                                                            << "h5:1"
                                                            << "votes"
                                                            << 0
                                                            << "priority"
                                                            << 0))));
    const int myConfigIndex = 3;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        if (request.target == HostAndPort("h1", 1) || request.target == HostAndPort("h2", 1)) {
            getNet()->scheduleResponse(
                noi,
                startDate + Milliseconds(10),
                (RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(8))));
        } else {
            getNet()->blackHole(noi);
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();
    ASSERT_OK(waitForQuorumCheck());
}

TEST_F(CheckQuorumForReconfig, QuorumCheckProcessesCallbackCanceledResponse) {
    // In this test, "we" are host "h3".  Only "h1" and "h2" can vote.
    // We are checking that even if a callback was canceled, it will count as a response towards
    // the quorum. Since we make h2 respond negatively, the test requires that it processes the
    // cancelled callback in order to complete.

    const ReplSetConfig rsConfig =
        assertMakeRSConfig(BSON("_id"
                                << "rs0"
                                << "version"
                                << 2
                                << "protocolVersion"
                                << 1
                                << "members"
                                << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                         << "h1:1")
                                              << BSON("_id" << 2 << "host"
                                                            << "h2:1")
                                              << BSON("_id" << 3 << "host"
                                                            << "h3:1"))));
    const int myConfigIndex = 2;
    const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

    startQuorumCheck(rsConfig, myConfigIndex);
    const Date_t startDate = getNet()->now();
    const int numCommandsExpected = rsConfig.getNumMembers() - 1;
    stdx::unordered_set<HostAndPort> seenHosts;
    getNet()->enterNetwork();
    for (int i = 0; i < numCommandsExpected; ++i) {
        const NetworkInterfaceMock::NetworkOperationIterator noi = getNet()->getNextReadyRequest();
        const RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS("admin", request.dbname);
        ASSERT_BSONOBJ_EQ(hbRequest, request.cmdObj);
        ASSERT(seenHosts.insert(request.target).second) << "Already saw "
                                                        << request.target.toString();
        if (request.target == HostAndPort("h1", 1)) {
            getNet()->scheduleResponse(
                noi,
                startDate + Milliseconds(10),
                (RemoteCommandResponse(ErrorCodes::CallbackCanceled, "Testing canceled callback")));
        } else {
            getNet()->scheduleResponse(
                noi,
                startDate + Milliseconds(10),
                (RemoteCommandResponse(BSON("ok" << 0), BSONObj(), Milliseconds(8))));
        }
    }
    getNet()->runUntil(startDate + Milliseconds(10));
    getNet()->exitNetwork();

    Status status = waitForQuorumCheck();
    ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
    ASSERT_REASON_CONTAINS(status, "not enough voting nodes responded");
}

}  // namespace
}  // namespace repl
}  // namespace mongo
