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

    // So that you can ASSERT_EQUALS two OpTimes
    std::ostream& operator<<( std::ostream &s, const OpTime &ot ) {
        s << ot.toString();
        return s;
    }
    // So that you can ASSERT_EQUALS two Date_ts
    std::ostream& operator<<( std::ostream &s, const Date_t &t ) {
        s << t.toString();
        return s;
    }

namespace repl {
namespace {

    bool stringContains(const std::string &haystack, const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    }

    TEST(TopologyCoordinator, ChooseSyncSourceBasic) {
        TopologyCoordinatorImpl topocoord((Seconds(999)));
        Date_t now = 0;
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
        ASSERT_OK(config.validate());

        topocoord.updateConfig(config, 0, now++, OpTime(0,0));

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
        topocoord.recordPing(HostAndPort("h2"), Milliseconds(300));
        topocoord.recordPing(HostAndPort("h2"), Milliseconds(300));
        topocoord.recordPing(HostAndPort("h3"), Milliseconds(300));
        topocoord.recordPing(HostAndPort("h3"), Milliseconds(300));

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
        topocoord.updateConfig(config, 0, now++, lastOpTimeWeApplied);

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
        topocoord.recordPing(HostAndPort("h1"), Milliseconds(700));
        topocoord.recordPing(HostAndPort("h1"), Milliseconds(700));
        topocoord.recordPing(HostAndPort("h2"), Milliseconds(600));
        topocoord.recordPing(HostAndPort("h2"), Milliseconds(600));
        topocoord.recordPing(HostAndPort("h3"), Milliseconds(500));
        topocoord.recordPing(HostAndPort("h3"), Milliseconds(500));
        topocoord.recordPing(HostAndPort("h4"), Milliseconds(400));
        topocoord.recordPing(HostAndPort("h4"), Milliseconds(400));
        topocoord.recordPing(HostAndPort("h5"), Milliseconds(300));
        topocoord.recordPing(HostAndPort("h5"), Milliseconds(300));
        topocoord.recordPing(HostAndPort("h6"), Milliseconds(200));
        topocoord.recordPing(HostAndPort("h6"), Milliseconds(200));
        topocoord.recordPing(HostAndPort("hprimary"), Milliseconds(100));
        topocoord.recordPing(HostAndPort("hprimary"), Milliseconds(100));

        // Should choose primary first; it's closest
        topocoord.chooseNewSyncSource(now++, lastOpTimeWeApplied);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("hprimary"));

        // Primary goes far far away
        topocoord.recordPing(HostAndPort("hprimary"), Milliseconds(10000000));

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
        topocoord.updateConfig(config, 0, now++, OpTime(0,0));

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
        topocoord.recordPing(HostAndPort("h2"), Milliseconds(100));
        topocoord.recordPing(HostAndPort("h2"), Milliseconds(100));
        topocoord.recordPing(HostAndPort("h3"), Milliseconds(300));
        topocoord.recordPing(HostAndPort("h3"), Milliseconds(300));

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
        ReplicaSetConfig config;

        ASSERT_OK(config.initialize(BSON("_id" << "rs0" <<
                                         "version" << 1 <<
                                         "members" << BSON_ARRAY(
                                             BSON("_id" << 10 << "host" << "hself") <<
                                             BSON("_id" << 20 << "host" << "h2") <<
                                             BSON("_id" << 30 << "host" << "h3")))));

        TopologyCoordinatorImpl topocoord((Seconds(999)));
        Date_t now = 0;
        topocoord.updateConfig(config, 0, now++, OpTime(0,0));

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
        topocoord.recordPing(HostAndPort("h2"), Milliseconds(300));
        topocoord.recordPing(HostAndPort("h2"), Milliseconds(300));
        topocoord.recordPing(HostAndPort("h3"), Milliseconds(100));
        topocoord.recordPing(HostAndPort("h3"), Milliseconds(100));

        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h3"));
        topocoord.setForceSyncSourceIndex(1);
        topocoord.chooseNewSyncSource(now++, OpTime(0,0));
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h2"));
    }

