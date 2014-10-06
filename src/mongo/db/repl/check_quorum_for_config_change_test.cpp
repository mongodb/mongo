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

#include "mongo/platform/basic.h"

#include <boost/thread.hpp>
#include <boost/scoped_ptr.hpp>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/check_quorum_for_config_change.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"

#define ASSERT_REASON_CONTAINS(STATUS, PATTERN) do {                    \
        const mongo::Status s_ = (STATUS);                              \
        ASSERT_FALSE(s_.reason().find(PATTERN) == std::string::npos) << \
            #STATUS ".reason() == " << s_.reason();                     \
    } while (false)

#define ASSERT_NOT_REASON_CONTAINS(STATUS, PATTERN) do {                \
        const mongo::Status s_ = (STATUS);                              \
        ASSERT_TRUE(s_.reason().find(PATTERN) == std::string::npos) <<  \
            #STATUS ".reason() == " << s_.reason();                     \
    } while (false)

namespace mongo {
namespace repl {
namespace {

    typedef ReplicationExecutor::RemoteCommandRequest RemoteCommandRequest;

    class CheckQuorumTest : public mongo::unittest::Test {
    protected:
        NetworkInterfaceMockWithMap* _net;
        boost::scoped_ptr<ReplicationExecutor> _executor;
        boost::scoped_ptr<boost::thread> _executorThread;

    private:
        void setUp();
        void tearDown();
    };

    class CheckQuorumForInitiate : public CheckQuorumTest {};
    class CheckQuorumForReconfig : public CheckQuorumTest {};

    void CheckQuorumTest::setUp() {
        _net = new NetworkInterfaceMockWithMap;
        _executor.reset(new ReplicationExecutor(_net, 1 /* prng */ ));
        _executorThread.reset(new boost::thread(stdx::bind(&ReplicationExecutor::run,
                                                           _executor.get())));
    }

    void CheckQuorumTest::tearDown() {
        _net->unblockAll();
        _executor->shutdown();
        _executorThread->join();
    }

    ReplicaSetConfig assertMakeRSConfig(const BSONObj& configBson) {
        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(configBson));
        ASSERT_OK(config.validate());
        return config;
    }

    TEST_F(CheckQuorumForInitiate, ValidSingleNodeSet) {
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1"))));
        ASSERT_OK(checkQuorumForInitiate(_executor.get(), config, 0));
    }

    TEST_F(CheckQuorumForInitiate, QuorumCheckCanceledByShutdown) {
        _executor->shutdown();
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1"))));
        ASSERT_EQUALS(ErrorCodes::ShutdownInProgress,
                      checkQuorumForInitiate(_executor.get(), config, 0));
    }

