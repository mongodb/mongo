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

#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {
namespace {

    TEST(TopologyCoordinator, ChooseSyncSourceBasic) {
        ReplicationExecutor::CallbackHandle cbh;
        ReplicationExecutor::CallbackData cbData(NULL, 
                                                 cbh,
                                                 Status::OK());
        ReplicaSetConfig config;

        ASSERT_OK(config.initialize(BSON("_id" << "rs0" <<
                                         "version" << 1 <<
                                         "members" << BSON_ARRAY(
                                             BSON("_id" << 10 << "host" << "hself") <<
                                             BSON("_id" << 20 << "host" << "h2") <<
                                             BSON("_id" << 30 << "host" << "h3")))));

        TopologyCoordinatorImpl topocoord((Seconds(999)));
        Date_t now = 0;
        topocoord.updateConfig(cbData, config, 0, now++, OpTime(0,0));

        MemberHeartbeatData h0Info(0);
        h0Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h0Info, 10, OpTime(0,0));

        // member h2 is the furthest ahead
        MemberHeartbeatData h1Info(1);
        h1Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(1,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h1Info, 20, OpTime(0,0));

        MemberHeartbeatData h2Info(2);
        h2Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h2Info, 30, OpTime(0,0));

        // We start with no sync source
        ASSERT(topocoord.getSyncSourceAddress().empty());

        // Fail due to insufficient number of pings
        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT(topocoord.getSyncSourceAddress().empty());

        // Record 2N pings to allow choosing a new sync source; all members equidistant
        topocoord.recordPing(HostAndPort("h2"), 300);
        topocoord.recordPing(HostAndPort("h2"), 300);
        topocoord.recordPing(HostAndPort("h3"), 300);
        topocoord.recordPing(HostAndPort("h3"), 300);

        // Should choose h2, since it is furthest ahead
        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h2"));
        
        // h3 becomes further ahead, so it should be chosen
        h2Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(3,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h2Info, 30, OpTime(0,0));
        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h3"));

        // h3 becomes an invalid candidate for sync source; should choose h2 again
        h2Info.setUpValues(now, MemberState::RS_RECOVERING, OpTime(0,0), OpTime(4,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h2Info, 30, OpTime(0,0));
        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h2"));

        // h3 goes down
        h2Info.setDownValues(now, ""); 
        topocoord.updateHeartbeatData(now++, h2Info, 30, OpTime(0,0));
        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h2"));

        // h3 back up and ahead
        h2Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(5,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h2Info, 30, OpTime(0,0));
        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h3"));

    }

    TEST(TopologyCoordinator, ChooseSyncSourceCandidates) {
        ReplicationExecutor::CallbackHandle cbh;
        ReplicationExecutor::CallbackData cbData(NULL, 
                                                 cbh,
                                                 Status::OK());
        ReplicaSetConfig config;

        ASSERT_OK(config.initialize(
                      BSON("_id" << "rs0" <<
                           "version" << 1 <<
                           "members" << BSON_ARRAY(
                               BSON("_id" << 1 << "host" << "hself") <<
                               BSON("_id" << 10 << "host" << "h1") <<
                               BSON("_id" << 20 << "host" << "h2" << "buildIndexes" << false) <<
                               BSON("_id" << 30 << "host" << "h3" << "hidden" << true) <<
                               BSON("_id" << 40 << "host" << "h4" << "arbiterOnly" << true) <<
                               BSON("_id" << 50 << "host" << "h5" << "slaveDelay" << 1) <<
                               BSON("_id" << 60 << "host" << "h6") <<
                               BSON("_id" << 70 << "host" << "hprimary")))));

        TopologyCoordinatorImpl topocoord((Seconds(100 /*maxSyncSourceLagSeconds*/)));
        Date_t now = 0;
        OpTime lastOpTimeWeApplied(100,0);
        topocoord.updateConfig(cbData, config, 0, now++, lastOpTimeWeApplied);

        MemberHeartbeatData hselfInfo(0);
        hselfInfo.setUpValues(now, MemberState::RS_SECONDARY, lastOpTimeWeApplied, 
                              lastOpTimeWeApplied, "", ""); 
        topocoord.updateHeartbeatData(now++, hselfInfo, 1, lastOpTimeWeApplied);

        MemberHeartbeatData h1Info(1);
        h1Info.setUpValues(now, MemberState::RS_SECONDARY, lastOpTimeWeApplied, OpTime(501,0), 
                           "", ""); 
        topocoord.updateHeartbeatData(now++, h1Info, 10, lastOpTimeWeApplied);

        MemberHeartbeatData h2Info(2);
        h2Info.setUpValues(now, MemberState::RS_SECONDARY, lastOpTimeWeApplied, OpTime(501,0), 
                           "", ""); 
        topocoord.updateHeartbeatData(now++, h2Info, 20, lastOpTimeWeApplied);

        MemberHeartbeatData h3Info(3);
        h3Info.setUpValues(now, MemberState::RS_SECONDARY, lastOpTimeWeApplied, OpTime(501,0), 
                           "", ""); 
        topocoord.updateHeartbeatData(now++, h3Info, 30, lastOpTimeWeApplied);

        MemberHeartbeatData h4Info(4);
        h4Info.setUpValues(now, MemberState::RS_SECONDARY, lastOpTimeWeApplied, OpTime(501,0), 
                           "", ""); 
        topocoord.updateHeartbeatData(now++, h4Info, 40, lastOpTimeWeApplied);

        MemberHeartbeatData h5Info(5);
        h5Info.setUpValues(now, MemberState::RS_SECONDARY, lastOpTimeWeApplied, OpTime(501,0), 
                           "", ""); 
        topocoord.updateHeartbeatData(now++, h5Info, 50, lastOpTimeWeApplied);

        MemberHeartbeatData h6Info(6);
        // This node is lagged further than maxSyncSourceLagSeconds.
        h6Info.setUpValues(now, MemberState::RS_SECONDARY, lastOpTimeWeApplied, OpTime(499,0),
                           "", ""); 
        topocoord.updateHeartbeatData(now++, h6Info, 60, lastOpTimeWeApplied);

        MemberHeartbeatData hprimaryInfo(7);
        hprimaryInfo.setUpValues(now, MemberState::RS_PRIMARY, lastOpTimeWeApplied, OpTime(600,0),
                                 "", ""); 
        topocoord.updateHeartbeatData(now++, hprimaryInfo, 70, lastOpTimeWeApplied);

        // Record 2(N-1) pings to allow choosing a new sync source
        topocoord.recordPing(HostAndPort("h1"), 700);
        topocoord.recordPing(HostAndPort("h1"), 700);
        topocoord.recordPing(HostAndPort("h2"), 600);
        topocoord.recordPing(HostAndPort("h2"), 600);
        topocoord.recordPing(HostAndPort("h3"), 500);
        topocoord.recordPing(HostAndPort("h3"), 500);
        topocoord.recordPing(HostAndPort("h4"), 400);
        topocoord.recordPing(HostAndPort("h4"), 400);
        topocoord.recordPing(HostAndPort("h5"), 300);
        topocoord.recordPing(HostAndPort("h5"), 300);
        topocoord.recordPing(HostAndPort("h6"), 200);
        topocoord.recordPing(HostAndPort("h6"), 200);
        topocoord.recordPing(HostAndPort("hprimary"), 100);
        topocoord.recordPing(HostAndPort("hprimary"), 100);

        // Should choose primary first; it's closest
        topocoord.chooseNewSyncSource(now++, lastOpTimeWeApplied);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("hprimary"));

        // Primary goes far far away
        topocoord.recordPing(HostAndPort("hprimary"), 10000000);

        // Should choose h4.  (if an arbiter has an oplog, it's a valid sync source)
        // h6 is not considered because it is outside the maxSyncLagSeconds window,
        topocoord.chooseNewSyncSource(now++, lastOpTimeWeApplied);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h4"));
        
        // h4 goes down; should choose h1
        h4Info.setDownValues(now, ""); 
        topocoord.updateHeartbeatData(now++, h4Info, 40, lastOpTimeWeApplied);
        topocoord.chooseNewSyncSource(now++, lastOpTimeWeApplied);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h1"));

        // Primary and h1 go down; should choose h6 
        h1Info.setDownValues(now, "");
        topocoord.updateHeartbeatData(now++, h1Info, 10, lastOpTimeWeApplied);
        hprimaryInfo.setDownValues(now, "");
        topocoord.updateHeartbeatData(now++, hprimaryInfo, 70, lastOpTimeWeApplied);
        topocoord.chooseNewSyncSource(now++, lastOpTimeWeApplied);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h6"));

        // h6 goes down; should choose h5
        h6Info.setDownValues(now, "");
        topocoord.updateHeartbeatData(now++, h6Info, 60, lastOpTimeWeApplied);
        topocoord.chooseNewSyncSource(now++, lastOpTimeWeApplied);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h5"));

        // h5 goes down; should choose h3
        h5Info.setDownValues(now, "");
        topocoord.updateHeartbeatData(now++, h5Info, 50, lastOpTimeWeApplied);
        topocoord.chooseNewSyncSource(now++, lastOpTimeWeApplied);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h3"));

        // h3 goes down; no sync source candidates remain
        h3Info.setDownValues(now, "");
        topocoord.updateHeartbeatData(now++, h3Info, 30, lastOpTimeWeApplied);
        topocoord.chooseNewSyncSource(now++, lastOpTimeWeApplied);
        ASSERT(topocoord.getSyncSourceAddress().empty());

    }


    TEST(TopologyCoordinator, ChooseSyncSourceChainingNotAllowed) {
        ReplicationExecutor::CallbackHandle cbh;
        ReplicationExecutor::CallbackData cbData(NULL, 
                                                 cbh,
                                                 Status::OK());
        ReplicaSetConfig config;

        ASSERT_OK(config.initialize(BSON("_id" << "rs0" <<
                                         "version" << 1 <<
                                         "settings" << BSON("chainingAllowed" << false) <<
                                         "members" << BSON_ARRAY(
                                             BSON("_id" << 10 << "host" << "hself") <<
                                             BSON("_id" << 20 << "host" << "h2") <<
                                             BSON("_id" << 30 << "host" << "h3")))));

        TopologyCoordinatorImpl topocoord((Seconds(999)));
        Date_t now = 0;
        topocoord.updateConfig(cbData, config, 0, now++, OpTime(0,0));

        MemberHeartbeatData h0Info(0);
        h0Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h0Info, 10, OpTime(0,0));

        MemberHeartbeatData h1Info(1);
        h1Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(1,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h1Info, 20, OpTime(0,0));

        MemberHeartbeatData h2Info(2);
        h2Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h2Info, 30, OpTime(0,0));

        // Record 2(N-1) pings to allow choosing a new sync source; h2 is the closest.
        topocoord.recordPing(HostAndPort("h2"), 100);
        topocoord.recordPing(HostAndPort("h2"), 100);
        topocoord.recordPing(HostAndPort("h3"), 300);
        topocoord.recordPing(HostAndPort("h3"), 300);

        // No primary situation: should choose no sync source.
        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT(topocoord.getSyncSourceAddress().empty());
        
        // Add primary
        h2Info.setUpValues(now, MemberState::RS_PRIMARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h2Info, 30, OpTime(0,0));

        // h3 is primary and should be chosen as sync source, despite being further away than h2.
        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h3"));

    }

    TEST(TopologyCoordinator, ForceSyncSource) {
        ReplicationExecutor::CallbackHandle cbh;
        ReplicationExecutor::CallbackData cbData(NULL, 
                                                 cbh,
                                                 Status::OK());
        ReplicaSetConfig config;

        ASSERT_OK(config.initialize(BSON("_id" << "rs0" <<
                                         "version" << 1 <<
                                         "members" << BSON_ARRAY(
                                             BSON("_id" << 10 << "host" << "hself") <<
                                             BSON("_id" << 20 << "host" << "h2") <<
                                             BSON("_id" << 30 << "host" << "h3")))));

        TopologyCoordinatorImpl topocoord((Seconds(999)));
        Date_t now = 0;
        topocoord.updateConfig(cbData, config, 0, now++, OpTime(0,0));

        MemberHeartbeatData h0Info(0);
        h0Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h0Info, 10, OpTime(0,0));

        MemberHeartbeatData h1Info(1);
        h1Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(1,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h1Info, 20, OpTime(0,0));

        MemberHeartbeatData h2Info(2);
        h2Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(2,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h2Info, 30, OpTime(0,0));

        // Record 2(N-1) pings to allow choosing a new sync source; h3 is the closest.
        topocoord.recordPing(HostAndPort("h2"), 300);
        topocoord.recordPing(HostAndPort("h2"), 300);
        topocoord.recordPing(HostAndPort("h3"), 100);
        topocoord.recordPing(HostAndPort("h3"), 100);

        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h3"));
        topocoord.setForceSyncSourceIndex(1);
        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h2"));
    }

    TEST(TopologyCoordinator, BlacklistSyncSource) {
        ReplicationExecutor::CallbackHandle cbh;
        ReplicationExecutor::CallbackData cbData(NULL, 
                                                 cbh,
                                                 Status::OK());
        ReplicaSetConfig config;

        ASSERT_OK(config.initialize(BSON("_id" << "rs0" <<
                                         "version" << 1 <<
                                         "members" << BSON_ARRAY(
                                             BSON("_id" << 10 << "host" << "hself") <<
                                             BSON("_id" << 20 << "host" << "h2") <<
                                             BSON("_id" << 30 << "host" << "h3")))));

        TopologyCoordinatorImpl topocoord((Seconds(999)));
        Date_t now = 0;
        topocoord.updateConfig(cbData, config, 0, now++, OpTime(0,0));

        MemberHeartbeatData h0Info(0);
        h0Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h0Info, 10, OpTime(0,0));

        MemberHeartbeatData h1Info(1);
        h1Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(1,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h1Info, 20, OpTime(0,0));

        MemberHeartbeatData h2Info(2);
        h2Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(2,0), "", ""); 
        topocoord.updateHeartbeatData(now++, h2Info, 30, OpTime(0,0));

        // Record 2(N-1) pings to allow choosing a new sync source; h3 is the closest.
        topocoord.recordPing(HostAndPort("h2"), 300);
        topocoord.recordPing(HostAndPort("h2"), 300);
        topocoord.recordPing(HostAndPort("h3"), 100);
        topocoord.recordPing(HostAndPort("h3"), 100);

        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h3"));
        
        Date_t expireTime = 100;
        topocoord.blacklistSyncSource(HostAndPort("h3"), expireTime);
        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        // Should choose second best choice now that h3 is blacklisted.
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h2"));

        // After time has passed, should go back to original sync source
        topocoord.chooseNewSyncSource(expireTime, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h3"));
    }

    TEST(TopologyCoordinator, ReplSetGetStatus) {
        // This test starts by configuring a TopologyCoordinator as a member of a 4 node replica
        // set, with each node in a different state.
        // The first node is DOWN, as if we tried heartbeating them and it failed in some way.
        // The second node is in state SECONDARY, as if we've received a valid heartbeat from them.
        // The third node is in state UNKNOWN, as if we've not yet had any heartbeating activity
        // with them yet.  The fourth node is PRIMARY and corresponds to ourself, which gets its
        // information for replSetGetStatus from a different source than the nodes that aren't
        // ourself.  After this setup, we call prepareStatusResponse and make sure that the fields
        // returned for each member match our expectations.
        Date_t startupTime(100);
        Date_t heartbeatTime = 5000;
        Seconds uptimeSecs(10);
        Date_t curTime = heartbeatTime + uptimeSecs.total_milliseconds();
        OpTime electionTime(1, 2);
        OpTime oplogProgress(3, 4);
        ReplicationExecutor::CallbackHandle cbh;
        ReplicationExecutor::CallbackData cbData(NULL,
                                                 cbh,
                                                 Status::OK());
        TopologyCoordinatorImpl topocoord((Seconds(999)));

        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(
                BSON("_id" << "mySet" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << "test0:1234") <<
                                             BSON("_id" << 1 << "host" << "test1:1234") <<
                                             BSON("_id" << 2 << "host" << "test2:1234") <<
                                             BSON("_id" << 3 << "host" << "test3:1234")))));
        topocoord.updateConfig(cbData, config, 3, startupTime + 1, OpTime(0,0));

        // Now that the replica set is setup, put the members into the states we want them in.
        MemberHeartbeatData member0hb(0);
        member0hb.setDownValues(heartbeatTime, "");
        topocoord.updateHeartbeatData(heartbeatTime, member0hb, 0, oplogProgress);
        MemberHeartbeatData member1hb(1);
        member1hb.setUpValues(
                heartbeatTime, MemberState::RS_SECONDARY, electionTime, oplogProgress, "", "READY");
        topocoord.updateHeartbeatData(startupTime, member1hb, 1, oplogProgress);
        topocoord._changeMemberState(MemberState::RS_PRIMARY);

        // Now node 0 is down, node 1 is up, and for node 2 we have no heartbeat data yet.
        BSONObjBuilder statusBuilder;
        Status resultStatus(ErrorCodes::InternalError, "prepareStatusResponse didn't set result");
        topocoord.prepareStatusResponse(cbData,
                                        curTime,
                                        uptimeSecs.total_seconds(),
                                        oplogProgress,
                                        &statusBuilder,
                                        &resultStatus);
        ASSERT_OK(resultStatus);
        BSONObj rsStatus = statusBuilder.obj();

        // Test results for all non-self members
        ASSERT_EQUALS("mySet", rsStatus["set"].String());
        ASSERT_EQUALS(curTime.asInt64(), rsStatus["date"].Date().asInt64());
        std::vector<BSONElement> memberArray = rsStatus["members"].Array();
        ASSERT_EQUALS(4U, memberArray.size());
        BSONObj member0Status = memberArray[0].Obj();
        BSONObj member1Status = memberArray[1].Obj();
        BSONObj member2Status = memberArray[2].Obj();

        // Test member 0, the node that's DOWN
        ASSERT_EQUALS(0, member0Status["_id"].Int());
        ASSERT_EQUALS("test0:1234", member0Status["name"].String());
        ASSERT_EQUALS(0, member0Status["health"].Double());
        ASSERT_EQUALS(MemberState::RS_DOWN, member0Status["state"].Int());
        ASSERT_EQUALS("(not reachable/healthy)", member0Status["stateStr"].String());
        ASSERT_EQUALS(0, member0Status["uptime"].Int());
        ASSERT_EQUALS(OpTime(), OpTime(member0Status["optime"].timestampValue()));
        ASSERT_EQUALS(OpTime().asDate(), member0Status["optimeDate"].Date().millis);
        ASSERT_EQUALS(heartbeatTime, member0Status["lastHeartbeat"].Date());
        ASSERT_EQUALS(Date_t(), member0Status["lastHeartbeatRecv"].Date());

        // Test member 1, the node that's SECONDARY
        ASSERT_EQUALS(1, member1Status["_id"].Int());
        ASSERT_EQUALS("test1:1234", member1Status["name"].String());
        ASSERT_EQUALS(1, member1Status["health"].Double());
        ASSERT_EQUALS(MemberState::RS_SECONDARY, member1Status["state"].Int());
        ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY).toString(),
                      member1Status["stateStr"].String());
        ASSERT_EQUALS(uptimeSecs.total_seconds(), member1Status["uptime"].Int());
        ASSERT_EQUALS(oplogProgress, OpTime(member1Status["optime"].timestampValue()));
        ASSERT_EQUALS(oplogProgress.asDate(), member1Status["optimeDate"].Date().millis);
        ASSERT_EQUALS(heartbeatTime, member1Status["lastHeartbeat"].Date());
        ASSERT_EQUALS(Date_t(), member1Status["lastHeartbeatRecv"].Date());
        ASSERT_EQUALS("READY", member1Status["lastHeartbeatMessage"].String());

        // Test member 2, the node that's UNKNOWN
        ASSERT_EQUALS(2, member2Status["_id"].Int());
        ASSERT_EQUALS("test2:1234", member2Status["name"].String());
        ASSERT_EQUALS(-1, member2Status["health"].Double());
        ASSERT_EQUALS(MemberState::RS_UNKNOWN, member2Status["state"].Int());
        ASSERT_EQUALS(MemberState(MemberState::RS_UNKNOWN).toString(),
                      member2Status["stateStr"].String());
        ASSERT_FALSE(member2Status.hasField("uptime"));
        ASSERT_FALSE(member2Status.hasField("optime"));
        ASSERT_FALSE(member2Status.hasField("optimeDate"));
        ASSERT_FALSE(member2Status.hasField("lastHearbeat"));
        ASSERT_FALSE(member2Status.hasField("lastHearbeatRecv"));

        // Now test results for ourself, the PRIMARY
        ASSERT_EQUALS(MemberState::RS_PRIMARY, rsStatus["myState"].Int());
        BSONObj selfStatus = memberArray[3].Obj();
        ASSERT_TRUE(selfStatus["self"].Bool());
        ASSERT_EQUALS(3, selfStatus["_id"].Int());
        ASSERT_EQUALS("test3:1234", selfStatus["name"].String());
        ASSERT_EQUALS(1, selfStatus["health"].Double());
        ASSERT_EQUALS(MemberState::RS_PRIMARY, selfStatus["state"].Int());
        ASSERT_EQUALS(MemberState(MemberState::RS_PRIMARY).toString(),
                      selfStatus["stateStr"].String());
        ASSERT_EQUALS(uptimeSecs.total_seconds(), selfStatus["uptime"].Int());
        ASSERT_EQUALS(oplogProgress, OpTime(selfStatus["optime"].timestampValue()));
        ASSERT_EQUALS(oplogProgress.asDate(), selfStatus["optimeDate"].Date().millis);

        // TODO(spencer): Test electionTime and pingMs are set properly
    }

}  // namespace
}  // namespace repl
}  // namespace mongo