    TEST(TopologyCoordinator, BlacklistSyncSource) {
        ReplicaSetConfig config;

        ASSERT_OK(config.initialize(BSON("_id" << "rs0" <<
                                         "version" << 1 <<
                                         "members" << BSON_ARRAY(
                                             BSON("_id" << 10 << "host" << "hself") <<
                                             BSON("_id" << 20 << "host" << "h2") <<
                                             BSON("_id" << 30 << "host" << "h3")))));

        TopologyCoordinatorImpl topocoord((Seconds(999)));
        Date_t now = 0;
        topocoord.updateConfig(config, 0, now++, OpTime(0,0));

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
        topocoord.recordPing(HostAndPort("h2"), Milliseconds(300));
        topocoord.recordPing(HostAndPort("h2"), Milliseconds(300));
        topocoord.recordPing(HostAndPort("h3"), Milliseconds(100));
        topocoord.recordPing(HostAndPort("h3"), Milliseconds(100));

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

    TEST(TopologyCoordinator, PrepareSyncFromResponse) {
        TopologyCoordinatorImpl topocoord((Seconds(999)));
        Date_t now = 0;
        
        // Test trying to sync from another node when we are an arbiter
        ReplicaSetConfig config1;
        ASSERT_OK(config1.initialize(BSON("_id" << "rs0" <<
                                         "version" << 1 <<
                                         "members" << BSON_ARRAY(BSON("_id" << 0 <<
                                                                      "host" << "hself" <<
                                                                      "arbiterOnly" << true) <<
                                                                 BSON("_id" << 1 <<
                                                                      "host" << "h1")))));
        ASSERT_OK(config1.validate());
        topocoord.updateConfig(config1, 0, now++, OpTime(0,0));

        OpTime staleOpTime(1, 1);
        OpTime ourOpTime(staleOpTime.getSecs() + 11, 1);
         
        Status result = Status::OK();
        BSONObjBuilder response;
        ReplicationExecutor::CallbackHandle cbh;
        ReplicationExecutor::CallbackData cbData(NULL,
                                                 cbh,
                                                 Status::OK());
        topocoord.prepareSyncFromResponse(cbData, HostAndPort("h1"), ourOpTime, &response, &result);
        ASSERT_EQUALS(ErrorCodes::NotSecondary, result);
        ASSERT_EQUALS("arbiters don't sync", result.reason());

        // Set up config for the rest of the tests
        ReplicaSetConfig config2;
        ASSERT_OK(config2.initialize(BSON("_id" << "rs0" <<
                                         "version" << 1 <<
                                         "members" << BSON_ARRAY(
                                                 BSON("_id" << 0 << "host" << "hself") <<
                                                 BSON("_id" << 1 <<
                                                      "host" << "h1" <<
                                                      "arbiterOnly" << true) <<
                                                 BSON("_id" << 2 <<
                                                      "host" << "h2" <<
                                                      "priority" << 0 <<
                                                      "buildIndexes" << false) <<
                                                 BSON("_id" << 3 << "host" << "h3") <<
                                                 BSON("_id" << 4 << "host" << "h4") <<
                                                 BSON("_id" << 5 << "host" << "h5") <<
                                                 BSON("_id" << 6 << "host" << "h6")))));
        ASSERT_OK(config2.validate());
        topocoord.updateConfig(config2, 0, now++, OpTime(0,0));

        // Try to sync while PRIMARY
        topocoord._setCurrentPrimaryForTest(0);
        BSONObjBuilder response1;
        topocoord.prepareSyncFromResponse(
                cbData, HostAndPort("h3"), ourOpTime, &response1, &result);
        ASSERT_EQUALS(ErrorCodes::NotSecondary, result);
        ASSERT_EQUALS("primaries don't sync", result.reason());
        ASSERT_EQUALS("h3:27017", response1.obj()["syncFromRequested"].String());

        // Try to sync from non-existent member
        topocoord._setCurrentPrimaryForTest(-1);
        BSONObjBuilder response2;
        topocoord.prepareSyncFromResponse(
                cbData, HostAndPort("fakemember"), ourOpTime, &response2, &result);
        ASSERT_EQUALS(ErrorCodes::NodeNotFound, result);
        ASSERT_EQUALS("Could not find member \"fakemember:27017\" in replica set", result.reason());

        // Try to sync from self
        BSONObjBuilder response3;
        topocoord.prepareSyncFromResponse(
                cbData, HostAndPort("hself"), ourOpTime, &response3, &result);
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, result);
        ASSERT_EQUALS("I cannot sync from myself", result.reason());

        // Try to sync from an arbiter
        BSONObjBuilder response4;
        topocoord.prepareSyncFromResponse(
                cbData, HostAndPort("h1"), ourOpTime, &response4, &result);
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, result);
        ASSERT_EQUALS("Cannot sync from \"h1:27017\" because it is an arbiter", result.reason());