    TEST_F(CheckQuorumForInitiate, QuorumCheckFailedDueToSeveralDownNodes) {
        // In this test, "we" are host "h3:1".  All other nodes time out on
        // their heartbeat request, and so the quorum check for initiate
        // will fail because some members were unavailable.
        ReplicaSetConfig config = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1:1") <<
                             BSON("_id" << 2 << "host" << "h2:1") <<
                             BSON("_id" << 3 << "host" << "h3:1") <<
                             BSON("_id" << 4 << "host" << "h4:1") <<
                             BSON("_id" << 5 << "host" << "h5:1"))));
        Status status = checkQuorumForInitiate(_executor.get(), config, 2);
        ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
        ASSERT_REASON_CONTAINS(
                status, "Could not contact the following nodes during replica set initiation");
        ASSERT_REASON_CONTAINS(status, "h1:1");
        ASSERT_REASON_CONTAINS(status, "h2:1");
        ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
        ASSERT_REASON_CONTAINS(status, "h4:1");
        ASSERT_REASON_CONTAINS(status, "h5:1");
    }

    const BSONObj makeHeartbeatRequest(const ReplicaSetConfig& rsConfig, int myConfigIndex) {
        const MemberConfig& myConfig = rsConfig.getMemberAt(myConfigIndex);
        ReplSetHeartbeatArgs hbArgs;
        hbArgs.setSetName(rsConfig.getReplSetName());
        hbArgs.setProtocolVersion(1);
        hbArgs.setConfigVersion(rsConfig.getConfigVersion());
        hbArgs.setCheckEmpty(rsConfig.getConfigVersion() == 1);
        hbArgs.setSenderHost(myConfig.getHostAndPort());
        hbArgs.setSenderId(myConfig.getId());
        return hbArgs.toBSON();
    }

    TEST_F(CheckQuorumForInitiate, QuorumCheckSuccessForFiveNodes) {
        // In this test, "we" are host "h3:1".  All nodes respond successfully to their heartbeat
        // requests, and the quorum check succeeds.

        const ReplicaSetConfig rsConfig = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1:1") <<
                             BSON("_id" << 2 << "host" << "h2:1") <<
                             BSON("_id" << 3 << "host" << "h3:1") <<
                             BSON("_id" << 4 << "host" << "h4:1") <<
                             BSON("_id" << 5 << "host" << "h5:1"))));
        const int myConfigIndex = 2;
        const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h2", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h4", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h5", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));

        ASSERT_OK(checkQuorumForInitiate(_executor.get(), rsConfig, myConfigIndex));
    }

    TEST_F(CheckQuorumForInitiate, QuorumCheckFailedDueToOneDownNode) {
        // In this test, "we" are host "h3:1".  All nodes except "h2:1" respond
        // successfully to their heartbeat requests, but quorum check fails because
        // all nodes must be available for initiate.  This is so even though "h2"
        // is neither voting nor electable.

        const ReplicaSetConfig rsConfig = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1:1") <<
                             BSON("_id" << 2 << "host" << "h2:1" <<
                                  "priority" << 0 << "votes" << 0) <<
                             BSON("_id" << 3 << "host" << "h3:1") <<
                             BSON("_id" << 4 << "host" << "h4:1") <<
                             BSON("_id" << 5 << "host" << "h5:1") <<
                             BSON("_id" << 6 << "host" << "h6:1"))));
        const int myConfigIndex = 2;
        const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h4", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h5", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h6", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));

        Status status = checkQuorumForInitiate(_executor.get(), rsConfig, myConfigIndex);
        ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
        ASSERT_REASON_CONTAINS(
                status, "Could not contact the following nodes during replica set initiation");
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

        const ReplicaSetConfig rsConfig = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1:1") <<
                             BSON("_id" << 2 << "host" << "h2:1") <<
                             BSON("_id" << 3 << "host" << "h3:1") <<
                             BSON("_id" << 4 << "host" << "h4:1") <<
                             BSON("_id" << 5 << "host" << "h5:1"))));
        const int myConfigIndex = 2;
        const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h2", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h4", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 0 << "mismatch" << true)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h5", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));

        Status status = checkQuorumForInitiate(_executor.get(), rsConfig, myConfigIndex);
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
        ASSERT_REASON_CONTAINS(
                status, "Our set name did not match");
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

        const ReplicaSetConfig rsConfig = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1:1") <<
                             BSON("_id" << 2 << "host" << "h2:1") <<
                             BSON("_id" << 3 << "host" << "h3:1") <<
                             BSON("_id" << 4 << "host" << "h4:1") <<
                             BSON("_id" << 5 << "host" << "h5:1"))));
        const int myConfigIndex = 2;
        const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h2", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h4", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h5", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 0 << "set" << "rs0" << "v" << 1)));

        Status status = checkQuorumForInitiate(_executor.get(), rsConfig, myConfigIndex);
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
        ASSERT_REASON_CONTAINS(
                status, "Our config version of");
        ASSERT_REASON_CONTAINS(
                status, "is no larger than the version");
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

        const ReplicaSetConfig rsConfig = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1:1") <<
                             BSON("_id" << 2 << "host" << "h2:1") <<
                             BSON("_id" << 3 << "host" << "h3:1") <<
                             BSON("_id" << 4 << "host" << "h4:1") <<
                             BSON("_id" << 5 << "host" << "h5:1"))));
        const int myConfigIndex = 2;
        const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

        // Responses from nodes h1, h2 and h4 and blocked until after the test
        // completes.
        _net->addResponse(RemoteCommandRequest(HostAndPort("h1", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)),
                          true);
        _net->addResponse(RemoteCommandRequest(HostAndPort("h2", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)),
                          true);
        _net->addResponse(RemoteCommandRequest(HostAndPort("h4", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)),
                          true);

        // h5 responds, with a version incompatibility.
        _net->addResponse(RemoteCommandRequest(HostAndPort("h5", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 0 << "set" << "rs0" << "v" << 1)));

        Status status = checkQuorumForInitiate(_executor.get(), rsConfig, myConfigIndex);
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
        ASSERT_REASON_CONTAINS(
                status, "Our config version of");
        ASSERT_REASON_CONTAINS(
                status, "is no larger than the version");
        ASSERT_NOT_REASON_CONTAINS(status, "h1:1");
        ASSERT_NOT_REASON_CONTAINS(status, "h2:1");
        ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
        ASSERT_NOT_REASON_CONTAINS(status, "h4:1");
        ASSERT_REASON_CONTAINS(status, "h5:1");
    }

    TEST_F(CheckQuorumForReconfig, QuorumCheckVetoedDueToHigherConfigVersion) {
        // In this test, "we" are host "h3:1".  The request to "h2" times out,
        // and the request to "h1" comes back indicating a higher config version.

        const ReplicaSetConfig rsConfig = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1:1") <<
                             BSON("_id" << 2 << "host" << "h2:1") <<
                             BSON("_id" << 3 << "host" << "h3:1"))));
        const int myConfigIndex = 2;
        const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 0 << "set" << "rs0" << "v" << 5)));

        Status status = checkQuorumForReconfig(_executor.get(), rsConfig, myConfigIndex);
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
        ASSERT_REASON_CONTAINS(
                status, "Our config version of");
        ASSERT_REASON_CONTAINS(
                status, "is no larger than the version");
        ASSERT_REASON_CONTAINS(status, "h1:1");
        ASSERT_NOT_REASON_CONTAINS(status, "h2:1");
        ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
    }

    TEST_F(CheckQuorumForReconfig, QuorumCheckVetoedDueToIncompatibleSetName) {
        // In this test, "we" are host "h3:1".  The request to "h1" times out,
        // and the request to "h2" comes back indicating an incompatible set name.

        const ReplicaSetConfig rsConfig = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1:1") <<
                             BSON("_id" << 2 << "host" << "h2:1") <<
                             BSON("_id" << 3 << "host" << "h3:1"))));
        const int myConfigIndex = 2;
        const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h2", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 0 << "mismatch" << true)));

        Status status = checkQuorumForReconfig(_executor.get(), rsConfig, myConfigIndex);
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, status);
        ASSERT_REASON_CONTAINS(status, "Our set name did not match");
        ASSERT_NOT_REASON_CONTAINS(status, "h1:1");
        ASSERT_REASON_CONTAINS(status, "h2:1");
        ASSERT_NOT_REASON_CONTAINS(status, "h3:1");

    }

    TEST_F(CheckQuorumForReconfig, QuorumCheckFailsDueToInsufficientVoters) {
        // In this test, "we" are host "h4".  Only "h1", "h2" and "h3" are voters,
        // and of the voters, only "h1" responds.  As a result, quorum check fails.

        const ReplicaSetConfig rsConfig = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1:1") <<
                             BSON("_id" << 2 << "host" << "h2:1") <<
                             BSON("_id" << 3 << "host" << "h3:1") <<
                             BSON("_id" << 4 << "host" << "h4:1" << "votes" << 0) <<
                             BSON("_id" << 5 << "host" << "h5:1" << "votes" << 0))));
        const int myConfigIndex = 3;
        const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h5", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        Status status = checkQuorumForReconfig(_executor.get(), rsConfig, myConfigIndex);
        ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
        ASSERT_REASON_CONTAINS(status, "not enough voting nodes responded; required 2 but only");
        ASSERT_REASON_CONTAINS(status, "h1:1");
        ASSERT_NOT_REASON_CONTAINS(status, "h2:1");
        ASSERT_NOT_REASON_CONTAINS(status, "h3:1");
        ASSERT_NOT_REASON_CONTAINS(status, "h4:1");
        ASSERT_NOT_REASON_CONTAINS(status, "h5:1");
    }

    TEST_F(CheckQuorumForReconfig, QuorumCheckFailsDueToNoElectableNodeResponding) {
        // In this test, "we" are host "h4".  Only "h1", "h2" and "h3" are electable,
        // and none of them respond.

        const ReplicaSetConfig rsConfig = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1:1") <<
                             BSON("_id" << 2 << "host" << "h2:1") <<
                             BSON("_id" << 3 << "host" << "h3:1") <<
                             BSON("_id" << 4 << "host" << "h4:1" << "priority" << 0) <<
                             BSON("_id" << 5 << "host" << "h5:1" << "priority" << 0))));
        const int myConfigIndex = 3;
        const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h5", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        Status status = checkQuorumForReconfig(_executor.get(), rsConfig, myConfigIndex);
        ASSERT_EQUALS(ErrorCodes::NodeNotFound, status);
        ASSERT_REASON_CONTAINS(status, "no electable nodes responded");
    }

    // TODO: Succeed with minimal quorum.
    TEST_F(CheckQuorumForReconfig, QuorumCheckSucceedsWithAsSoonAsPossible) {
        // In this test, "we" are host "h4".  Only "h1", "h2" and "h3" can vote.
        // This test should succeed as soon as h1 and h2 respond, so we block
        // h3 and h5 from responding or timing out until the test completes.

        const ReplicaSetConfig rsConfig = assertMakeRSConfig(
                BSON("_id" << "rs0" <<
                     "version" << 2 <<
                     "members" << BSON_ARRAY(
                             BSON("_id" << 1 << "host" << "h1:1") <<
                             BSON("_id" << 2 << "host" << "h2:1") <<
                             BSON("_id" << 3 << "host" << "h3:1") <<
                             BSON("_id" << 4 << "host" << "h4:1" << "votes" << 0) <<
                             BSON("_id" << 5 << "host" << "h5:1" << "votes" << 0))));
        const int myConfigIndex = 3;
        const BSONObj hbRequest = makeHeartbeatRequest(rsConfig, myConfigIndex);

        _net->addResponse(RemoteCommandRequest(HostAndPort("h1", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));
        _net->addResponse(RemoteCommandRequest(HostAndPort("h2", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 1)));

        // If this message arrived, the reconfig would be vetoed, but it is delayed
        // until the quorum check completes, and so has no effect.
        _net->addResponse(RemoteCommandRequest(HostAndPort("h3", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 0 << "set" << "rs0" << "v" << 3)),
                          true);

        // Ditto RE veto and no effect.
        _net->addResponse(RemoteCommandRequest(HostAndPort("h5", 1),
                                               "admin",
                                               hbRequest),
                          StatusWith<BSONObj>(BSON("ok" << 0 << "set" << "rs0" << "v" << 3)),
                          true);
        ASSERT_OK(checkQuorumForReconfig(_executor.get(), rsConfig, myConfigIndex));
    }

    class QuorumScatterGatherTest : public mongo::unittest::Test {
    public:
        virtual void start(ReplicaSetConfig* config) {
            int selfConfigIndex = 0;
            _checker.reset(new QuorumChecker(config, selfConfigIndex));
        }

    protected:
        bool hasReceivedSufficientResponses() {
            return _checker->hasReceivedSufficientResponses();
        }

        Status getFinalStatus() {
            return _checker->getFinalStatus();
        }

        void processResponse(const RemoteCommandRequest& request, const ResponseStatus& response) {
            _checker->processResponse(request, response);
        }

        RemoteCommandRequest requestFrom(std::string hostname) {
            return RemoteCommandRequest(HostAndPort(hostname),
                                        "", // the non-hostname fields do not matter for Quorum
                                        BSONObj(),
                                        Milliseconds(0));
        }

        ResponseStatus badResponseStatus() {
            return ResponseStatus(ErrorCodes::NodeNotFound, "not on my watch");
        }

        ResponseStatus notOk() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("ok" << 0),
                                                                 Milliseconds(10)));
        }

        ResponseStatus mismatchSetName() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("mismatch" << true),
                                                                 Milliseconds(10)));
        }

        ResponseStatus higherConfigValue() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("set" << std::string("rs0") <<
                                                                      "v" << 3),
                                                                 Milliseconds(10)));
        }

        ResponseStatus ok() {
            return ResponseStatus(NetworkInterfaceMock::Response(BSON("ok" << 1),
                                                                 Milliseconds(10)));
        }

        BSONObj fiveNodeConfigWeDontVote() {
            return BSON("_id" << "rs0" <<
                        "version" << 2 <<
                        "members" << BSON_ARRAY(
                            BSON("_id" << 0 << "host" << "host0" << "votes" << 0) <<
                            BSON("_id" << 1 << "host" << "host1") <<
                            BSON("_id" << 2 << "host" << "host2") <<
                            BSON("_id" << 3 << "host" << "host3") <<
                            BSON("_id" << 4 << "host" << "host4")));
        }

        BSONObj basicFiveNodeConfig() {
            return BSON("_id" << "rs0" <<
                        "version" << 2 <<
                        "members" << BSON_ARRAY(
                            BSON("_id" << 0 << "host" << "host0") <<
                            BSON("_id" << 1 << "host" << "host1") <<
                            BSON("_id" << 2 << "host" << "host2") <<
                            BSON("_id" << 3 << "host" << "host3") <<
                            BSON("_id" << 4 << "host" << "host4")));
        }

        BSONObj initialThreeNodeConfig() {
            return BSON("_id" << "rs0" <<
                        "version" << 1 <<
                        "members" << BSON_ARRAY(
                            BSON("_id" << 0 << "host" << "host0") <<
                            BSON("_id" << 1 << "host" << "host1") <<
                            BSON("_id" << 2 << "host" << "host2")));
        }

        BSONObj basicThreeNodeConfig() {
            return BSON("_id" << "rs0" <<
                        "version" << 2 <<
                        "members" << BSON_ARRAY(
                            BSON("_id" << 0 << "host" << "host0") <<
                            BSON("_id" << 1 << "host" << "host1") <<
                            BSON("_id" << 2 << "host" << "host2")));
        }

        BSONObj threeNodeOnlyOneElectableConfig() {
            return BSON("_id" << "rs0" <<
                        "version" << 2 <<
                        "members" << BSON_ARRAY(
                            BSON("_id" << 0 << "host" << "host0" << "priority" << 0) <<
                            BSON("_id" << 1 << "host" << "host1") <<
                            BSON("_id" << 2 << "host" << "host2" << "priority" << 0)));
        }

    private:
        scoped_ptr<QuorumChecker> _checker;
    };

    TEST_F(QuorumScatterGatherTest, MismatchSetNames) {
        ReplicaSetConfig config;
        config.initialize(basicThreeNodeConfig());
        start(&config);
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host1")), mismatchSetName());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, getFinalStatus());
        ASSERT_TRUE(getFinalStatus().reason().find("set name did not match") != std::string::npos);
    }

    TEST_F(QuorumScatterGatherTest, HigherConfigVersionExists) {
        ReplicaSetConfig config;
        config.initialize(basicThreeNodeConfig());
        start(&config);
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host1")), higherConfigValue());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(ErrorCodes::NewReplicaSetConfigurationIncompatible, getFinalStatus());
        ASSERT_TRUE(getFinalStatus().reason().find("larger than the version") != std::string::npos);
    }

    TEST_F(QuorumScatterGatherTest, InitialConfigRequiresAllMembers) {
        ReplicaSetConfig config;
        config.initialize(initialThreeNodeConfig());
        start(&config);
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host1")), ok());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host2")), ok());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_OK(getFinalStatus());
    }

    TEST_F(QuorumScatterGatherTest, OneBadStatusDoesntSpoilTheBunch) {
        ReplicaSetConfig config;
        config.initialize(basicThreeNodeConfig());
        start(&config);
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host1")), badResponseStatus());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host2")), ok());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_OK(getFinalStatus());
    }

    TEST_F(QuorumScatterGatherTest, OneNotOkDoesntSpoilTheBunch) {
        ReplicaSetConfig config;
        config.initialize(basicThreeNodeConfig());
        start(&config);
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host1")), notOk());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host2")), ok());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_OK(getFinalStatus());
    }

    TEST_F(QuorumScatterGatherTest, ElectableMemberDoesNotRespond) {
        ReplicaSetConfig config;
        config.initialize(threeNodeOnlyOneElectableConfig());
        start(&config);
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host1")), notOk());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host2")), ok());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(ErrorCodes::NodeNotFound, getFinalStatus());
        ASSERT_TRUE(getFinalStatus().reason().find("no electable nodes") != std::string::npos);
    }

    TEST_F(QuorumScatterGatherTest, NotEnoughVotes) {
        ReplicaSetConfig config;
        config.initialize(basicFiveNodeConfig());
        start(&config);
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host1")), notOk());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host2")), notOk());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host3")), notOk());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host4")), ok());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(ErrorCodes::NodeNotFound, getFinalStatus());
        ASSERT_TRUE(getFinalStatus().reason().find("not enough voting nodes") != std::string::npos);
    }

    TEST_F(QuorumScatterGatherTest, NoVotes) {
        ReplicaSetConfig config;
        config.initialize(fiveNodeConfigWeDontVote());
        start(&config);
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host1")), notOk());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host2")), notOk());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host3")), notOk());
        ASSERT_FALSE(hasReceivedSufficientResponses());

        processResponse(requestFrom(std::string("host4")), notOk());
        ASSERT_TRUE(hasReceivedSufficientResponses());
        ASSERT_EQUALS(ErrorCodes::NodeNotFound, getFinalStatus());
        ASSERT_TRUE(getFinalStatus().reason().find("not enough voting nodes") != std::string::npos);
        ASSERT_TRUE(getFinalStatus().reason().find("none responded") != std::string::npos);
    }

}  // namespace
}  // namespace repl
}  // namespace mongo
