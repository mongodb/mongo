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

#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/network_interface_mock.h"
#include "mongo/db/repl/repl_coordinator_external_state_mock.h"
#include "mongo/db/repl/repl_coordinator_impl.h"
#include "mongo/db/repl/repl_coordinator_test_fixture.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {
namespace {

    class ReplCoordHBTest : public ReplCoordTest {
    protected:
        void assertMemberState(MemberState expected);
        ReplSetHeartbeatResponse receiveHeartbeatFrom(
                const ReplicaSetConfig& rsConfig,
                int sourceId,
                const HostAndPort& source);
    };

    void ReplCoordHBTest::assertMemberState(const MemberState expected) {
        const MemberState actual = getReplCoord()->getCurrentMemberState();
        ASSERT(expected == actual) << "Expected coordinator to report state " <<
            expected.toString() << " but found " << actual.toString();
    }

    ReplSetHeartbeatResponse ReplCoordHBTest::receiveHeartbeatFrom(
            const ReplicaSetConfig& rsConfig,
            int sourceId,
            const HostAndPort& source) {
        ReplSetHeartbeatArgs hbArgs;
        hbArgs.setProtocolVersion(1);
        hbArgs.setConfigVersion(rsConfig.getConfigVersion());
        hbArgs.setSetName(rsConfig.getReplSetName());
        hbArgs.setSenderHost(source);
        hbArgs.setSenderId(sourceId);
        ASSERT(hbArgs.isInitialized());

        ReplSetHeartbeatResponse response;
        ASSERT_OK(getReplCoord()->processHeartbeat(hbArgs, &response));
        return response;
    }

    TEST_F(ReplCoordHBTest, JoinExistingReplSet) {
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(3));
        ReplicaSetConfig rsConfig = assertMakeRSConfig(
                BSON("_id" << "mySet" <<
                     "version" << 3 <<
                     "members" << BSON_ARRAY(BSON("_id" << 1 << "host" << "h1:1") <<
                                             BSON("_id" << 2 << "host" << "h2:1") <<
                                             BSON("_id" << 3 << "host" << "h3:1"))));
        init("mySet");
        addSelf(HostAndPort("h2", 1));
        const Date_t startDate = getNet()->now();
        start();
        enterNetwork();
        assertMemberState(MemberState::RS_STARTUP);
        NetworkInterfaceMock* net = getNet();
        ASSERT_FALSE(net->hasReadyRequests());
        exitNetwork();
        receiveHeartbeatFrom(rsConfig, 1, HostAndPort("h1", 1));

        enterNetwork();
        NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
        const ReplicationExecutor::RemoteCommandRequest& request = noi->getRequest();
        ASSERT_EQUALS(HostAndPort("h1", 1), request.target);
        ReplSetHeartbeatArgs hbArgs;
        ASSERT_OK(hbArgs.initialize(request.cmdObj));
        ASSERT_EQUALS("mySet", hbArgs.getSetName());
        ASSERT_EQUALS(-2, hbArgs.getConfigVersion());
        ReplSetHeartbeatResponse hbResp;
        hbResp.setSetName("mySet");
        hbResp.setState(MemberState::RS_PRIMARY);
        hbResp.noteReplSet();
        hbResp.setVersion(rsConfig.getConfigVersion());
        hbResp.setConfig(rsConfig);
        BSONObjBuilder responseBuilder;
        responseBuilder << "ok" << 1;
        hbResp.addToBSON(&responseBuilder);
        net->scheduleResponse(noi, startDate + 200, makeResponseStatus(responseBuilder.obj()));
        net->runUntil(startDate + 200);
        ASSERT_EQUALS(startDate + 200, net->now());

        // Because the new config is stored using an out-of-band thread, we need to perform some
        // extra synchronization to let the executor finish the heartbeat reconfig.  We know that
        // after the out-of-band thread completes, it schedules new heartbeats.  We assume that no
        // other network operations get scheduled during or before the reconfig, though this may
        // cease to be true in the future.
        noi = net->getNextReadyRequest();

        assertMemberState(MemberState::RS_STARTUP2);
        OperationContextNoop txn;
        ReplicaSetConfig storedConfig;
        ASSERT_OK(storedConfig.initialize(
                          unittest::assertGet(getExternalState()->loadLocalConfigDocument(&txn))));
        ASSERT_OK(storedConfig.validate());
        ASSERT_EQUALS(3, storedConfig.getConfigVersion());
        ASSERT_EQUALS(3, storedConfig.getNumMembers());
        exitNetwork();
    }

}  // namespace
}  // namespace repl
}  // namespace mongo