        // Try to sync from a node that doesn't build indexes
        BSONObjBuilder response5;
        topocoord.prepareSyncFromResponse(
                cbData, HostAndPort("h2"), ourOpTime, &response5, &result);
        ASSERT_EQUALS(ErrorCodes::InvalidOptions, result);
        ASSERT_EQUALS("Cannot sync from \"h2:27017\" because it does not build indexes",
                      result.reason());

        // Try to sync from a node we can't authenticate to
        MemberHeartbeatData h3Info(0);
        h3Info.setAuthIssue();
        topocoord.updateHeartbeatData(now++, h3Info, 3, OpTime(0,0));

        BSONObjBuilder response6;
        topocoord.prepareSyncFromResponse(
                cbData, HostAndPort("h3"), ourOpTime, &response6, &result);
        ASSERT_EQUALS(ErrorCodes::Unauthorized, result);
        ASSERT_EQUALS("not authorized to communicate with h3:27017", result.reason());

        // Try to sync from a member that is down
        MemberHeartbeatData h4Info(1);
        h4Info.setDownValues(now, "");
        topocoord.updateHeartbeatData(now++, h4Info, 4, OpTime(0,0));

        BSONObjBuilder response7;
        topocoord.prepareSyncFromResponse(
                cbData, HostAndPort("h4"), ourOpTime, &response7, &result);
        ASSERT_EQUALS(ErrorCodes::HostUnreachable, result);
        ASSERT_EQUALS("I cannot reach the requested member: h4:27017", result.reason());

