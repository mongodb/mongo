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

#include <limits>

#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

namespace {

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
                                             BSON("_id" << 10 << "host" << "h1") <<
                                             BSON("_id" << 20 << "host" << "h2") <<
                                             BSON("_id" << 30 << "host" << "h3")))));

        TopologyCoordinatorImpl topocoord((Seconds(std::numeric_limits<long>::max())));
        Date_t now = 0;
        topocoord.updateConfig(cbData, config, 0, now++);

        MemberHeartbeatData newInfo0(0);
        newInfo0.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, newInfo0, 10);

        MemberHeartbeatData newInfo1(1);
        newInfo1.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(1,0), "", ""); 
        topocoord.updateHeartbeatData(now++, newInfo1, 20);

        MemberHeartbeatData newInfo2(2);
        newInfo2.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, newInfo2, 30);

        // Record 2N pings to allow choosing a new sync source; h2 is the closest.
        topocoord.recordPing(HostAndPort("h1"), 300);
        topocoord.recordPing(HostAndPort("h1"), 300);
        topocoord.recordPing(HostAndPort("h2"), 100);
        topocoord.recordPing(HostAndPort("h2"), 100);
        topocoord.recordPing(HostAndPort("h3"), 300);
        topocoord.recordPing(HostAndPort("h3"), 300);

        // No primary situation: should choose no sync source.
        topocoord.chooseNewSyncSource(now++);
        ASSERT(topocoord.getSyncSourceAddress().empty());
        
        // Add primary
        MemberHeartbeatData newInfo3(2);
        newInfo2.setUpValues(now, MemberState::RS_PRIMARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, newInfo2, 30);

        // h3 is primary and should be chosen as sync source, despite being further away than h2.
        topocoord.chooseNewSyncSource(now++);
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
                                             BSON("_id" << 10 << "host" << "h1") <<
                                             BSON("_id" << 20 << "host" << "h2") <<
                                             BSON("_id" << 30 << "host" << "h3")))));

        TopologyCoordinatorImpl topocoord((Seconds(std::numeric_limits<long>::max())));
        Date_t now = 0;
        topocoord.updateConfig(cbData, config, 0, now++);

        MemberHeartbeatData newInfo0(0);
        newInfo0.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, newInfo0, 10);

        MemberHeartbeatData newInfo1(1);
        newInfo1.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(1,0), "", ""); 
        topocoord.updateHeartbeatData(now++, newInfo1, 20);

        MemberHeartbeatData newInfo2(2);
        newInfo2.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(2,0), "", ""); 
        topocoord.updateHeartbeatData(now++, newInfo2, 30);

        // Record 2N pings to allow choosing a new sync source; h3 is the closest.
        topocoord.recordPing(HostAndPort("h1"), 300);
        topocoord.recordPing(HostAndPort("h1"), 300);
        topocoord.recordPing(HostAndPort("h2"), 300);
        topocoord.recordPing(HostAndPort("h2"), 300);
        topocoord.recordPing(HostAndPort("h3"), 100);
        topocoord.recordPing(HostAndPort("h3"), 100);

        topocoord.chooseNewSyncSource(now++);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h3"));
        topocoord.setForceSyncSourceIndex(0);
        topocoord.chooseNewSyncSource(now++);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h1"));
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
                                             BSON("_id" << 10 << "host" << "h1") <<
                                             BSON("_id" << 20 << "host" << "h2") <<
                                             BSON("_id" << 30 << "host" << "h3")))));

        TopologyCoordinatorImpl topocoord((Seconds(std::numeric_limits<long>::max())));
        Date_t now = 0;
        topocoord.updateConfig(cbData, config, 0, now++);

        MemberHeartbeatData newInfo0(0);
        newInfo0.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(0,0), "", ""); 
        topocoord.updateHeartbeatData(now++, newInfo0, 10);

        MemberHeartbeatData newInfo1(1);
        newInfo1.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(1,0), "", ""); 
        topocoord.updateHeartbeatData(now++, newInfo1, 20);

        MemberHeartbeatData newInfo2(2);
        newInfo2.setUpValues(now, MemberState::RS_SECONDARY, OpTime(0,0), OpTime(2,0), "", ""); 
        topocoord.updateHeartbeatData(now++, newInfo2, 30);

        // Record 2N pings to allow choosing a new sync source; h3 is the closest.
        topocoord.recordPing(HostAndPort("h1"), 300);
        topocoord.recordPing(HostAndPort("h1"), 300);
        topocoord.recordPing(HostAndPort("h2"), 300);
        topocoord.recordPing(HostAndPort("h2"), 300);
        topocoord.recordPing(HostAndPort("h3"), 100);
        topocoord.recordPing(HostAndPort("h3"), 100);

        topocoord.chooseNewSyncSource(now++);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h3"));
        
        Date_t expireTime = 100;
        topocoord.blacklistSyncSource(HostAndPort("h3"), expireTime);
        topocoord.chooseNewSyncSource(now++);
        // Should choose second best choice now that h3 is blacklisted.
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h2"));

        // After time has passed, should go back to original sync source
        topocoord.chooseNewSyncSource(expireTime);
        ASSERT_EQUALS(topocoord.getSyncSourceAddress(),HostAndPort("h3"));
    }


}  // namespace
}  // namespace repl
}  // namespace mongo