        // Sync successfully from a member that is stale
        MemberHeartbeatData h5Info(2);
        h5Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), staleOpTime, "", "");
        topocoord.updateHeartbeatData(now++, h5Info, 5, OpTime(0,0));

        BSONObjBuilder response8;
        topocoord.prepareSyncFromResponse(
                cbData, HostAndPort("h5"), ourOpTime, &response8, &result);
        ASSERT_OK(result);
        ASSERT_EQUALS("requested member \"h5:27017\" is more than 10 seconds behind us",
                      response8.obj()["warning"].String());
        topocoord.chooseNewSyncSource(now++, ourOpTime);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h5"));

        // Sync successfully from an up-to-date member
        MemberHeartbeatData h6Info(2);
        h6Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), ourOpTime, "", "");
        topocoord.updateHeartbeatData(now++, h6Info, 6, OpTime(0,0));

        BSONObjBuilder response9;
        topocoord.prepareSyncFromResponse(
                cbData, HostAndPort("h6"), ourOpTime, &response9, &result);
        ASSERT_OK(result);
        BSONObj response9Obj = response9.obj();
        ASSERT_FALSE(response9Obj.hasField("warning"));
        ASSERT_EQUALS(HostAndPort("h5").toString(), response9Obj["prevSyncTarget"].String());
        topocoord.chooseNewSyncSource(now++, ourOpTime);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(), HostAndPort("h6"));
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
        TopologyCoordinatorImpl topocoord((Seconds(999)));

        ReplicaSetConfig config;
        ASSERT_OK(config.initialize(
                BSON("_id" << "mySet" <<
                     "version" << 1 <<
                     "members" << BSON_ARRAY(BSON("_id" << 0 << "host" << "test0:1234") <<
                                             BSON("_id" << 1 << "host" << "test1:1234") <<
                                             BSON("_id" << 2 << "host" << "test2:1234") <<
                                             BSON("_id" << 3 << "host" << "test3:1234")))));
        topocoord.updateConfig(config, 3, startupTime + 1, OpTime(0,0));

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
        ReplicationExecutor::CallbackHandle cbh;
        ReplicationExecutor::CallbackData cbData(NULL,
                                                 cbh,
                                                 Status::OK());
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

    TEST(TopologyCoordinator, PrepareFreshResponse) {
        ReplicationExecutor::CallbackHandle cbh;
        ReplicationExecutor::CallbackData cbData(NULL,
                                                 cbh,
                                                 Status::OK());
        ReplicaSetConfig config;

        ASSERT_OK(config.initialize(BSON("_id" << "rs0" <<
                                         "version" << 10 <<
                                         "members" << BSON_ARRAY(
                                             BSON("_id" << 10 <<
                                                  "host" << "hself" <<
                                                  "priority" << 10) <<
                                             BSON("_id" << 20 << "host" << "h1") <<
                                             BSON("_id" << 30 << "host" << "h2") <<
                                             BSON("_id" << 40 <<
                                                  "host" << "h3" <<
                                                  "priority" << 10)))));
        ASSERT_OK(config.validate());

        TopologyCoordinatorImpl topocoord((Seconds(999)));
        Date_t now = 0;
        topocoord.updateConfig(config, 0, now++, OpTime(0,0));

        OpTime ourOpTime(10, 10);
        OpTime staleOpTime(1, 1);

        Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

        // Test with incorrect replset name
        ReplicationCoordinator::ReplSetFreshArgs args;
        args.setName = "fakeset";

        BSONObjBuilder responseBuilder0;
        Status status0 = internalErrorStatus;
        topocoord.prepareFreshResponse(cbData, args, ourOpTime, &responseBuilder0, &status0);
        ASSERT_EQUALS(ErrorCodes::ReplicaSetNotFound, status0);
        ASSERT_TRUE(responseBuilder0.obj().isEmpty());


        // Test with non-existent node.
        args.setName = "rs0";
        args.cfgver = 5; // stale config
        args.id = 0;
        args.who = HostAndPort("fakenode");
        args.opTime = staleOpTime;

        BSONObjBuilder responseBuilder1;
        Status status1 = internalErrorStatus;
        topocoord.prepareFreshResponse(cbData, args, ourOpTime, &responseBuilder1, &status1);
        ASSERT_OK(status1);
        BSONObj response1 = responseBuilder1.obj();
        ASSERT_EQUALS("config version stale", response1["info"].String());
        ASSERT_EQUALS(ourOpTime, OpTime(response1["opTime"].timestampValue()));
        ASSERT_TRUE(response1["fresher"].Bool());
        ASSERT_TRUE(response1["veto"].Bool());
        ASSERT_EQUALS("replSet couldn't find member with id 0", response1["errmsg"].String());


        // Test when we are primary and target node is stale.
        args.id = 20;
        args.cfgver = 10;
        args.who = HostAndPort("h1");
        args.opTime = ourOpTime;

        MemberHeartbeatData h1Info(1);
        h1Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0, 0), staleOpTime, "", "");
        topocoord.updateHeartbeatData(now++, h1Info, 20, ourOpTime);

        topocoord._setCurrentPrimaryForTest(0);

        BSONObjBuilder responseBuilder2;
        Status status2 = internalErrorStatus;
        topocoord.prepareFreshResponse(cbData, args, ourOpTime, &responseBuilder2, &status2);
        ASSERT_OK(status2);
        BSONObj response2 = responseBuilder2.obj();
        ASSERT_FALSE(response2.hasField("info"));
        ASSERT_EQUALS(ourOpTime, OpTime(response2["opTime"].timestampValue()));
        ASSERT_FALSE(response2["fresher"].Bool());
        ASSERT_TRUE(response2["veto"].Bool());
        ASSERT_EQUALS("I am already primary, h1:27017 can try again once I've stepped down",
                      response2["errmsg"].String());


        // Test when someone else is primary and target node is stale.
        MemberHeartbeatData h2Info(2);
        h2Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0, 0), ourOpTime, "", "");
        topocoord.updateHeartbeatData(now++, h2Info, 30, ourOpTime);

        topocoord._changeMemberState(MemberState::RS_SECONDARY);
        topocoord._setCurrentPrimaryForTest(2);

        BSONObjBuilder responseBuilder3;
        Status status3 = internalErrorStatus;
        topocoord.prepareFreshResponse(cbData, args, ourOpTime, &responseBuilder3, &status3);
        ASSERT_OK(status3);
        BSONObj response3 = responseBuilder3.obj();
        ASSERT_FALSE(response3.hasField("info"));
        ASSERT_EQUALS(ourOpTime, OpTime(response3["opTime"].timestampValue()));
        ASSERT_FALSE(response3["fresher"].Bool());
        ASSERT_TRUE(response3["veto"].Bool());
        ASSERT_EQUALS(
                "h1:27017 is trying to elect itself but h2:27017 is already primary and more "
                        "up-to-date",
                response3["errmsg"].String());


        // Test trying to elect a node that is caught up but isn't the highest priority node.
        h1Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0, 0), ourOpTime, "", "");
        topocoord.updateHeartbeatData(now++, h1Info, 20, ourOpTime);
        h2Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0, 0), staleOpTime, "", "");
        topocoord.updateHeartbeatData(now++, h2Info, 30, ourOpTime);
        MemberHeartbeatData h3Info(3);
        h3Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0, 0), ourOpTime, "", "");
        topocoord.updateHeartbeatData(now++, h3Info, 40, ourOpTime);

        BSONObjBuilder responseBuilder4;
        Status status4 = internalErrorStatus;
        topocoord.prepareFreshResponse(cbData, args, ourOpTime, &responseBuilder4, &status4);
        ASSERT_OK(status4);
        BSONObj response4 = responseBuilder4.obj();
        ASSERT_FALSE(response4.hasField("info"));
        ASSERT_EQUALS(ourOpTime, OpTime(response4["opTime"].timestampValue()));
        ASSERT_FALSE(response4["fresher"].Bool());
        ASSERT_TRUE(response4["veto"].Bool());
        ASSERT_EQUALS("h1:27017 has lower priority of 1 than h3:27017 which has a priority of 10",
                      response4["errmsg"].String());


        // Test trying to elect a node that isn't electable
        args.id = 40;
        args.who = HostAndPort("h3");

        h3Info.setDownValues(now, "");
        topocoord.updateHeartbeatData(now++, h3Info, 40, ourOpTime);

        BSONObjBuilder responseBuilder5;
        Status status5 = internalErrorStatus;
        topocoord.prepareFreshResponse(cbData, args, ourOpTime, &responseBuilder5, &status5);
        ASSERT_OK(status5);
        BSONObj response5 = responseBuilder5.obj();
        ASSERT_FALSE(response5.hasField("info"));
        ASSERT_EQUALS(ourOpTime, OpTime(response5["opTime"].timestampValue()));
        ASSERT_FALSE(response5["fresher"].Bool());
        ASSERT_TRUE(response5["veto"].Bool());
        ASSERT_EQUALS(
            "I don't think h3:27017 is electable because the member is not currently a secondary",
            response5["errmsg"].String());


        // Finally, test trying to elect a valid node
        args.id = 40;
        args.who = HostAndPort("h3");

        h3Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0, 0), ourOpTime, "", "");
        topocoord.updateHeartbeatData(now++, h3Info, 40, ourOpTime);

        BSONObjBuilder responseBuilder6;
        Status status6 = internalErrorStatus;
        topocoord.prepareFreshResponse(cbData, args, ourOpTime, &responseBuilder6, &status6);
        ASSERT_OK(status6);
        BSONObj response6 = responseBuilder6.obj();
        cout << response6.jsonString(TenGen, 1);
        ASSERT_FALSE(response6.hasField("info")) << response6.toString();
        ASSERT_EQUALS(ourOpTime, OpTime(response6["opTime"].timestampValue()));
        ASSERT_FALSE(response6["fresher"].Bool()) << response6.toString();
        ASSERT_FALSE(response6["veto"].Bool()) << response6.toString();
        ASSERT_FALSE(response6.hasField("errmsg")) << response6.toString();
    }

    class TopoCoordTest : public mongo::unittest::Test {
    public:
        virtual void setUp() {
            _topo = new TopologyCoordinatorImpl(Seconds(100));
            _now = 0;
        }

        virtual void tearDown() {
            delete _topo;
        }

    protected:
        TopologyCoordinatorImpl& getTopoCoord() {return *_topo;}
        Date_t& now() {return _now;}

        int64_t countLogLinesContaining(const std::string& needle) {
            return std::count_if(getCapturedLogMessages().begin(),
                                 getCapturedLogMessages().end(),
                                 stdx::bind(stringContains,
                                            stdx::placeholders::_1,
                                            needle));
        }

        // Update config and set selfIndex
        void updateConfig(BSONObj cfg, int selfIndex) {
            ReplicaSetConfig config;
            ASSERT_OK(config.initialize(cfg));
            ASSERT_OK(config.validate());

            getTopoCoord().updateConfig(config, selfIndex, ++_now, OpTime(0,0));
        }

    private:
        TopologyCoordinatorImpl* _topo;
        Date_t _now;
    };

    TEST_F(TopoCoordTest, UpdateHeartbeatDataOlderConfigVersionNoMajority) {
        updateConfig(BSON("_id" << "rs0" <<
                          "version" << 5 <<
                          "members" << BSON_ARRAY(
                              BSON("_id" << 0 << "host" << "host1:27017") <<
                              BSON("_id" << 1 << "host" << "host2:27017"))),
                     0);

        OpTime staleOpTime = OpTime(1,0);
        OpTime election = OpTime(5,0);
        OpTime lastOpTimeApplied = OpTime(3,0);

        MemberHeartbeatData h1Info(1);
        h1Info.setUpValues(now(), MemberState::RS_SECONDARY, election, staleOpTime, "", "");
        ReplSetHeartbeatResponse::HeartbeatResultAction nextAction = 
                getTopoCoord().updateHeartbeatData(now()++, h1Info, 1, lastOpTimeApplied);
        ASSERT_EQUALS(nextAction, ReplSetHeartbeatResponse::NoAction);
    }

    TEST(TopologyCoordinator, UpdateHeartbeatDataNewPrimary) {}

    TEST(TopologyCoordinator, UpdateHeartbeatDataTwoPrimariesNewOneOlder) {}
    TEST(TopologyCoordinator, UpdateHeartbeatDataTwoPrimariesNewOneNewer) {}

    TEST(TopologyCoordinator, UpdateHeartbeatDataTwoPrimariesIncludingMeNewOneOlder) {}
    TEST(TopologyCoordinator, UpdateHeartbeatDataTwoPrimariesIncludingMeNewOneNewer) {}

    TEST(TopologyCoordinator, UpdateHeartbeatDataPrimaryDownMajorityButIAmStarting) {}
    TEST(TopologyCoordinator, UpdateHeartbeatDataPrimaryDownMajorityButIAmRecovering) {}
    TEST(TopologyCoordinator, UpdateHeartbeatDataPrimaryDownMajorityButIHaveStepdownWait) {}
    TEST(TopologyCoordinator, UpdateHeartbeatDataPrimaryDownMajorityButIArbiter) {}

    TEST(TopologyCoordinator, UpdateHeartbeatDataPrimaryDownMajority) {}
    TEST(TopologyCoordinator, UpdateHeartbeatDataPrimaryDownNoMajority) {}

    class PrepareElectResponseTest : public TopoCoordTest {
    public:

        PrepareElectResponseTest() :
            now(0),
            round(OID::gen()),
            cbData(NULL, ReplicationExecutor::CallbackHandle(), Status::OK()) {}

        virtual void setUp() {
            TopoCoordTest::setUp();
            updateConfig(BSON("_id" << "rs0" <<
                              "version" << 10 <<
                              "members" << BSON_ARRAY(
                                  BSON("_id" << 0 << "host" << "hself") <<
                                  BSON("_id" << 1 << "host" << "h1") <<
                                  BSON("_id" << 2 <<
                                       "host" << "h2" <<
                                       "priority" << 10) <<
                                  BSON("_id" << 3 <<
                                       "host" << "h3" <<
                                       "priority" << 10))),
                         0);
        }

    protected:
        Date_t now;
        OID round;
        ReplicationExecutor::CallbackData cbData;
    };

    TEST_F(PrepareElectResponseTest, IncorrectReplSetName) {
        // Test with incorrect replset name
        ReplicationCoordinator::ReplSetElectArgs args;
        args.set = "fakeset";
        args.round = round;

        BSONObjBuilder responseBuilder;
        startCapturingLogMessages();
        getTopoCoord().prepareElectResponse(cbData, args, now++, &responseBuilder);
        stopCapturingLogMessages();
        BSONObj response = responseBuilder.obj();
        ASSERT_EQUALS(0, response["vote"].Int());
        ASSERT_EQUALS(round, response["round"].OID());
        ASSERT_EQUALS(1,
                      countLogLinesContaining("received an elect request for 'fakeset' but our "
                              "set name is 'rs0'"));
    }

    TEST_F(PrepareElectResponseTest, OurConfigStale) {
        // Test with us having a stale config version
        ReplicationCoordinator::ReplSetElectArgs args;
        args.set = "rs0";
        args.round = round;
        args.cfgver = 20;

        BSONObjBuilder responseBuilder;
        startCapturingLogMessages();
        getTopoCoord().prepareElectResponse(cbData, args, now++, &responseBuilder);
        stopCapturingLogMessages();
        BSONObj response = responseBuilder.obj();
        ASSERT_EQUALS(0, response["vote"].Int());
        ASSERT_EQUALS(round, response["round"].OID());
        ASSERT_EQUALS(1,
                      countLogLinesContaining("not voting because our config version is stale"));
    }

    TEST_F(PrepareElectResponseTest, TheirConfigStale) {
        // Test with them having a stale config version
        ReplicationCoordinator::ReplSetElectArgs args;
        args.set = "rs0";
        args.round = round;
        args.cfgver = 5;

        BSONObjBuilder responseBuilder;
        startCapturingLogMessages();
        getTopoCoord().prepareElectResponse(cbData, args, now++, &responseBuilder);
        stopCapturingLogMessages();
        BSONObj response = responseBuilder.obj();
        ASSERT_EQUALS(-10000, response["vote"].Int());
        ASSERT_EQUALS(round, response["round"].OID());
        ASSERT_EQUALS(1,
                      countLogLinesContaining("received stale config version # during election"));
    }

    TEST_F(PrepareElectResponseTest, NonExistentNode) {
        // Test with a non-existent node
        ReplicationCoordinator::ReplSetElectArgs args;
        args.set = "rs0";
        args.round = round;
        args.cfgver = 10;
        args.whoid = 99;

        BSONObjBuilder responseBuilder;
        startCapturingLogMessages();
        getTopoCoord().prepareElectResponse(cbData, args, now++, &responseBuilder);
        stopCapturingLogMessages();
        BSONObj response = responseBuilder.obj();
        ASSERT_EQUALS(-10000, response["vote"].Int());
        ASSERT_EQUALS(round, response["round"].OID());
        ASSERT_EQUALS(1, countLogLinesContaining("couldn't find member with id 99"));
    }

    TEST_F(PrepareElectResponseTest, WeArePrimary) {
        // Test when we are already primary
        ReplicationCoordinator::ReplSetElectArgs args;
        args.set = "rs0";
        args.round = round;
        args.cfgver = 10;
        args.whoid = 1;

        getTopoCoord()._setCurrentPrimaryForTest(0);

        BSONObjBuilder responseBuilder;
        startCapturingLogMessages();
        getTopoCoord().prepareElectResponse(cbData, args, now++, &responseBuilder);
        stopCapturingLogMessages();
        BSONObj response = responseBuilder.obj();
        ASSERT_EQUALS(-10000, response["vote"].Int());
        ASSERT_EQUALS(round, response["round"].OID());
        ASSERT_EQUALS(1, countLogLinesContaining("I am already primary"));
    }

    TEST_F(PrepareElectResponseTest, SomeoneElseIsPrimary) {
        // Test when someone else is already primary
        ReplicationCoordinator::ReplSetElectArgs args;
        args.set = "rs0";
        args.round = round;
        args.cfgver = 10;
        args.whoid = 1;
        getTopoCoord()._setCurrentPrimaryForTest(2);

        BSONObjBuilder responseBuilder;
        startCapturingLogMessages();
        getTopoCoord().prepareElectResponse(cbData, args, now++, &responseBuilder);
        stopCapturingLogMessages();
        BSONObj response = responseBuilder.obj();
        ASSERT_EQUALS(-10000, response["vote"].Int());
        ASSERT_EQUALS(round, response["round"].OID());
        ASSERT_EQUALS(1, countLogLinesContaining("h2:27017 is already primary"));
    }

    TEST_F(PrepareElectResponseTest, NotHighestPriority) {
        // Test trying to elect someone who isn't the highest priority node
        ReplicationCoordinator::ReplSetElectArgs args;
        args.set = "rs0";
        args.round = round;
        args.cfgver = 10;
        args.whoid = 1;

        MemberHeartbeatData h3Info(3);
        h3Info.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0, 0), jsTime(), "", "");
        getTopoCoord().updateHeartbeatData(now++, h3Info, 3, OpTime(0, 0));

        BSONObjBuilder responseBuilder;
        startCapturingLogMessages();
        getTopoCoord().prepareElectResponse(cbData, args, now++, &responseBuilder);
        stopCapturingLogMessages();
        BSONObj response = responseBuilder.obj();
        ASSERT_EQUALS(-10000, response["vote"].Int());
        ASSERT_EQUALS(round, response["round"].OID());
        ASSERT_EQUALS(1, countLogLinesContaining("h1:27017 has lower priority than h3:27017"));
    }

    TEST_F(PrepareElectResponseTest, ValidVotes) {
        // Test a valid vote
        ReplicationCoordinator::ReplSetElectArgs args;
        args.set = "rs0";
        args.round = round;
        args.cfgver = 10;
        args.whoid = 2;
        now = 100;

        BSONObjBuilder responseBuilder1;
        startCapturingLogMessages();
        getTopoCoord().prepareElectResponse(cbData, args, now++, &responseBuilder1);
        stopCapturingLogMessages();
        BSONObj response1 = responseBuilder1.obj();
        ASSERT_EQUALS(1, response1["vote"].Int());
        ASSERT_EQUALS(round, response1["round"].OID());
        ASSERT_EQUALS(1, countLogLinesContaining("voting yea for h2:27017 (2)"));

        // Test what would be a valid vote except that we already voted too recently
        args.whoid = 3;

        BSONObjBuilder responseBuilder2;
        startCapturingLogMessages();
        getTopoCoord().prepareElectResponse(cbData, args, now++, &responseBuilder2);
        stopCapturingLogMessages();
        BSONObj response2 = responseBuilder2.obj();
        ASSERT_EQUALS(0, response2["vote"].Int());
        ASSERT_EQUALS(round, response2["round"].OID());
        ASSERT_EQUALS(1, countLogLinesContaining("voting no for h3:27017; "
                "voted for h2:27017 0 secs ago"));

        // Test that after enough time passes the same vote can proceed
        now = Date_t(now.millis + 3 * 1000); // 3 seconds later

        BSONObjBuilder responseBuilder3;
        startCapturingLogMessages();
        getTopoCoord().prepareElectResponse(cbData, args, now++, &responseBuilder3);
        stopCapturingLogMessages();
        BSONObj response3 = responseBuilder3.obj();
        ASSERT_EQUALS(1, response3["vote"].Int());
        ASSERT_EQUALS(round, response3["round"].OID());
        ASSERT_EQUALS(1, countLogLinesContaining("voting yea for h3:27017 (3)"));
    }

}  // namespace
}  // namespace repl
}  // namespace mongo
