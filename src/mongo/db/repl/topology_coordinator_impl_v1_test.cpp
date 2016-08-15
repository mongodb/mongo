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

#include <iostream>

#include "mongo/bson/json.h"
#include "mongo/db/repl/heartbeat_response_action.h"
#include "mongo/db/repl/member_heartbeat_data.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/db/repl/topology_coordinator_impl.h"
#include "mongo/db/server_options.h"
#include "mongo/logger/logger.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#define ASSERT_NO_ACTION(EXPRESSION) \
    ASSERT_EQUALS(mongo::repl::HeartbeatResponseAction::NoAction, (EXPRESSION))

using std::unique_ptr;
using mongo::rpc::ReplSetMetadata;

namespace mongo {
namespace repl {
namespace {

Date_t operator++(Date_t& d, int) {
    Date_t result = d;
    d += Milliseconds(1);
    return result;
}

bool stringContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

class TopoCoordTest : public mongo::unittest::Test {
public:
    virtual void setUp() {
        _options = TopologyCoordinatorImpl::Options{};
        _options.maxSyncSourceLagSecs = Seconds{100};
        _topo.reset(new TopologyCoordinatorImpl(_options));
        _now = Date_t();
        _selfIndex = -1;
        _cbData.reset(new ReplicationExecutor::CallbackArgs(
            NULL, ReplicationExecutor::CallbackHandle(), Status::OK()));
    }

    virtual void tearDown() {
        _topo.reset(NULL);
        _cbData.reset(NULL);
    }

protected:
    TopologyCoordinatorImpl& getTopoCoord() {
        return *_topo;
    }
    ReplicationExecutor::CallbackArgs cbData() {
        return *_cbData;
    }
    Date_t& now() {
        return _now;
    }

    void setOptions(const TopologyCoordinatorImpl::Options& options) {
        _options = options;
        _topo.reset(new TopologyCoordinatorImpl(_options));
    }

    int64_t countLogLinesContaining(const std::string& needle) {
        return std::count_if(getCapturedLogMessages().begin(),
                             getCapturedLogMessages().end(),
                             stdx::bind(stringContains, stdx::placeholders::_1, needle));
    }

    void makeSelfPrimary(const Timestamp& electionOpTime = Timestamp(0, 0)) {
        getTopoCoord().changeMemberState_forTest(MemberState::RS_PRIMARY, electionOpTime);
        getTopoCoord()._setCurrentPrimaryForTest(_selfIndex);
    }

    void setSelfMemberState(const MemberState& newState) {
        getTopoCoord().changeMemberState_forTest(newState);
    }

    int getCurrentPrimaryIndex() {
        return getTopoCoord().getCurrentPrimaryIndex();
    }

    HostAndPort getCurrentPrimaryHost() {
        return _currentConfig.getMemberAt(getTopoCoord().getCurrentPrimaryIndex()).getHostAndPort();
    }

    // Update config and set selfIndex
    // If "now" is passed in, set _now to now+1
    void updateConfig(BSONObj cfg,
                      int selfIndex,
                      Date_t now = Date_t::fromMillisSinceEpoch(-1),
                      const OpTime& lastOp = OpTime()) {
        ReplicaSetConfig config;
        // Use Protocol version 1 by default.
        ASSERT_OK(config.initialize(cfg, true));
        ASSERT_OK(config.validate());

        _selfIndex = selfIndex;

        if (now == Date_t::fromMillisSinceEpoch(-1)) {
            getTopoCoord().updateConfig(config, selfIndex, _now, lastOp);
            _now += Milliseconds(1);
        } else {
            invariant(now > _now);
            getTopoCoord().updateConfig(config, selfIndex, now, lastOp);
            _now = now + Milliseconds(1);
        }

        _currentConfig = config;
    }

    // Make the metadata coming from sync source.
    // Only set visibleOpTime, primaryIndex and syncSourceIndex
    ReplSetMetadata makeMetadata(OpTime opTime = OpTime(),
                                 int primaryIndex = -1,
                                 int syncSourceIndex = -1) {
        return ReplSetMetadata(_topo->getTerm(),
                               OpTime(),
                               opTime,
                               _currentConfig.getConfigVersion(),
                               OID(),
                               primaryIndex,
                               syncSourceIndex);
    }

    HeartbeatResponseAction receiveUpHeartbeat(const HostAndPort& member,
                                               const std::string& setName,
                                               MemberState memberState,
                                               const OpTime& electionTime,
                                               const OpTime& lastOpTimeSender,
                                               const OpTime& lastOpTimeReceiver) {
        return _receiveHeartbeatHelper(Status::OK(),
                                       member,
                                       setName,
                                       memberState,
                                       electionTime.getTimestamp(),
                                       lastOpTimeSender,
                                       lastOpTimeReceiver,
                                       Milliseconds(1));
    }

    HeartbeatResponseAction receiveDownHeartbeat(
        const HostAndPort& member,
        const std::string& setName,
        const OpTime& lastOpTimeReceiver,
        ErrorCodes::Error errcode = ErrorCodes::HostUnreachable) {
        // timed out heartbeat to mark a node as down

        Milliseconds roundTripTime{ReplicaSetConfig::kDefaultHeartbeatTimeoutPeriod};
        return _receiveHeartbeatHelper(Status(errcode, ""),
                                       member,
                                       setName,
                                       MemberState::RS_UNKNOWN,
                                       Timestamp(),
                                       OpTime(),
                                       lastOpTimeReceiver,
                                       roundTripTime);
    }

    HeartbeatResponseAction heartbeatFromMember(const HostAndPort& member,
                                                const std::string& setName,
                                                MemberState memberState,
                                                const OpTime& lastOpTimeSender,
                                                Milliseconds roundTripTime = Milliseconds(1)) {
        return _receiveHeartbeatHelper(Status::OK(),
                                       member,
                                       setName,
                                       memberState,
                                       Timestamp(),
                                       lastOpTimeSender,
                                       OpTime(),
                                       roundTripTime);
    }

private:
    HeartbeatResponseAction _receiveHeartbeatHelper(Status responseStatus,
                                                    const HostAndPort& member,
                                                    const std::string& setName,
                                                    MemberState memberState,
                                                    Timestamp electionTime,
                                                    const OpTime& lastOpTimeSender,
                                                    const OpTime& lastOpTimeReceiver,
                                                    Milliseconds roundTripTime) {
        ReplSetHeartbeatResponse hb;
        hb.setConfigVersion(1);
        hb.setState(memberState);
        hb.setDurableOpTime(lastOpTimeSender);
        hb.setAppliedOpTime(lastOpTimeSender);
        hb.setElectionTime(electionTime);
        hb.setTerm(getTopoCoord().getTerm());

        StatusWith<ReplSetHeartbeatResponse> hbResponse = responseStatus.isOK()
            ? StatusWith<ReplSetHeartbeatResponse>(hb)
            : StatusWith<ReplSetHeartbeatResponse>(responseStatus);

        getTopoCoord().prepareHeartbeatRequestV1(now(), setName, member);
        now() += roundTripTime;
        return getTopoCoord().processHeartbeatResponse(
            now(), roundTripTime, member, hbResponse, lastOpTimeReceiver);
    }

private:
    unique_ptr<TopologyCoordinatorImpl> _topo;
    unique_ptr<ReplicationExecutor::CallbackArgs> _cbData;
    ReplicaSetConfig _currentConfig;
    Date_t _now;
    int _selfIndex;
    TopologyCoordinatorImpl::Options _options;
};

TEST_F(TopoCoordTest, NodeReturnsSecondaryWithMostRecentDataAsSyncSource) {
    // if we do not have an index in the config, we should get an empty syncsource
    HostAndPort newSyncSource = getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_TRUE(newSyncSource.empty());

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // member h2 is the furthest ahead
    heartbeatFromMember(
        HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(1, 0), 0));
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, OpTime());

    // We start with no sync source
    ASSERT(getTopoCoord().getSyncSourceAddress().empty());

    // Fail due to insufficient number of pings
    newSyncSource = getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(getTopoCoord().getSyncSourceAddress(), newSyncSource);
    ASSERT(getTopoCoord().getSyncSourceAddress().empty());

    // Record 2nd round of pings to allow choosing a new sync source; all members equidistant
    heartbeatFromMember(
        HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(1, 0), 0));
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, OpTime());

    // Should choose h2, since it is furthest ahead
    newSyncSource = getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(getTopoCoord().getSyncSourceAddress(), newSyncSource);
    ASSERT_EQUALS(HostAndPort("h2"), getTopoCoord().getSyncSourceAddress());

    // h3 becomes further ahead, so it should be chosen
    heartbeatFromMember(
        HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(2, 0), 0));
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h3"), getTopoCoord().getSyncSourceAddress());

    // h3 becomes an invalid candidate for sync source; should choose h2 again
    heartbeatFromMember(
        HostAndPort("h3"), "rs0", MemberState::RS_RECOVERING, OpTime(Timestamp(2, 0), 0));
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h2"), getTopoCoord().getSyncSourceAddress());

    // h3 back in SECONDARY and ahead
    heartbeatFromMember(
        HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(2, 0), 0));
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h3"), getTopoCoord().getSyncSourceAddress());

    // h3 goes down
    receiveDownHeartbeat(HostAndPort("h3"), "rs0", OpTime());
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h2"), getTopoCoord().getSyncSourceAddress());

    // h3 back up and ahead
    heartbeatFromMember(
        HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(2, 0), 0));
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h3"), getTopoCoord().getSyncSourceAddress());
}

TEST_F(TopoCoordTest, NodeReturnsClosestValidSyncSourceAsSyncSource) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                               << "hself")
                                    << BSON("_id" << 10 << "host"
                                                  << "h1")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2"
                                                  << "buildIndexes"
                                                  << false
                                                  << "priority"
                                                  << 0)
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"
                                                  << "hidden"
                                                  << true
                                                  << "priority"
                                                  << 0
                                                  << "votes"
                                                  << 0)
                                    << BSON("_id" << 40 << "host"
                                                  << "h4"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 50 << "host"
                                                  << "h5"
                                                  << "slaveDelay"
                                                  << 1
                                                  << "priority"
                                                  << 0)
                                    << BSON("_id" << 60 << "host"
                                                  << "h6")
                                    << BSON("_id" << 70 << "host"
                                                  << "hprimary"))),
                 0);

    setSelfMemberState(MemberState::RS_SECONDARY);
    Timestamp lastOpTimeWeApplied = Timestamp(100, 0);

    heartbeatFromMember(HostAndPort("h1"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(501, 0), 0),
                        Milliseconds(700));
    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(501, 0), 0),
                        Milliseconds(600));
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(501, 0), 0),
                        Milliseconds(500));
    heartbeatFromMember(HostAndPort("h4"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(501, 0), 0),
                        Milliseconds(400));
    heartbeatFromMember(HostAndPort("h5"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(501, 0), 0),
                        Milliseconds(300));

    // This node is lagged further than maxSyncSourceLagSeconds.
    heartbeatFromMember(HostAndPort("h6"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(499, 0), 0),
                        Milliseconds(200));

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    heartbeatFromMember(HostAndPort("hprimary"),
                        "rs0",
                        MemberState::RS_PRIMARY,
                        OpTime(Timestamp(600, 0), 0),
                        Milliseconds(100));
    ASSERT_EQUALS(7, getCurrentPrimaryIndex());

    // Record 2nd round of pings to allow choosing a new sync source
    heartbeatFromMember(HostAndPort("h1"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(501, 0), 0),
                        Milliseconds(700));
    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(501, 0), 0),
                        Milliseconds(600));
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(501, 0), 0),
                        Milliseconds(500));
    heartbeatFromMember(HostAndPort("h4"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(501, 0), 0),
                        Milliseconds(400));
    heartbeatFromMember(HostAndPort("h5"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(501, 0), 0),
                        Milliseconds(300));
    heartbeatFromMember(HostAndPort("h6"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(499, 0), 0),
                        Milliseconds(200));
    heartbeatFromMember(HostAndPort("hprimary"),
                        "rs0",
                        MemberState::RS_PRIMARY,
                        OpTime(Timestamp(600, 0), 0),
                        Milliseconds(100));

    // Should choose primary first; it's closest
    getTopoCoord().chooseNewSyncSource(now()++, lastOpTimeWeApplied);
    ASSERT_EQUALS(HostAndPort("hprimary"), getTopoCoord().getSyncSourceAddress());

    // Primary goes far far away
    heartbeatFromMember(HostAndPort("hprimary"),
                        "rs0",
                        MemberState::RS_PRIMARY,
                        OpTime(Timestamp(600, 0), 0),
                        Milliseconds(100000000));

    // Should choose h4.  (if an arbiter has an oplog, it's a valid sync source)
    // h6 is not considered because it is outside the maxSyncLagSeconds window.
    getTopoCoord().chooseNewSyncSource(now()++, lastOpTimeWeApplied);
    ASSERT_EQUALS(HostAndPort("h4"), getTopoCoord().getSyncSourceAddress());

    // h4 goes down; should choose h1
    receiveDownHeartbeat(HostAndPort("h4"), "rs0", OpTime());
    getTopoCoord().chooseNewSyncSource(now()++, lastOpTimeWeApplied);
    ASSERT_EQUALS(HostAndPort("h1"), getTopoCoord().getSyncSourceAddress());

    // Primary and h1 go down; should choose h6
    receiveDownHeartbeat(HostAndPort("h1"), "rs0", OpTime());
    receiveDownHeartbeat(HostAndPort("hprimary"), "rs0", OpTime());
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    getTopoCoord().chooseNewSyncSource(now()++, lastOpTimeWeApplied);
    ASSERT_EQUALS(HostAndPort("h6"), getTopoCoord().getSyncSourceAddress());

    // h6 goes down; should choose h5
    receiveDownHeartbeat(HostAndPort("h6"), "rs0", OpTime());
    getTopoCoord().chooseNewSyncSource(now()++, lastOpTimeWeApplied);
    ASSERT_EQUALS(HostAndPort("h5"), getTopoCoord().getSyncSourceAddress());

    // h5 goes down; should choose h3
    receiveDownHeartbeat(HostAndPort("h5"), "rs0", OpTime());
    getTopoCoord().chooseNewSyncSource(now()++, lastOpTimeWeApplied);
    ASSERT_EQUALS(HostAndPort("h3"), getTopoCoord().getSyncSourceAddress());

    // h3 goes down; no sync source candidates remain
    receiveDownHeartbeat(HostAndPort("h3"), "rs0", OpTime());
    getTopoCoord().chooseNewSyncSource(now()++, lastOpTimeWeApplied);
    ASSERT(getTopoCoord().getSyncSourceAddress().empty());
}


TEST_F(TopoCoordTest, ChooseOnlyPrimaryAsSyncSourceWhenChainingIsDisallowed) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "settings"
                      << BSON("chainingAllowed" << false)
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);

    setSelfMemberState(MemberState::RS_SECONDARY);

    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(1, 0), 0),
                        Milliseconds(100));
    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(1, 0), 0),
                        Milliseconds(100));
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(0, 0), 0),
                        Milliseconds(300));
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(0, 0), 0),
                        Milliseconds(300));

    // No primary situation: should choose no sync source.
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT(getTopoCoord().getSyncSourceAddress().empty());

    // Add primary
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_PRIMARY,
                        OpTime(Timestamp(0, 0), 0),
                        Milliseconds(300));
    ASSERT_EQUALS(2, getCurrentPrimaryIndex());

    // h3 is primary and should be chosen as sync source, despite being further away than h2
    // and the primary (h3) being behind our most recently applied optime
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp(10, 0));
    ASSERT_EQUALS(HostAndPort("h3"), getTopoCoord().getSyncSourceAddress());
}

TEST_F(TopoCoordTest, ChooseOnlyVotersAsSyncSourceWhenNodeIsAVoter) {
    updateConfig(fromjson("{_id:'rs0', version:1, members:["
                          "{_id:10, host:'hself'}, "
                          "{_id:20, host:'h2', votes:0, priority:0}, "
                          "{_id:30, host:'h3'} "
                          "]}"),
                 0);

    setSelfMemberState(MemberState::RS_SECONDARY);

    HostAndPort h2("h2"), h3("h3");
    Timestamp t1(1, 0), t5(5, 0), t10(10, 0);
    OpTime ot1(t1, 0), ot5(t5, 0);
    Milliseconds hbRTT100(100), hbRTT300(300);

    // Two rounds of heartbeat pings from each member.
    heartbeatFromMember(h2, "rs0", MemberState::RS_SECONDARY, ot5, hbRTT100);
    heartbeatFromMember(h2, "rs0", MemberState::RS_SECONDARY, ot5, hbRTT100);
    heartbeatFromMember(h3, "rs0", MemberState::RS_SECONDARY, ot1, hbRTT300);
    heartbeatFromMember(h3, "rs0", MemberState::RS_SECONDARY, ot1, hbRTT300);

    // Should choose h3 as it is a voter
    auto newSource = getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(h3, newSource);

    // Can't choose h2 as it is not a voter
    newSource = getTopoCoord().chooseNewSyncSource(now()++, t10);
    ASSERT_EQUALS(HostAndPort(), newSource);

    // Should choose h3 as it is a voter, and ahead
    heartbeatFromMember(h3, "rs0", MemberState::RS_SECONDARY, ot5, hbRTT300);
    newSource = getTopoCoord().chooseNewSyncSource(now()++, t1);
    ASSERT_EQUALS(h3, newSource);
}

TEST_F(TopoCoordTest, ChooseNoSyncSourceWhenPrimary) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);

    setSelfMemberState(MemberState::RS_SECONDARY);

    // Two rounds of heartbeat pings from each member.
    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(1, 0), 0),
                        Milliseconds(100));
    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(1, 0), 0),
                        Milliseconds(100));
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(0, 0), 0),
                        Milliseconds(300));
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(0, 0), 0),
                        Milliseconds(300));

    // No primary situation: should choose h2 sync source.
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h2"), getTopoCoord().getSyncSourceAddress());

    // Become primary
    makeSelfPrimary(Timestamp(3.0));
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    // Check sync source
    ASSERT_EQUALS(HostAndPort(), getTopoCoord().getSyncSourceAddress());
}

TEST_F(TopoCoordTest, ChooseRequestedSyncSourceOnlyTheFirstTimeAfterTheSyncSourceIsForciblySet) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);

    setSelfMemberState(MemberState::RS_SECONDARY);
    OpTime oldOpTime = OpTime(Timestamp(1, 0), 0);
    OpTime newOpTime = OpTime(Timestamp(2, 0), 0);

    // two rounds of heartbeat pings from each member
    heartbeatFromMember(
        HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, oldOpTime, Milliseconds(300));
    heartbeatFromMember(
        HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, oldOpTime, Milliseconds(300));
    heartbeatFromMember(
        HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, newOpTime, Milliseconds(100));
    heartbeatFromMember(
        HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, newOpTime, Milliseconds(100));

    // force should overrule other defaults
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h3"), getTopoCoord().getSyncSourceAddress());
    getTopoCoord().setForceSyncSourceIndex(1);
    // force should cause shouldChangeSyncSource() to return true
    // even if the currentSource is the force target
    ASSERT_TRUE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("h2"), OpTime(), makeMetadata(oldOpTime), now()));
    ASSERT_TRUE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("h3"), OpTime(), makeMetadata(newOpTime), now()));
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h2"), getTopoCoord().getSyncSourceAddress());

    // force should only work for one call to chooseNewSyncSource
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h3"), getTopoCoord().getSyncSourceAddress());
}

TEST_F(TopoCoordTest, NodeDoesNotChooseBlacklistedSyncSourceUntilBlacklistingExpires) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);

    setSelfMemberState(MemberState::RS_SECONDARY);

    // Two rounds of heartbeat pings from each member.
    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(1, 0), 0),
                        Milliseconds(300));
    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(1, 0), 0),
                        Milliseconds(300));
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(2, 0), 0),
                        Milliseconds(100));
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(2, 0), 0),
                        Milliseconds(100));

    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h3"), getTopoCoord().getSyncSourceAddress());

    Date_t expireTime = Date_t::fromMillisSinceEpoch(1000);
    getTopoCoord().blacklistSyncSource(HostAndPort("h3"), expireTime);
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    // Should choose second best choice now that h3 is blacklisted.
    ASSERT_EQUALS(HostAndPort("h2"), getTopoCoord().getSyncSourceAddress());

    // After time has passed, should go back to original sync source
    getTopoCoord().chooseNewSyncSource(expireTime, Timestamp());
    ASSERT_EQUALS(HostAndPort("h3"), getTopoCoord().getSyncSourceAddress());
}

TEST_F(TopoCoordTest, ChooseNoSyncSourceWhenPrimaryIsBlacklistedAndChainingIsDisallowed) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "settings"
                      << BSON("chainingAllowed" << false)
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);

    setSelfMemberState(MemberState::RS_SECONDARY);

    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_PRIMARY,
                        OpTime(Timestamp(2, 0), 0),
                        Milliseconds(100));
    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_PRIMARY,
                        OpTime(Timestamp(2, 0), 0),
                        Milliseconds(100));
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(2, 0), 0),
                        Milliseconds(100));
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(2, 0), 0),
                        Milliseconds(100));

    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h2"), getTopoCoord().getSyncSourceAddress());

    Date_t expireTime = Date_t::fromMillisSinceEpoch(1000);
    getTopoCoord().blacklistSyncSource(HostAndPort("h2"), expireTime);
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    // Can't choose any sync source now.
    ASSERT(getTopoCoord().getSyncSourceAddress().empty());

    // After time has passed, should go back to the primary
    getTopoCoord().chooseNewSyncSource(expireTime, Timestamp());
    ASSERT_EQUALS(HostAndPort("h2"), getTopoCoord().getSyncSourceAddress());
}

TEST_F(TopoCoordTest, NodeChangesToRecoveringWhenOnlyUnauthorizedNodesAreUp) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);

    setSelfMemberState(MemberState::RS_SECONDARY);

    // Generate enough heartbeats to select a sync source below
    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(1, 0), 0),
                        Milliseconds(300));
    heartbeatFromMember(HostAndPort("h2"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(1, 0), 0),
                        Milliseconds(300));
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(2, 0), 0),
                        Milliseconds(100));
    heartbeatFromMember(HostAndPort("h3"),
                        "rs0",
                        MemberState::RS_SECONDARY,
                        OpTime(Timestamp(2, 0), 0),
                        Milliseconds(100));

    ASSERT_EQUALS(HostAndPort("h3"), getTopoCoord().chooseNewSyncSource(now()++, Timestamp()));
    ASSERT_EQUALS(MemberState::RS_SECONDARY, getTopoCoord().getMemberState().s);
    // Good state setup done

    // Mark nodes down, ensure that we have no source and are secondary
    receiveDownHeartbeat(HostAndPort("h2"), "rs0", OpTime(), ErrorCodes::NetworkTimeout);
    receiveDownHeartbeat(HostAndPort("h3"), "rs0", OpTime(), ErrorCodes::NetworkTimeout);
    ASSERT_TRUE(getTopoCoord().chooseNewSyncSource(now()++, Timestamp()).empty());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, getTopoCoord().getMemberState().s);

    // Mark nodes down + unauth, ensure that we have no source and are secondary
    receiveDownHeartbeat(HostAndPort("h2"), "rs0", OpTime(), ErrorCodes::Unauthorized);
    receiveDownHeartbeat(HostAndPort("h3"), "rs0", OpTime(), ErrorCodes::Unauthorized);
    ASSERT_TRUE(getTopoCoord().chooseNewSyncSource(now()++, Timestamp()).empty());
    ASSERT_EQUALS(MemberState::RS_RECOVERING, getTopoCoord().getMemberState().s);

    // Having an auth error but with another node up should bring us out of RECOVERING
    HeartbeatResponseAction action = receiveUpHeartbeat(HostAndPort("h2"),
                                                        "rs0",
                                                        MemberState::RS_SECONDARY,
                                                        OpTime(),
                                                        OpTime(Timestamp(2, 0), 0),
                                                        OpTime(Timestamp(2, 0), 0));
    ASSERT_EQUALS(MemberState::RS_SECONDARY, getTopoCoord().getMemberState().s);
    // Test that the heartbeat that brings us from RECOVERING to SECONDARY doesn't initiate
    // an election (SERVER-17164)
    ASSERT_NO_ACTION(action.getAction());
}

TEST_F(TopoCoordTest, NodeDoesNotActOnHeartbeatsWhenAbsentFromConfig) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "h1")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 -1);
    ASSERT_NO_ACTION(heartbeatFromMember(HostAndPort("h2"),
                                         "rs0",
                                         MemberState::RS_SECONDARY,
                                         OpTime(Timestamp(1, 0), 0),
                                         Milliseconds(300))
                         .getAction());
}

TEST_F(TopoCoordTest, NodeReturnsNotSecondaryWhenSyncFromIsRunPriorToHavingAConfig) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    // if we do not have an index in the config, we should get ErrorCodes::NotSecondary
    getTopoCoord().prepareSyncFromResponse(HostAndPort("h1"), ourOpTime, &response, &result);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, result);
    ASSERT_EQUALS("Removed and uninitialized nodes do not sync", result.reason());
}

TEST_F(TopoCoordTest, NodeReturnsNotSecondaryWhenSyncFromIsRunAgainstArbiter) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;


    // Test trying to sync from another node when we are an arbiter
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself"
                                               << "arbiterOnly"
                                               << true)
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"))),
                 0);

    getTopoCoord().prepareSyncFromResponse(HostAndPort("h1"), ourOpTime, &response, &result);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, result);
    ASSERT_EQUALS("arbiters don't sync", result.reason());
}

TEST_F(TopoCoordTest, NodeReturnsNotSecondaryWhenSyncFromIsRunAgainstPrimary) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 2 << "host"
                                                  << "h2"
                                                  << "priority"
                                                  << 0
                                                  << "buildIndexes"
                                                  << false)
                                    << BSON("_id" << 3 << "host"
                                                  << "h3")
                                    << BSON("_id" << 4 << "host"
                                                  << "h4")
                                    << BSON("_id" << 5 << "host"
                                                  << "h5")
                                    << BSON("_id" << 6 << "host"
                                                  << "h6"))),
                 0);

    // Try to sync while PRIMARY
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    makeSelfPrimary();
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());
    getTopoCoord()._setCurrentPrimaryForTest(0);
    getTopoCoord().prepareSyncFromResponse(HostAndPort("h3"), ourOpTime, &response, &result);
    ASSERT_EQUALS(ErrorCodes::NotSecondary, result);
    ASSERT_EQUALS("primaries don't sync", result.reason());
    ASSERT_EQUALS("h3:27017", response.obj()["syncFromRequested"].String());
}

TEST_F(TopoCoordTest, NodeReturnsNodeNotFoundWhenSyncFromRequestsANodeNotInConfig) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 2 << "host"
                                                  << "h2"
                                                  << "priority"
                                                  << 0
                                                  << "buildIndexes"
                                                  << false)
                                    << BSON("_id" << 3 << "host"
                                                  << "h3")
                                    << BSON("_id" << 4 << "host"
                                                  << "h4")
                                    << BSON("_id" << 5 << "host"
                                                  << "h5")
                                    << BSON("_id" << 6 << "host"
                                                  << "h6"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    getTopoCoord().prepareSyncFromResponse(
        HostAndPort("fakemember"), ourOpTime, &response, &result);
    ASSERT_EQUALS(ErrorCodes::NodeNotFound, result);
    ASSERT_EQUALS("Could not find member \"fakemember:27017\" in replica set", result.reason());
}

TEST_F(TopoCoordTest, NodeReturnsInvalidOptionsWhenSyncFromRequestsSelf) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 2 << "host"
                                                  << "h2"
                                                  << "priority"
                                                  << 0
                                                  << "buildIndexes"
                                                  << false)
                                    << BSON("_id" << 3 << "host"
                                                  << "h3")
                                    << BSON("_id" << 4 << "host"
                                                  << "h4")
                                    << BSON("_id" << 5 << "host"
                                                  << "h5")
                                    << BSON("_id" << 6 << "host"
                                                  << "h6"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // Try to sync from self
    getTopoCoord().prepareSyncFromResponse(HostAndPort("hself"), ourOpTime, &response, &result);
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, result);
    ASSERT_EQUALS("I cannot sync from myself", result.reason());
}

TEST_F(TopoCoordTest, NodeReturnsInvalidOptionsWhenSyncFromRequestsArbiter) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 2 << "host"
                                                  << "h2"
                                                  << "priority"
                                                  << 0
                                                  << "buildIndexes"
                                                  << false)
                                    << BSON("_id" << 3 << "host"
                                                  << "h3")
                                    << BSON("_id" << 4 << "host"
                                                  << "h4")
                                    << BSON("_id" << 5 << "host"
                                                  << "h5")
                                    << BSON("_id" << 6 << "host"
                                                  << "h6"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);


    // Try to sync from an arbiter
    getTopoCoord().prepareSyncFromResponse(HostAndPort("h1"), ourOpTime, &response, &result);
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, result);
    ASSERT_EQUALS("Cannot sync from \"h1:27017\" because it is an arbiter", result.reason());
}

TEST_F(TopoCoordTest, NodeReturnsInvalidOptionsWhenSyncFromRequestsAnIndexNonbuilder) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 2 << "host"
                                                  << "h2"
                                                  << "priority"
                                                  << 0
                                                  << "buildIndexes"
                                                  << false)
                                    << BSON("_id" << 3 << "host"
                                                  << "h3")
                                    << BSON("_id" << 4 << "host"
                                                  << "h4")
                                    << BSON("_id" << 5 << "host"
                                                  << "h5")
                                    << BSON("_id" << 6 << "host"
                                                  << "h6"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // Try to sync from a node that doesn't build indexes
    getTopoCoord().prepareSyncFromResponse(HostAndPort("h2"), ourOpTime, &response, &result);
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, result);
    ASSERT_EQUALS("Cannot sync from \"h2:27017\" because it does not build indexes",
                  result.reason());
}

TEST_F(TopoCoordTest, NodeReturnsHostUnreachableWhenSyncFromRequestsADownNode) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 2 << "host"
                                                  << "h2"
                                                  << "priority"
                                                  << 0
                                                  << "buildIndexes"
                                                  << false)
                                    << BSON("_id" << 3 << "host"
                                                  << "h3")
                                    << BSON("_id" << 4 << "host"
                                                  << "h4")
                                    << BSON("_id" << 5 << "host"
                                                  << "h5")
                                    << BSON("_id" << 6 << "host"
                                                  << "h6"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // Try to sync from a member that is down
    receiveDownHeartbeat(HostAndPort("h4"), "rs0", OpTime());

    getTopoCoord().prepareSyncFromResponse(HostAndPort("h4"), ourOpTime, &response, &result);
    ASSERT_EQUALS(ErrorCodes::HostUnreachable, result);
    ASSERT_EQUALS("I cannot reach the requested member: h4:27017", result.reason());
}

TEST_F(TopoCoordTest, ChooseRequestedNodeWhenSyncFromRequestsAStaleNode) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 2 << "host"
                                                  << "h2"
                                                  << "priority"
                                                  << 0
                                                  << "buildIndexes"
                                                  << false)
                                    << BSON("_id" << 3 << "host"
                                                  << "h3")
                                    << BSON("_id" << 4 << "host"
                                                  << "h4")
                                    << BSON("_id" << 5 << "host"
                                                  << "h5")
                                    << BSON("_id" << 6 << "host"
                                                  << "h6"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // Sync successfully from a member that is stale
    heartbeatFromMember(
        HostAndPort("h5"), "rs0", MemberState::RS_SECONDARY, staleOpTime, Milliseconds(100));

    getTopoCoord().prepareSyncFromResponse(HostAndPort("h5"), ourOpTime, &response, &result);
    ASSERT_OK(result);
    ASSERT_EQUALS("requested member \"h5:27017\" is more than 10 seconds behind us",
                  response.obj()["warning"].String());
    getTopoCoord().chooseNewSyncSource(now()++, ourOpTime.getTimestamp());
    ASSERT_EQUALS(HostAndPort("h5"), getTopoCoord().getSyncSourceAddress());
}

TEST_F(TopoCoordTest, ChooseRequestedNodeWhenSyncFromRequestsAValidNode) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 2 << "host"
                                                  << "h2"
                                                  << "priority"
                                                  << 0
                                                  << "buildIndexes"
                                                  << false)
                                    << BSON("_id" << 3 << "host"
                                                  << "h3")
                                    << BSON("_id" << 4 << "host"
                                                  << "h4")
                                    << BSON("_id" << 5 << "host"
                                                  << "h5")
                                    << BSON("_id" << 6 << "host"
                                                  << "h6"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // Sync successfully from an up-to-date member
    heartbeatFromMember(
        HostAndPort("h6"), "rs0", MemberState::RS_SECONDARY, ourOpTime, Milliseconds(100));

    getTopoCoord().prepareSyncFromResponse(HostAndPort("h6"), ourOpTime, &response, &result);
    ASSERT_OK(result);
    BSONObj responseObj = response.obj();
    ASSERT_FALSE(responseObj.hasField("warning"));
    getTopoCoord().chooseNewSyncSource(now()++, ourOpTime.getTimestamp());
    ASSERT_EQUALS(HostAndPort("h6"), getTopoCoord().getSyncSourceAddress());
}

TEST_F(TopoCoordTest,
       NodeReturnsRequestedNodeWhenSyncFromRequestsAValidNodeEvenIfTheNodeHasSinceBeenMarkedDown) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 2 << "host"
                                                  << "h2"
                                                  << "priority"
                                                  << 0
                                                  << "buildIndexes"
                                                  << false)
                                    << BSON("_id" << 3 << "host"
                                                  << "h3")
                                    << BSON("_id" << 4 << "host"
                                                  << "h4")
                                    << BSON("_id" << 5 << "host"
                                                  << "h5")
                                    << BSON("_id" << 6 << "host"
                                                  << "h6"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    heartbeatFromMember(
        HostAndPort("h6"), "rs0", MemberState::RS_SECONDARY, ourOpTime, Milliseconds(100));

    // node goes down between forceSync and chooseNewSyncSource
    getTopoCoord().prepareSyncFromResponse(HostAndPort("h6"), ourOpTime, &response, &result);
    BSONObj responseObj = response.obj();
    ASSERT_FALSE(responseObj.hasField("warning"));
    receiveDownHeartbeat(HostAndPort("h6"), "rs0", OpTime());
    HostAndPort syncSource = getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h6"), syncSource);
}

TEST_F(TopoCoordTest, NodeReturnsUnauthorizedWhenSyncFromRequestsANodeWeAreNotAuthorizedFor) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 2 << "host"
                                                  << "h2"
                                                  << "priority"
                                                  << 0
                                                  << "buildIndexes"
                                                  << false)
                                    << BSON("_id" << 3 << "host"
                                                  << "h3")
                                    << BSON("_id" << 4 << "host"
                                                  << "h4")
                                    << BSON("_id" << 5 << "host"
                                                  << "h5")
                                    << BSON("_id" << 6 << "host"
                                                  << "h6"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // Try to sync from a member that is unauth'd
    receiveDownHeartbeat(HostAndPort("h5"), "rs0", OpTime(), ErrorCodes::Unauthorized);

    getTopoCoord().prepareSyncFromResponse(HostAndPort("h5"), ourOpTime, &response, &result);
    ASSERT_NOT_OK(result);
    ASSERT_EQUALS(ErrorCodes::Unauthorized, result.code());
    ASSERT_EQUALS("not authorized to communicate with h5:27017", result.reason());
}

TEST_F(TopoCoordTest, NodeReturnsInvalidOptionsWhenAskedToSyncFromANonVoterAsAVoter) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;

    // Test trying to sync from another node
    updateConfig(fromjson("{_id:'rs0', version:1, members:["
                          "{_id:0, host:'self'},"
                          "{_id:1, host:'h1'},"
                          "{_id:2, host:'h2', votes:0, priority:0}"
                          "]}"),
                 0);

    getTopoCoord().prepareSyncFromResponse(HostAndPort("h2"), ourOpTime, &response, &result);
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, result);
    ASSERT_EQUALS("Cannot sync from \"h2:27017\" because it is not a voter", result.reason());
}

TEST_F(TopoCoordTest,
       NodeShouldReturnPrevSyncTargetWhenItHasASyncTargetAndSyncFromMakesAValidRequest) {
    OpTime staleOpTime(Timestamp(1, 1), 0);
    OpTime ourOpTime(Timestamp(staleOpTime.getSecs() + 11, 1), 0);

    Status result = Status::OK();
    BSONObjBuilder response;
    BSONObjBuilder response2;

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "h1"
                                                  << "arbiterOnly"
                                                  << true)
                                    << BSON("_id" << 2 << "host"
                                                  << "h2"
                                                  << "priority"
                                                  << 0
                                                  << "buildIndexes"
                                                  << false)
                                    << BSON("_id" << 3 << "host"
                                                  << "h3")
                                    << BSON("_id" << 4 << "host"
                                                  << "h4")
                                    << BSON("_id" << 5 << "host"
                                                  << "h5")
                                    << BSON("_id" << 6 << "host"
                                                  << "h6"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // Sync successfully from an up-to-date member.
    heartbeatFromMember(
        HostAndPort("h5"), "rs0", MemberState::RS_SECONDARY, ourOpTime, Milliseconds(100));

    getTopoCoord().prepareSyncFromResponse(HostAndPort("h5"), ourOpTime, &response, &result);
    ASSERT_OK(result);
    BSONObj responseObj = response.obj();
    ASSERT_FALSE(responseObj.hasField("warning"));
    ASSERT_FALSE(responseObj.hasField("prevSyncTarget"));
    getTopoCoord().chooseNewSyncSource(now()++, ourOpTime.getTimestamp());
    ASSERT_EQUALS(HostAndPort("h5"), getTopoCoord().getSyncSourceAddress());

    heartbeatFromMember(
        HostAndPort("h6"), "rs0", MemberState::RS_SECONDARY, ourOpTime, Milliseconds(100));

    // Sync successfully from another up-to-date member.
    getTopoCoord().prepareSyncFromResponse(HostAndPort("h6"), ourOpTime, &response2, &result);
    BSONObj response2Obj = response2.obj();
    ASSERT_FALSE(response2Obj.hasField("warning"));
    ASSERT_EQUALS(HostAndPort("h5").toString(), response2Obj["prevSyncTarget"].String());
}

// TODO(dannenberg) figure out a concise name for this
TEST_F(TopoCoordTest, ReplSetGetStatus) {
    // This test starts by configuring a TopologyCoordinator as a member of a 4 node replica
    // set, with each node in a different state.
    // The first node is DOWN, as if we tried heartbeating them and it failed in some way.
    // The second node is in state SECONDARY, as if we've received a valid heartbeat from them.
    // The third node is in state UNKNOWN, as if we've not yet had any heartbeating activity
    // with them yet.  The fourth node is PRIMARY and corresponds to ourself, which gets its
    // information for replSetGetStatus from a different source than the nodes that aren't
    // ourself.  After this setup, we call prepareStatusResponse and make sure that the fields
    // returned for each member match our expectations.
    Date_t startupTime = Date_t::fromMillisSinceEpoch(100);
    Date_t heartbeatTime = Date_t::fromMillisSinceEpoch(5000);
    Seconds uptimeSecs(10);
    Date_t curTime = heartbeatTime + uptimeSecs;
    Timestamp electionTime(1, 2);
    OpTime oplogProgress(Timestamp(3, 4), 2);
    OpTime oplogDurable(Timestamp(3, 4), 1);
    OpTime lastCommittedOpTime(Timestamp(2, 3), 6);
    OpTime readConcernMajorityOpTime(Timestamp(4, 5), 7);
    std::string setName = "mySet";

    ReplSetHeartbeatResponse hb;
    hb.setConfigVersion(1);
    hb.setState(MemberState::RS_SECONDARY);
    hb.setElectionTime(electionTime);
    hb.setHbMsg("READY");
    hb.setDurableOpTime(oplogDurable);
    hb.setAppliedOpTime(oplogProgress);
    StatusWith<ReplSetHeartbeatResponse> hbResponseGood = StatusWith<ReplSetHeartbeatResponse>(hb);

    updateConfig(BSON("_id" << setName << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test0:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test1:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test2:1234")
                                          << BSON("_id" << 3 << "host"
                                                        << "test3:1234"))),
                 3,
                 startupTime + Milliseconds(1));

    // Now that the replica set is setup, put the members into the states we want them in.
    HostAndPort member = HostAndPort("test0:1234");
    getTopoCoord().prepareHeartbeatRequestV1(startupTime + Milliseconds(1), setName, member);
    getTopoCoord().processHeartbeatResponse(
        startupTime + Milliseconds(2), Milliseconds(1), member, hbResponseGood, OpTime());
    getTopoCoord().prepareHeartbeatRequestV1(startupTime + Milliseconds(3), setName, member);
    Date_t timeoutTime =
        startupTime + Milliseconds(3) + ReplicaSetConfig::kDefaultHeartbeatTimeoutPeriod;

    StatusWith<ReplSetHeartbeatResponse> hbResponseDown =
        StatusWith<ReplSetHeartbeatResponse>(Status(ErrorCodes::HostUnreachable, ""));

    getTopoCoord().processHeartbeatResponse(
        timeoutTime, Milliseconds(5000), member, hbResponseDown, OpTime());

    member = HostAndPort("test1:1234");
    getTopoCoord().prepareHeartbeatRequestV1(startupTime + Milliseconds(2), setName, member);
    getTopoCoord().processHeartbeatResponse(
        heartbeatTime, Milliseconds(4000), member, hbResponseGood, OpTime());
    makeSelfPrimary();

    // Now node 0 is down, node 1 is up, and for node 2 we have no heartbeat data yet.
    BSONObjBuilder statusBuilder;
    Status resultStatus(ErrorCodes::InternalError, "prepareStatusResponse didn't set result");
    getTopoCoord().prepareStatusResponse(
        TopologyCoordinator::ReplSetStatusArgs{
            curTime,
            static_cast<unsigned>(durationCount<Seconds>(uptimeSecs)),
            oplogProgress,
            oplogDurable,
            lastCommittedOpTime,
            readConcernMajorityOpTime},
        &statusBuilder,
        &resultStatus);
    ASSERT_OK(resultStatus);
    BSONObj rsStatus = statusBuilder.obj();

    // Test results for all non-self members
    ASSERT_EQUALS(setName, rsStatus["set"].String());
    ASSERT_EQUALS(curTime.asInt64(), rsStatus["date"].Date().asInt64());
    ASSERT_EQUALS(lastCommittedOpTime.toBSON(), rsStatus["optimes"]["lastCommittedOpTime"].Obj());
    {
        const auto optimes = rsStatus["optimes"].Obj();
        ASSERT_EQUALS(readConcernMajorityOpTime.toBSON(),
                      optimes["readConcernMajorityOpTime"].Obj());
        ASSERT_EQUALS(oplogProgress.toBSON(), optimes["appliedOpTime"].Obj());
        ASSERT_EQUALS((oplogDurable).toBSON(), optimes["durableOpTime"].Obj());
    }
    std::vector<BSONElement> memberArray = rsStatus["members"].Array();
    ASSERT_EQUALS(4U, memberArray.size());
    BSONObj member0Status = memberArray[0].Obj();
    BSONObj member1Status = memberArray[1].Obj();
    BSONObj member2Status = memberArray[2].Obj();

    // Test member 0, the node that's DOWN
    ASSERT_EQUALS(0, member0Status["_id"].numberInt());
    ASSERT_EQUALS("test0:1234", member0Status["name"].str());
    ASSERT_EQUALS(0, member0Status["health"].numberDouble());
    ASSERT_EQUALS(MemberState::RS_DOWN, member0Status["state"].numberInt());
    ASSERT_EQUALS("(not reachable/healthy)", member0Status["stateStr"].str());
    ASSERT_EQUALS(0, member0Status["uptime"].numberInt());
    ASSERT_EQUALS(Timestamp(), Timestamp(member0Status["optime"]["ts"].timestampValue()));
    ASSERT_EQUALS(-1LL, member0Status["optime"]["t"].numberLong());
    ASSERT_TRUE(member0Status.hasField("optimeDate"));
    ASSERT_EQUALS(Date_t::fromMillisSinceEpoch(Timestamp().getSecs() * 1000ULL),
                  member0Status["optimeDate"].Date());
    ASSERT_EQUALS(timeoutTime, member0Status["lastHeartbeat"].date());
    ASSERT_EQUALS(Date_t(), member0Status["lastHeartbeatRecv"].date());

    // Test member 1, the node that's SECONDARY
    ASSERT_EQUALS(1, member1Status["_id"].Int());
    ASSERT_EQUALS("test1:1234", member1Status["name"].String());
    ASSERT_EQUALS(1, member1Status["health"].Double());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, member1Status["state"].numberInt());
    ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY).toString(),
                  member1Status["stateStr"].String());
    ASSERT_EQUALS(durationCount<Seconds>(uptimeSecs), member1Status["uptime"].numberInt());
    ASSERT_EQUALS(oplogProgress.toBSON(), member1Status["optime"].Obj());
    ASSERT_TRUE(member1Status.hasField("optimeDate"));
    ASSERT_EQUALS(Date_t::fromMillisSinceEpoch(oplogProgress.getSecs() * 1000ULL),
                  member1Status["optimeDate"].Date());
    ASSERT_EQUALS(heartbeatTime, member1Status["lastHeartbeat"].date());
    ASSERT_EQUALS(Date_t(), member1Status["lastHeartbeatRecv"].date());
    ASSERT_EQUALS("READY", member1Status["lastHeartbeatMessage"].str());

    // Test member 2, the node that's UNKNOWN
    ASSERT_EQUALS(2, member2Status["_id"].numberInt());
    ASSERT_EQUALS("test2:1234", member2Status["name"].str());
    ASSERT_EQUALS(-1, member2Status["health"].numberDouble());
    ASSERT_EQUALS(MemberState::RS_UNKNOWN, member2Status["state"].numberInt());
    ASSERT_EQUALS(MemberState(MemberState::RS_UNKNOWN).toString(), member2Status["stateStr"].str());
    ASSERT_TRUE(member2Status.hasField("uptime"));
    ASSERT_TRUE(member2Status.hasField("optime"));
    ASSERT_TRUE(member2Status.hasField("optimeDate"));
    ASSERT_FALSE(member2Status.hasField("lastHearbeat"));
    ASSERT_FALSE(member2Status.hasField("lastHearbeatRecv"));

    // Now test results for ourself, the PRIMARY
    ASSERT_EQUALS(MemberState::RS_PRIMARY, rsStatus["myState"].numberInt());
    BSONObj selfStatus = memberArray[3].Obj();
    ASSERT_TRUE(selfStatus["self"].boolean());
    ASSERT_EQUALS(3, selfStatus["_id"].numberInt());
    ASSERT_EQUALS("test3:1234", selfStatus["name"].str());
    ASSERT_EQUALS(1, selfStatus["health"].numberDouble());
    ASSERT_EQUALS(MemberState::RS_PRIMARY, selfStatus["state"].numberInt());
    ASSERT_EQUALS(MemberState(MemberState::RS_PRIMARY).toString(), selfStatus["stateStr"].str());
    ASSERT_EQUALS(durationCount<Seconds>(uptimeSecs), selfStatus["uptime"].numberInt());
    ASSERT_EQUALS(oplogProgress.toBSON(), selfStatus["optime"].Obj());
    ASSERT_TRUE(selfStatus.hasField("optimeDate"));
    ASSERT_EQUALS(Date_t::fromMillisSinceEpoch(oplogProgress.getSecs() * 1000ULL),
                  selfStatus["optimeDate"].Date());

    ASSERT_EQUALS(2000, rsStatus["heartbeatIntervalMillis"].numberInt());

    // TODO(spencer): Test electionTime and pingMs are set properly
}

TEST_F(TopoCoordTest, NodeReturnsInvalidReplicaSetConfigInResponseToGetStatusWhenAbsentFromConfig) {
    // This test starts by configuring a TopologyCoordinator to NOT be a member of a 3 node
    // replica set. Then running prepareStatusResponse should fail.
    Date_t startupTime = Date_t::fromMillisSinceEpoch(100);
    Date_t heartbeatTime = Date_t::fromMillisSinceEpoch(5000);
    Seconds uptimeSecs(10);
    Date_t curTime = heartbeatTime + uptimeSecs;
    OpTime oplogProgress(Timestamp(3, 4), 0);
    std::string setName = "mySet";

    updateConfig(BSON("_id" << setName << "version" << 1 << "members"
                            << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                     << "test0:1234")
                                          << BSON("_id" << 1 << "host"
                                                        << "test1:1234")
                                          << BSON("_id" << 2 << "host"
                                                        << "test2:1234"))),
                 -1,  // This one is not part of the replica set.
                 startupTime + Milliseconds(1));

    BSONObjBuilder statusBuilder;
    Status resultStatus(ErrorCodes::InternalError, "prepareStatusResponse didn't set result");
    getTopoCoord().prepareStatusResponse(
        TopologyCoordinator::ReplSetStatusArgs{
            curTime,
            static_cast<unsigned>(durationCount<Seconds>(uptimeSecs)),
            oplogProgress,
            oplogProgress,
            OpTime(),
            OpTime()},
        &statusBuilder,
        &resultStatus);
    ASSERT_NOT_OK(resultStatus);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, resultStatus);
}

TEST_F(TopoCoordTest, HeartbeatFrequencyShouldBeHalfElectionTimeoutWhenArbiter) {
    // This tests that arbiters issue heartbeats at electionTimeout/2 frequencies
    TopoCoordTest::setUp();
    updateConfig(fromjson("{_id:'mySet', version:1, protocolVersion:1, members:["
                          "{_id:1, host:'node1:12345', arbiterOnly:true}, "
                          "{_id:2, host:'node2:12345'}], "
                          "settings:{heartbeatIntervalMillis:10, electionTimeoutMillis:5000}}"),
                 0);
    HostAndPort target("host2", 27017);
    Date_t requestDate = now();
    std::pair<ReplSetHeartbeatArgs, Milliseconds> uppingRequest =
        getTopoCoord().prepareHeartbeatRequest(requestDate, "myset", target);
    auto action =
        getTopoCoord().processHeartbeatResponse(requestDate,
                                                Milliseconds(0),
                                                target,
                                                makeStatusWith<ReplSetHeartbeatResponse>(),
                                                OpTime(Timestamp(0, 0), 0));
    Date_t expected(now() + Milliseconds(2500));
    ASSERT_EQUALS(expected, action.getNextHeartbeatStartDate());
}

class PrepareHeartbeatResponseV1Test : public TopoCoordTest {
public:
    virtual void setUp() {
        TopoCoordTest::setUp();
        updateConfig(BSON("_id"
                          << "rs0"
                          << "version"
                          << 1
                          << "members"
                          << BSON_ARRAY(BSON("_id" << 10 << "host"
                                                   << "hself")
                                        << BSON("_id" << 20 << "host"
                                                      << "h2")
                                        << BSON("_id" << 30 << "host"
                                                      << "h3"))
                          << "settings"
                          << BSON("protocolVersion" << 1)),
                     0);
        setSelfMemberState(MemberState::RS_SECONDARY);
    }

    void prepareHeartbeatResponseV1(const ReplSetHeartbeatArgsV1& args,
                                    OpTime lastOpApplied,
                                    ReplSetHeartbeatResponse* response,
                                    Status* result) {
        *result = getTopoCoord().prepareHeartbeatResponseV1(
            now()++, args, "rs0", lastOpApplied, lastOpApplied, response);
    }
};

TEST_F(PrepareHeartbeatResponseV1Test,
       NodeReturnsInconsistentReplicaSetNamesWhenAHeartbeatRequestHasADifferentReplicaSetName) {
    // set up args with incorrect replset name
    ReplSetHeartbeatArgsV1 args;
    args.setSetName("rs1");
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    startCapturingLogMessages();
    prepareHeartbeatResponseV1(args, OpTime(), &response, &result);
    stopCapturingLogMessages();
    ASSERT_EQUALS(ErrorCodes::InconsistentReplicaSetNames, result);
    ASSERT(result.reason().find("repl set names do not match")) << "Actual string was \""
                                                                << result.reason() << '"';
    ASSERT_EQUALS(1,
                  countLogLinesContaining("replSet set names do not match, ours: rs0; remote "
                                          "node's: rs1"));
    // only protocolVersion should be set in this failure case
    ASSERT_EQUALS("", response.getReplicaSetName());
}

TEST_F(PrepareHeartbeatResponseV1Test,
       NodeReturnsInvalidReplicaSetConfigWhenAHeartbeatRequestComesInWhileAbsentFromAPV1Config) {
    // reconfig self out of set
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 3
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 20 << "host"
                                               << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))
                      << "settings"
                      << BSON("protocolVersion" << 1)),
                 -1);
    ReplSetHeartbeatArgsV1 args;
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");
    prepareHeartbeatResponseV1(args, OpTime(), &response, &result);
    ASSERT_EQUALS(ErrorCodes::InvalidReplicaSetConfig, result);
    ASSERT(result.reason().find("replica set configuration is invalid or does not include us"))
        << "Actual string was \"" << result.reason() << '"';
    // only protocolVersion should be set in this failure case
    ASSERT_EQUALS("", response.getReplicaSetName());
}

TEST_F(PrepareHeartbeatResponseV1Test, NodeReturnsBadValueWhenAHeartbeatRequestIsFromSelf) {
    // set up args with our id as the senderId
    ReplSetHeartbeatArgsV1 args;
    args.setSetName("rs0");
    args.setSenderId(10);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");
    prepareHeartbeatResponseV1(args, OpTime(), &response, &result);
    ASSERT_EQUALS(ErrorCodes::BadValue, result);
    ASSERT(result.reason().find("from member with the same member ID as our self"))
        << "Actual string was \"" << result.reason() << '"';
    // only protocolVersion should be set in this failure case
    ASSERT_EQUALS("", response.getReplicaSetName());
}

TEST_F(TopoCoordTest, SetConfigVersionToNegativeTwoInHeartbeatResponseWhenNoConfigHasBeenReceived) {
    // set up args and acknowledge sender
    ReplSetHeartbeatArgsV1 args;
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    // prepare response and check the results
    Status result = getTopoCoord().prepareHeartbeatResponseV1(
        now()++, args, "rs0", OpTime(), OpTime(), &response);
    ASSERT_OK(result);
    // this change to true because we can now see a majority, unlike in the previous cases
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(MemberState::RS_STARTUP, response.getState().s);
    ASSERT_EQUALS(OpTime(), response.getDurableOpTime());
    // default term of topology coordinator is -1
    ASSERT_EQUALS(-1, response.getTerm());
    ASSERT_EQUALS(-2, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseV1Test,
       PopulateFullHeartbeatResponseEvenWhenHeartbeatRequestLacksASenderID) {
    // set up args without a senderID
    ReplSetHeartbeatArgsV1 args;
    args.setSetName("rs0");
    args.setConfigVersion(1);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponseV1(args, OpTime(), &response, &result);
    ASSERT_OK(result);
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_EQUALS(OpTime(), response.getDurableOpTime());
    ASSERT_EQUALS(0, response.getTerm());
    ASSERT_EQUALS(1, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseV1Test,
       PopulateFullHeartbeatResponseEvenWhenHeartbeatRequestHasAnInvalidSenderID) {
    // set up args with a senderID which is not present in our config
    ReplSetHeartbeatArgsV1 args;
    args.setSetName("rs0");
    args.setConfigVersion(1);
    args.setSenderId(2);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponseV1(args, OpTime(), &response, &result);
    ASSERT_OK(result);
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_EQUALS(OpTime(), response.getDurableOpTime());
    ASSERT_EQUALS(0, response.getTerm());
    ASSERT_EQUALS(1, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseV1Test,
       PopulateHeartbeatResponseWithFullConfigWhenHeartbeatRequestHasAnOldConfigVersion) {
    // set up args with a config version lower than ours
    ReplSetHeartbeatArgsV1 args;
    args.setConfigVersion(0);
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponseV1(args, OpTime(), &response, &result);
    ASSERT_OK(result);
    ASSERT_TRUE(response.hasConfig());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_EQUALS(OpTime(), response.getDurableOpTime());
    ASSERT_EQUALS(0, response.getTerm());
    ASSERT_EQUALS(1, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseV1Test,
       PopulateFullHeartbeatResponseWhenHeartbeatRequestHasANewerConfigVersion) {
    // set up args with a config version higher than ours
    ReplSetHeartbeatArgsV1 args;
    args.setConfigVersion(10);
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponseV1(args, OpTime(), &response, &result);
    ASSERT_OK(result);
    ASSERT_FALSE(response.hasConfig());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_EQUALS(OpTime(), response.getDurableOpTime());
    ASSERT_EQUALS(0, response.getTerm());
    ASSERT_EQUALS(1, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseV1Test, SetStatePrimaryInHeartbeatResponseWhenPrimary) {
    makeSelfPrimary(Timestamp(10, 0));

    ReplSetHeartbeatArgsV1 args;
    args.setConfigVersion(1);
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponseV1(args, OpTime(Timestamp(11, 0), 0), &response, &result);
    ASSERT_OK(result);
    ASSERT_FALSE(response.hasConfig());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(MemberState::RS_PRIMARY, response.getState().s);
    ASSERT_TRUE(response.hasElectionTime());
    ASSERT_EQUALS(getTopoCoord().getElectionTime(), response.getElectionTime());
    ASSERT_EQUALS(OpTime(Timestamp(11, 0), 0), response.getDurableOpTime());
    ASSERT_EQUALS(0, response.getTerm());
    ASSERT_EQUALS(1, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseV1Test,
       IncludeSyncingToFieldInHeartbeatResponseWhenThereIsASyncSource) {
    // get a sync source
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, OpTime());
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, OpTime());
    heartbeatFromMember(
        HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(1, 0), 0));
    heartbeatFromMember(
        HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(1, 0), 0));
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());

    // set up args
    ReplSetHeartbeatArgsV1 args;
    args.setConfigVersion(1);
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponseV1(args, OpTime(Timestamp(100, 0), 0), &response, &result);
    ASSERT_OK(result);
    ASSERT_FALSE(response.hasConfig());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_FALSE(response.hasElectionTime());
    ASSERT_EQUALS(OpTime(Timestamp(100, 0), 0), response.getDurableOpTime());
    ASSERT_EQUALS(0, response.getTerm());
    ASSERT_EQUALS(1, response.getConfigVersion());
    ASSERT_EQUALS(HostAndPort("h2"), response.getSyncingTo());
}

TEST_F(TopoCoordTest, BecomeCandidateWhenBecomingSecondaryInSingleNodeSet) {
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP, getTopoCoord().getMemberState().s);
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                               << "hself"))),
                 0);
    ASSERT_EQUALS(MemberState::RS_STARTUP2, getTopoCoord().getMemberState().s);

    // if we are the only node, we should become a candidate when we transition to SECONDARY
    ASSERT_FALSE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    getTopoCoord().setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, getTopoCoord().getMemberState().s);
}

TEST_F(TopoCoordTest, BecomeCandidateWhenReconfigToBeElectableInSingleNodeSet) {
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP, getTopoCoord().getMemberState().s);
    ReplicaSetConfig cfg;
    cfg.initialize(BSON("_id"
                        << "rs0"
                        << "version"
                        << 1
                        << "members"
                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                 << "hself"
                                                 << "priority"
                                                 << 0))));
    getTopoCoord().updateConfig(cfg, 0, now()++, OpTime());
    ASSERT_EQUALS(MemberState::RS_STARTUP2, getTopoCoord().getMemberState().s);

    ASSERT_FALSE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    getTopoCoord().setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_FALSE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, getTopoCoord().getMemberState().s);

    // we should become a candidate when we reconfig to become electable

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                               << "hself"))),
                 0);
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
}

TEST_F(TopoCoordTest, NodeDoesNotBecomeCandidateWhenBecomingSecondaryInSingleNodeSetIfUnelectable) {
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP, getTopoCoord().getMemberState().s);
    ReplicaSetConfig cfg;
    cfg.initialize(BSON("_id"
                        << "rs0"
                        << "version"
                        << 1
                        << "members"
                        << BSON_ARRAY(BSON("_id" << 1 << "host"
                                                 << "hself"
                                                 << "priority"
                                                 << 0))));

    getTopoCoord().updateConfig(cfg, 0, now()++, OpTime());
    ASSERT_EQUALS(MemberState::RS_STARTUP2, getTopoCoord().getMemberState().s);

    // despite being the only node, we are unelectable, so we should not become a candidate
    ASSERT_FALSE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    getTopoCoord().setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_FALSE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, getTopoCoord().getMemberState().s);
}

TEST_F(TopoCoordTest, NodeTransitionsFromRemovedToStartup2WhenAddedToConfig) {
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP, getTopoCoord().getMemberState().s);
    // config to be absent from the set
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                               << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))),
                 -1);
    // should become removed since we are not in the set
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_REMOVED, getTopoCoord().getMemberState().s);

    // reconfig to add to set
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 2
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))),
                 0);
    // having been added to the config, we should no longer be REMOVED and should enter STARTUP2
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP2, getTopoCoord().getMemberState().s);
}

TEST_F(TopoCoordTest, NodeTransitionsToRemovedWhenRemovedFromConfig) {
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP, getTopoCoord().getMemberState().s);
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))),
                 0);
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP2, getTopoCoord().getMemberState().s);

    // reconfig to remove self
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 2
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                               << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))),
                 -1);
    // should become removed since we are no longer in the set
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_REMOVED, getTopoCoord().getMemberState().s);
}

TEST_F(TopoCoordTest, NodeTransitionsToRemovedWhenRemovedFromConfigEvenWhenPrimary) {
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP, getTopoCoord().getMemberState().s);
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017"))),
                 0);
    ASSERT_FALSE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP2, getTopoCoord().getMemberState().s);
    getTopoCoord().setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    // win election and primary
    getTopoCoord().processWinElection(OID::gen(), Timestamp());
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_PRIMARY, getTopoCoord().getMemberState().s);

    // reconfig to remove self
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 2
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                               << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))),
                 -1);
    // should become removed since we are no longer in the set even though we were primary
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_REMOVED, getTopoCoord().getMemberState().s);
}

TEST_F(TopoCoordTest, NodeTransitionsToSecondaryWhenReconfiggingToBeUnelectable) {
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP, getTopoCoord().getMemberState().s);
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017"))),
                 0);
    ASSERT_FALSE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP2, getTopoCoord().getMemberState().s);
    getTopoCoord().setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    // win election and primary
    getTopoCoord().processWinElection(OID::gen(), Timestamp());
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_PRIMARY, getTopoCoord().getMemberState().s);

    // now lose primary due to loss of electability
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 2
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017"
                                               << "priority"
                                               << 0)
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))),
                 0);
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, getTopoCoord().getMemberState().s);
}

TEST_F(TopoCoordTest, NodeMaintainsPrimaryStateAcrossReconfigIfNodeRemainsElectable) {
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP, getTopoCoord().getMemberState().s);
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017"))),
                 0);

    ASSERT_FALSE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP2, getTopoCoord().getMemberState().s);
    getTopoCoord().setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    // win election and primary
    getTopoCoord().processWinElection(OID::gen(), Timestamp());
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_PRIMARY, getTopoCoord().getMemberState().s);

    // Now reconfig in ways that leave us electable and ensure we are still the primary.
    // Add hosts
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 2
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))),
                 0,
                 Date_t::fromMillisSinceEpoch(-1),
                 OpTime(Timestamp(10, 0), 0));
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_PRIMARY, getTopoCoord().getMemberState().s);

    // Change priorities and tags
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 2
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017"
                                                  << "priority"
                                                  << 5
                                                  << "tags"
                                                  << BSON("dc"
                                                          << "NA"
                                                          << "rack"
                                                          << "rack1")))),
                 0,
                 Date_t::fromMillisSinceEpoch(-1),
                 OpTime(Timestamp(10, 0), 0));
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_PRIMARY, getTopoCoord().getMemberState().s);
}

TEST_F(TopoCoordTest, NodeMaintainsSecondaryStateAcrossReconfig) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 1 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host2:27017"))),
                 0);
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_STARTUP2, getTopoCoord().getMemberState().s);
    setSelfMemberState(MemberState::RS_SECONDARY);
    ASSERT_EQUALS(MemberState::RS_SECONDARY, getTopoCoord().getMemberState().s);

    // reconfig and stay secondary
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 2
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))),
                 0);
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, getTopoCoord().getMemberState().s);
}

TEST_F(TopoCoordTest, ShouldNotStandForElectionWhileAwareOfPrimary) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    heartbeatFromMember(
        HostAndPort("h2"), "rs0", MemberState::RS_PRIMARY, OpTime(Timestamp(1, 0), 0));
    ASSERT_NOT_OK(getTopoCoord().checkShouldStandForElection(now()++, OpTime()));
}

TEST_F(TopoCoordTest, ShouldStandForElectionDespiteNotCloseEnoughToLastOptime) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    heartbeatFromMember(
        HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(10000, 0), 0));
    ASSERT_OK(getTopoCoord().checkShouldStandForElection(now()++, OpTime(Timestamp(100, 0), 0)));
}

TEST_F(TopoCoordTest, VoteForMyselfFailsWhileNotCandidate) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);
    ASSERT_FALSE(getTopoCoord().voteForMyself(now()++));
}

TEST_F(TopoCoordTest, NodeReturnsArbiterWhenGetMemberStateRunsAgainstArbiter) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "arbiterOnly"
                                               << true)
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    ASSERT_EQUALS(MemberState::RS_ARBITER, getTopoCoord().getMemberState().s);
}

TEST_F(TopoCoordTest, ShouldNotStandForElectionWhileRemovedFromTheConfig) {
    const auto status =
        getTopoCoord().checkShouldStandForElection(now()++, OpTime(Timestamp(10, 0), 0));
    ASSERT_NOT_OK(status);
    ASSERT_STRING_CONTAINS(status.reason(), "not a member of a valid replica set config");
}

TEST_F(TopoCoordTest, NodeDoesNotGrantVotesToTwoDifferentNodesInTheSameTerm) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "rs0"
                                               << "term"
                                               << 1LL
                                               << "candidateIndex"
                                               << 0LL
                                               << "configVersion"
                                               << 1LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;
    OpTime lastAppliedOpTime;

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime);
    ASSERT_EQUALS("", response.getReason());
    ASSERT_TRUE(response.getVoteGranted());

    ReplSetRequestVotesArgs args2;
    args2.initialize(
        BSON("replSetRequestVotes" << 1 << "setName"
                                   << "rs0"
                                   << "term"
                                   << 1LL
                                   << "candidateIndex"
                                   << 1LL
                                   << "configVersion"
                                   << 1LL
                                   << "lastCommittedOp"
                                   << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response2;

    // different candidate same term, should be a problem
    getTopoCoord().processReplSetRequestVotes(args2, &response2, lastAppliedOpTime);
    ASSERT_EQUALS("already voted for another candidate this term", response2.getReason());
    ASSERT_FALSE(response2.getVoteGranted());
}

TEST_F(TopoCoordTest, DryRunVoteRequestShouldNotPreventSubsequentDryRunsForThatTerm) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // dry run
    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "rs0"
                                               << "dryRun"
                                               << true
                                               << "term"
                                               << 1LL
                                               << "candidateIndex"
                                               << 0LL
                                               << "configVersion"
                                               << 1LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;
    OpTime lastAppliedOpTime;

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime);
    ASSERT_EQUALS("", response.getReason());
    ASSERT_TRUE(response.getVoteGranted());

    // second dry run fine
    ReplSetRequestVotesArgs args2;
    args2.initialize(
        BSON("replSetRequestVotes" << 1 << "setName"
                                   << "rs0"
                                   << "dryRun"
                                   << true
                                   << "term"
                                   << 1LL
                                   << "candidateIndex"
                                   << 0LL
                                   << "configVersion"
                                   << 1LL
                                   << "lastCommittedOp"
                                   << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response2;

    getTopoCoord().processReplSetRequestVotes(args2, &response2, lastAppliedOpTime);
    ASSERT_EQUALS("", response2.getReason());
    ASSERT_TRUE(response2.getVoteGranted());

    // real request fine
    ReplSetRequestVotesArgs args3;
    args3.initialize(
        BSON("replSetRequestVotes" << 1 << "setName"
                                   << "rs0"
                                   << "dryRun"
                                   << false
                                   << "term"
                                   << 1LL
                                   << "candidateIndex"
                                   << 0LL
                                   << "configVersion"
                                   << 1LL
                                   << "lastCommittedOp"
                                   << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response3;

    getTopoCoord().processReplSetRequestVotes(args3, &response3, lastAppliedOpTime);
    ASSERT_EQUALS("", response3.getReason());
    ASSERT_TRUE(response3.getVoteGranted());

    // dry post real, fails
    ReplSetRequestVotesArgs args4;
    args4.initialize(
        BSON("replSetRequestVotes" << 1 << "setName"
                                   << "rs0"
                                   << "dryRun"
                                   << false
                                   << "term"
                                   << 1LL
                                   << "candidateIndex"
                                   << 0LL
                                   << "configVersion"
                                   << 1LL
                                   << "lastCommittedOp"
                                   << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response4;

    getTopoCoord().processReplSetRequestVotes(args4, &response4, lastAppliedOpTime);
    ASSERT_EQUALS("already voted for another candidate this term", response4.getReason());
    ASSERT_FALSE(response4.getVoteGranted());
}

TEST_F(TopoCoordTest, VoteRequestShouldNotPreventDryRunsForThatTerm) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // real request fine
    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "rs0"
                                               << "dryRun"
                                               << false
                                               << "term"
                                               << 1LL
                                               << "candidateIndex"
                                               << 0LL
                                               << "configVersion"
                                               << 1LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;
    OpTime lastAppliedOpTime;

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime);
    ASSERT_EQUALS("", response.getReason());
    ASSERT_TRUE(response.getVoteGranted());

    // dry post real, fails
    ReplSetRequestVotesArgs args2;
    args2.initialize(
        BSON("replSetRequestVotes" << 1 << "setName"
                                   << "rs0"
                                   << "dryRun"
                                   << false
                                   << "term"
                                   << 1LL
                                   << "candidateIndex"
                                   << 0LL
                                   << "configVersion"
                                   << 1LL
                                   << "lastCommittedOp"
                                   << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response2;

    getTopoCoord().processReplSetRequestVotes(args2, &response2, lastAppliedOpTime);
    ASSERT_EQUALS("already voted for another candidate this term", response2.getReason());
    ASSERT_FALSE(response2.getVoteGranted());
}

TEST_F(TopoCoordTest, NodeDoesNotGrantVoteWhenReplSetNameDoesNotMatch) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // mismatched setName
    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "wrongName"
                                               << "term"
                                               << 1LL
                                               << "candidateIndex"
                                               << 0LL
                                               << "configVersion"
                                               << 1LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;
    OpTime lastAppliedOpTime;

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime);
    ASSERT_EQUALS("candidate's set name differs from mine", response.getReason());
    ASSERT_FALSE(response.getVoteGranted());
}

TEST_F(TopoCoordTest, NodeDoesNotGrantVoteWhenConfigVersionDoesNotMatch) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    // mismatched configVersion
    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "rs0"
                                               << "term"
                                               << 1LL
                                               << "candidateIndex"
                                               << 1LL
                                               << "configVersion"
                                               << 0LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;
    OpTime lastAppliedOpTime;

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime);
    ASSERT_EQUALS("candidate's config version differs from mine", response.getReason());
    ASSERT_FALSE(response.getVoteGranted());
}

TEST_F(TopoCoordTest, NodeDoesNotGrantVoteWhenTermIsStale) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    ASSERT(TopologyCoordinator::UpdateTermResult::kUpdatedTerm ==
           getTopoCoord().updateTerm(2, now()));
    ASSERT_EQUALS(2, getTopoCoord().getTerm());

    // stale term
    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "rs0"
                                               << "term"
                                               << 1LL
                                               << "candidateIndex"
                                               << 1LL
                                               << "configVersion"
                                               << 1LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;
    OpTime lastAppliedOpTime;

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime);
    ASSERT_EQUALS("candidate's term is lower than mine", response.getReason());
    ASSERT_EQUALS(2, response.getTerm());
    ASSERT_FALSE(response.getVoteGranted());
}

TEST_F(TopoCoordTest, NodeDoesNotGrantVoteWhenOpTimeIsStale) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);


    // stale OpTime
    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "rs0"
                                               << "term"
                                               << 3LL
                                               << "candidateIndex"
                                               << 1LL
                                               << "configVersion"
                                               << 1LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;
    OpTime lastAppliedOpTime2 = {Timestamp(20, 0), 0};

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime2);
    ASSERT_EQUALS("candidate's data is staler than mine", response.getReason());
    ASSERT_FALSE(response.getVoteGranted());
}

TEST_F(TopoCoordTest, NodeDoesNotGrantDryRunVoteWhenReplSetNameDoesNotMatch) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);
    // set term to 1
    ASSERT(TopologyCoordinator::UpdateTermResult::kUpdatedTerm ==
           getTopoCoord().updateTerm(1, now()));
    // and make sure we voted in term 1
    ReplSetRequestVotesArgs argsForRealVote;
    argsForRealVote.initialize(
        BSON("replSetRequestVotes" << 1 << "setName"
                                   << "rs0"
                                   << "term"
                                   << 1LL
                                   << "candidateIndex"
                                   << 0LL
                                   << "configVersion"
                                   << 1LL
                                   << "lastCommittedOp"
                                   << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse responseForRealVote;
    OpTime lastAppliedOpTime;

    getTopoCoord().processReplSetRequestVotes(
        argsForRealVote, &responseForRealVote, lastAppliedOpTime);
    ASSERT_EQUALS("", responseForRealVote.getReason());
    ASSERT_TRUE(responseForRealVote.getVoteGranted());


    // mismatched setName
    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "wrongName"
                                               << "dryRun"
                                               << true
                                               << "term"
                                               << 2LL
                                               << "candidateIndex"
                                               << 0LL
                                               << "configVersion"
                                               << 1LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime);
    ASSERT_EQUALS("candidate's set name differs from mine", response.getReason());
    ASSERT_EQUALS(1, response.getTerm());
    ASSERT_FALSE(response.getVoteGranted());
}

TEST_F(TopoCoordTest, NodeDoesNotGrantDryRunVoteWhenConfigVersionDoesNotMatch) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);
    // set term to 1
    ASSERT(TopologyCoordinator::UpdateTermResult::kUpdatedTerm ==
           getTopoCoord().updateTerm(1, now()));
    // and make sure we voted in term 1
    ReplSetRequestVotesArgs argsForRealVote;
    argsForRealVote.initialize(
        BSON("replSetRequestVotes" << 1 << "setName"
                                   << "rs0"
                                   << "term"
                                   << 1LL
                                   << "candidateIndex"
                                   << 0LL
                                   << "configVersion"
                                   << 1LL
                                   << "lastCommittedOp"
                                   << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse responseForRealVote;
    OpTime lastAppliedOpTime;

    getTopoCoord().processReplSetRequestVotes(
        argsForRealVote, &responseForRealVote, lastAppliedOpTime);
    ASSERT_EQUALS("", responseForRealVote.getReason());
    ASSERT_TRUE(responseForRealVote.getVoteGranted());


    // mismatched configVersion
    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "rs0"
                                               << "dryRun"
                                               << true
                                               << "term"
                                               << 2LL
                                               << "candidateIndex"
                                               << 1LL
                                               << "configVersion"
                                               << 0LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime);
    ASSERT_EQUALS("candidate's config version differs from mine", response.getReason());
    ASSERT_EQUALS(1, response.getTerm());
    ASSERT_FALSE(response.getVoteGranted());
}

TEST_F(TopoCoordTest, NodeDoesNotGrantDryRunVoteWhenTermIsStale) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);
    // set term to 1
    ASSERT(TopologyCoordinator::UpdateTermResult::kUpdatedTerm ==
           getTopoCoord().updateTerm(1, now()));
    // and make sure we voted in term 1
    ReplSetRequestVotesArgs argsForRealVote;
    argsForRealVote.initialize(
        BSON("replSetRequestVotes" << 1 << "setName"
                                   << "rs0"
                                   << "term"
                                   << 1LL
                                   << "candidateIndex"
                                   << 0LL
                                   << "configVersion"
                                   << 1LL
                                   << "lastCommittedOp"
                                   << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse responseForRealVote;
    OpTime lastAppliedOpTime;

    getTopoCoord().processReplSetRequestVotes(
        argsForRealVote, &responseForRealVote, lastAppliedOpTime);
    ASSERT_EQUALS("", responseForRealVote.getReason());
    ASSERT_TRUE(responseForRealVote.getVoteGranted());

    // stale term
    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "rs0"
                                               << "dryRun"
                                               << true
                                               << "term"
                                               << 0LL
                                               << "candidateIndex"
                                               << 1LL
                                               << "configVersion"
                                               << 1LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime);
    ASSERT_EQUALS("candidate's term is lower than mine", response.getReason());
    ASSERT_EQUALS(1, response.getTerm());
    ASSERT_FALSE(response.getVoteGranted());
}

TEST_F(TopoCoordTest, GrantDryRunVoteEvenWhenTermHasBeenSeen) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);
    // set term to 1
    ASSERT(TopologyCoordinator::UpdateTermResult::kUpdatedTerm ==
           getTopoCoord().updateTerm(1, now()));
    // and make sure we voted in term 1
    ReplSetRequestVotesArgs argsForRealVote;
    argsForRealVote.initialize(
        BSON("replSetRequestVotes" << 1 << "setName"
                                   << "rs0"
                                   << "term"
                                   << 1LL
                                   << "candidateIndex"
                                   << 0LL
                                   << "configVersion"
                                   << 1LL
                                   << "lastCommittedOp"
                                   << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse responseForRealVote;
    OpTime lastAppliedOpTime;

    getTopoCoord().processReplSetRequestVotes(
        argsForRealVote, &responseForRealVote, lastAppliedOpTime);
    ASSERT_EQUALS("", responseForRealVote.getReason());
    ASSERT_TRUE(responseForRealVote.getVoteGranted());


    // repeat term
    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "rs0"
                                               << "dryRun"
                                               << true
                                               << "term"
                                               << 1LL
                                               << "candidateIndex"
                                               << 1LL
                                               << "configVersion"
                                               << 1LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime);
    ASSERT_EQUALS("", response.getReason());
    ASSERT_EQUALS(1, response.getTerm());
    ASSERT_TRUE(response.getVoteGranted());
}

TEST_F(TopoCoordTest, DoNotGrantDryRunVoteWhenOpTimeIsStale) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 1
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);
    // set term to 1
    ASSERT(TopologyCoordinator::UpdateTermResult::kUpdatedTerm ==
           getTopoCoord().updateTerm(1, now()));
    // and make sure we voted in term 1
    ReplSetRequestVotesArgs argsForRealVote;
    argsForRealVote.initialize(
        BSON("replSetRequestVotes" << 1 << "setName"
                                   << "rs0"
                                   << "term"
                                   << 1LL
                                   << "candidateIndex"
                                   << 0LL
                                   << "configVersion"
                                   << 1LL
                                   << "lastCommittedOp"
                                   << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse responseForRealVote;
    OpTime lastAppliedOpTime;

    getTopoCoord().processReplSetRequestVotes(
        argsForRealVote, &responseForRealVote, lastAppliedOpTime);
    ASSERT_EQUALS("", responseForRealVote.getReason());
    ASSERT_TRUE(responseForRealVote.getVoteGranted());


    // stale OpTime
    ReplSetRequestVotesArgs args;
    args.initialize(BSON("replSetRequestVotes" << 1 << "setName"
                                               << "rs0"
                                               << "dryRun"
                                               << true
                                               << "term"
                                               << 3LL
                                               << "candidateIndex"
                                               << 1LL
                                               << "configVersion"
                                               << 1LL
                                               << "lastCommittedOp"
                                               << BSON("ts" << Timestamp(10, 0) << "term" << 0LL)));
    ReplSetRequestVotesResponse response;
    OpTime lastAppliedOpTime2 = {Timestamp(20, 0), 0};

    getTopoCoord().processReplSetRequestVotes(args, &response, lastAppliedOpTime2);
    ASSERT_EQUALS("candidate's data is staler than mine", response.getReason());
    ASSERT_EQUALS(1, response.getTerm());
    ASSERT_FALSE(response.getVoteGranted());
}

TEST_F(TopoCoordTest, NodeTransitionsToRemovedIfCSRSButHaveNoReadCommittedSupport) {
    ON_BLOCK_EXIT([]() { serverGlobalParams.clusterRole = ClusterRole::None; });
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    TopologyCoordinatorImpl::Options options;
    options.clusterRole = ClusterRole::ConfigServer;
    setOptions(options);
    getTopoCoord().setStorageEngineSupportsReadCommitted(false);

    updateConfig(BSON("_id"
                      << "rs0"
                      << "protocolVersion"
                      << 1
                      << "version"
                      << 1
                      << "configsvr"
                      << true
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);
    ASSERT_EQUALS(MemberState::RS_REMOVED, getTopoCoord().getMemberState().s);
}

TEST_F(TopoCoordTest, NodeBecomesSecondaryAsNormalWhenReadCommittedSupportedAndCSRS) {
    ON_BLOCK_EXIT([]() { serverGlobalParams.clusterRole = ClusterRole::None; });
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    TopologyCoordinatorImpl::Options options;
    options.clusterRole = ClusterRole::ConfigServer;
    setOptions(options);
    getTopoCoord().setStorageEngineSupportsReadCommitted(true);

    updateConfig(BSON("_id"
                      << "rs0"
                      << "protocolVersion"
                      << 1
                      << "version"
                      << 1
                      << "configsvr"
                      << true
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself")
                                    << BSON("_id" << 20 << "host"
                                                  << "h2")
                                    << BSON("_id" << 30 << "host"
                                                  << "h3"))),
                 0);

    ASSERT_EQUALS(MemberState::RS_STARTUP2, getTopoCoord().getMemberState().s);
    getTopoCoord().setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_EQUALS(MemberState::RS_SECONDARY, getTopoCoord().getMemberState().s);
}

class HeartbeatResponseTestV1 : public TopoCoordTest {
public:
    virtual void setUp() {
        TopoCoordTest::setUp();
        updateConfig(BSON("_id"
                          << "rs0"
                          << "version"
                          << 5
                          << "members"
                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                   << "host1:27017")
                                        << BSON("_id" << 1 << "host"
                                                      << "host2:27017")
                                        << BSON("_id" << 2 << "host"
                                                      << "host3:27017"))
                          << "protocolVersion"
                          << 1
                          << "settings"
                          << BSON("heartbeatTimeoutSecs" << 5)),
                     0);
    }
};

TEST_F(HeartbeatResponseTestV1,
       ShouldChangeSyncSourceWhenFresherMemberDoesNotBuildIndexesAndNeitherDoWe) {
    // In this test, the TopologyCoordinator should tell us to change sync sources away from
    // "host2" and to "host3" despite "host3" not building indexes because we do not build
    // indexes either and "host2" is more than maxSyncSourceLagSecs(30) behind "host3"
    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(4, 0), 0);
    // ahead by more than maxSyncSourceLagSecs (30)
    OpTime fresherLastOpTimeApplied = OpTime(Timestamp(3005, 0), 0);

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 7
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself"
                                               << "buildIndexes"
                                               << false
                                               << "priority"
                                               << 0)
                                    << BSON("_id" << 1 << "host"
                                                  << "host2")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3"
                                                  << "buildIndexes"
                                                  << false
                                                  << "priority"
                                                  << 0))),
                 0);
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_SECONDARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    fresherLastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // set up complete, time for actual check
    startCapturingLogMessages();
    ASSERT_TRUE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(lastOpTimeApplied), now()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("re-evaluating sync source"));
}

TEST_F(HeartbeatResponseTestV1, NodeReturnsBadValueWhenProcessingPV0ElectionCommandsInPV1) {
    // Both the replSetFresh and replSetElect commands should fail in PV1.
    ReplicationCoordinator::ReplSetFreshArgs freshArgs;
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");
    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(freshArgs, Date_t(), OpTime(), &responseBuilder, &status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_EQUALS("replset: incompatible replset protocol version: 1", status.reason());
    ASSERT_TRUE(responseBuilder.obj().isEmpty());

    BSONObjBuilder electResponseBuilder;
    ReplicationCoordinator::ReplSetElectArgs electArgs;
    status = internalErrorStatus;
    getTopoCoord().prepareElectResponse(
        electArgs, Date_t(), OpTime(), &electResponseBuilder, &status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_EQUALS("replset: incompatible replset protocol version: 1", status.reason());
    ASSERT_TRUE(electResponseBuilder.obj().isEmpty());
}

TEST_F(HeartbeatResponseTestV1,
       ShouldChangeSyncSourceWhenUpstreamNodeHasNoSyncSourceAndIsNotPrimary) {
    // In this test, the TopologyCoordinator will tell us change our sync source away from "host2"
    // when it is not ahead of us, unless it is PRIMARY or has a sync source of its own.
    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(400, 0), 0);

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    // Show we like host2 while it is primary.
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), lastOpTimeApplied, makeMetadata(lastOpTimeApplied, 1), now()));

    // Show that we also like host2 while it has a sync source.
    nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    lastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), lastOpTimeApplied, makeMetadata(lastOpTimeApplied, 2, 2), now()));

    // Show that we do not like it when it is not PRIMARY and lacks a sync source and lacks progress
    // beyond our own.
    nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    lastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), lastOpTimeApplied, makeMetadata(lastOpTimeApplied), now()));

    // Sometimes the heartbeat is stale and the metadata says it's the primary. Trust the metadata.
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"),
        lastOpTimeApplied,
        makeMetadata(lastOpTimeApplied, 1 /* host2 is primary */, -1 /* no sync source */),
        now()));

    // But if it is secondary and has some progress beyond our own, we still like it.
    OpTime newerThanLastOpTimeApplied = OpTime(Timestamp(500, 0), 0);
    nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    newerThanLastOpTimeApplied,
                                    newerThanLastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), lastOpTimeApplied, makeMetadata(newerThanLastOpTimeApplied), now()));
}

TEST_F(HeartbeatResponseTestV1, ShouldNotChangeSyncSourceWhenFresherMemberIsDown) {
    // In this test, the TopologyCoordinator should not tell us to change sync sources away from
    // "host2" and to "host3" despite "host2" being more than maxSyncSourceLagSecs(30) behind
    // "host3", since "host3" is down
    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(400, 0), 0);
    // ahead by more than maxSyncSourceLagSecs (30)
    OpTime fresherLastOpTimeApplied = OpTime(Timestamp(3005, 0), 0);

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_SECONDARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    fresherLastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // set up complete, time for actual check
    nextAction = receiveDownHeartbeat(HostAndPort("host3"), "rs0", lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(lastOpTimeApplied), now()));
}

TEST_F(HeartbeatResponseTestV1, ShouldNotChangeSyncSourceWhileFresherMemberIsBlackListed) {
    // In this test, the TopologyCoordinator should not tell us to change sync sources away from
    // "host2" and to "host3" despite "host2" being more than maxSyncSourceLagSecs(30) behind
    // "host3", since "host3" is blacklisted
    // Then, confirm that unblacklisting only works if time has passed the blacklist time.
    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(400, 0), 0);
    // ahead by more than maxSyncSourceLagSecs (30)
    OpTime fresherLastOpTimeApplied = OpTime(Timestamp(3005, 0), 0);

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_SECONDARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    fresherLastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    getTopoCoord().blacklistSyncSource(HostAndPort("host3"), now() + Milliseconds(100));

    // set up complete, time for actual check
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(lastOpTimeApplied), now()));

    // unblacklist with too early a time (node should remained blacklisted)
    getTopoCoord().unblacklistSyncSource(HostAndPort("host3"), now() + Milliseconds(90));
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(lastOpTimeApplied), now()));

    // unblacklist and it should succeed
    getTopoCoord().unblacklistSyncSource(HostAndPort("host3"), now() + Milliseconds(100));
    startCapturingLogMessages();
    ASSERT_TRUE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(lastOpTimeApplied), now()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("re-evaluating sync source"));
}

TEST_F(HeartbeatResponseTestV1, ShouldNotChangeSyncSourceIfNodeIsFreshByHeartbeatButNotMetadata) {
    // In this test, the TopologyCoordinator should not tell us to change sync sources away from
    // "host2" and to "host3" since "host2" is only more than maxSyncSourceLagSecs(30) behind
    // "host3" according to metadata, not heartbeat data.
    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(4, 0), 0);
    // ahead by more than maxSyncSourceLagSecs (30)
    OpTime fresherLastOpTimeApplied = OpTime(Timestamp(3005, 0), 0);

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_SECONDARY,
                                                            election,
                                                            fresherLastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    fresherLastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // set up complete, time for actual check
    startCapturingLogMessages();
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(lastOpTimeApplied), now()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("re-evaluating sync source"));
}

TEST_F(HeartbeatResponseTestV1, ShouldNotChangeSyncSourceIfNodeIsStaleByHeartbeatButNotMetadata) {
    // In this test, the TopologyCoordinator should not tell us to change sync sources away from
    // "host2" and to "host3" since "host2" is only more than maxSyncSourceLagSecs(30) behind
    // "host3" according to heartbeat data, not metadata.
    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(4, 0), 0);
    // ahead by more than maxSyncSourceLagSecs (30)
    OpTime fresherLastOpTimeApplied = OpTime(Timestamp(3005, 0), 0);

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_SECONDARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    fresherLastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // set up complete, time for actual check
    startCapturingLogMessages();
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(fresherLastOpTimeApplied), now()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("re-evaluating sync source"));
}

TEST_F(HeartbeatResponseTestV1, ShouldChangeSyncSourceWhenFresherMemberExists) {
    // In this test, the TopologyCoordinator should tell us to change sync sources away from
    // "host2" and to "host3" since "host2" is more than maxSyncSourceLagSecs(30) behind "host3"
    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(4, 0), 0);
    // ahead by more than maxSyncSourceLagSecs (30)
    OpTime fresherLastOpTimeApplied = OpTime(Timestamp(3005, 0), 0);

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_SECONDARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    fresherLastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // set up complete, time for actual check
    startCapturingLogMessages();
    ASSERT_TRUE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(lastOpTimeApplied), now()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("re-evaluating sync source"));
}

TEST_F(HeartbeatResponseTestV1, ShouldNotChangeSyncSourceWhenMemberHasYetToHeartbeatUs) {
    // In this test, the TopologyCoordinator should not tell us to change sync sources away from
    // "host2" since we do not use the member's heartbeatdata in pv1.
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(), now()));
}

TEST_F(HeartbeatResponseTestV1, ShouldNotChangeSyncSourceWhenMemberNotInConfig) {
    // In this test, the TopologyCoordinator should tell us to change sync sources away from
    // "host4" since "host4" is absent from the config of version 10.
    ReplSetMetadata metadata(0, OpTime(), OpTime(), 10, OID(), -1, -1);
    ASSERT_TRUE(
        getTopoCoord().shouldChangeSyncSource(HostAndPort("host4"), OpTime(), metadata, now()));
}

// TODO(dannenberg) figure out what this is trying to test..
TEST_F(HeartbeatResponseTestV1, ReconfigNodeRemovedBetweenHeartbeatRequestAndRepsonse) {
    OpTime election = OpTime(Timestamp(14, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(13, 0), 0);

    // all three members up and secondaries
    setSelfMemberState(MemberState::RS_SECONDARY);

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    lastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // now request from host3 and receive after host2 has been removed via reconfig
    getTopoCoord().prepareHeartbeatRequestV1(now()++, "rs0", HostAndPort("host3"));

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 2
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017"))
                      << "protocolVersion"
                      << 1),
                 0);

    ReplSetHeartbeatResponse hb;
    hb.initialize(BSON("ok" << 1 << "v" << 1 << "state" << MemberState::RS_PRIMARY), 0);
    hb.setDurableOpTime(lastOpTimeApplied);
    hb.setElectionTime(election.getTimestamp());
    StatusWith<ReplSetHeartbeatResponse> hbResponse = StatusWith<ReplSetHeartbeatResponse>(hb);
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        now()++, Milliseconds(0), HostAndPort("host3"), hbResponse, lastOpTimeApplied);

    // primary should not be set and we should perform NoAction in response
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(action.getAction());
}

// TODO(dannenberg) figure out what this is trying to test..
TEST_F(HeartbeatResponseTestV1, ReconfigBetweenHeartbeatRequestAndRepsonse) {
    OpTime election = OpTime(Timestamp(14, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(13, 0), 0);

    // all three members up and secondaries
    setSelfMemberState(MemberState::RS_SECONDARY);

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    lastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // now request from host3 and receive after host2 has been removed via reconfig
    getTopoCoord().prepareHeartbeatRequestV1(now()++, "rs0", HostAndPort("host3"));

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 2
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))
                      << "protocolVersion"
                      << 1),
                 0);

    ReplSetHeartbeatResponse hb;
    hb.initialize(BSON("ok" << 1 << "v" << 1 << "state" << MemberState::RS_PRIMARY), 0);
    hb.setDurableOpTime(lastOpTimeApplied);
    hb.setElectionTime(election.getTimestamp());
    StatusWith<ReplSetHeartbeatResponse> hbResponse = StatusWith<ReplSetHeartbeatResponse>(hb);
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        now()++, Milliseconds(0), HostAndPort("host3"), hbResponse, lastOpTimeApplied);

    // now primary should be host3, index 1, and we should perform NoAction in response
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(action.getAction());
}

TEST_F(HeartbeatResponseTestV1, NodeDoesNotUpdateHeartbeatDataIfNodeIsAbsentFromConfig) {
    OpTime election = OpTime(Timestamp(5, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host9"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1, RelinquishPrimaryWhenMajorityOfVotersIsNoLongerVisible) {
    // Become PRIMARY.
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    makeSelfPrimary(Timestamp(2, 0));
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    // Become aware of other nodes.
    heartbeatFromMember(
        HostAndPort("host2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(1, 0), 0));
    heartbeatFromMember(
        HostAndPort("host2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(1, 0), 0));
    heartbeatFromMember(HostAndPort("host3"), "rs0", MemberState::RS_SECONDARY, OpTime());
    heartbeatFromMember(HostAndPort("host3"), "rs0", MemberState::RS_SECONDARY, OpTime());

    // Lose that awareness, but we are not going to step down, because stepdown only
    // depends on liveness.
    HeartbeatResponseAction nextAction =
        receiveDownHeartbeat(HostAndPort("host2"), "rs0", OpTime(Timestamp(100, 0), 0));
    ASSERT_NO_ACTION(nextAction.getAction());
    nextAction = receiveDownHeartbeat(HostAndPort("host3"), "rs0", OpTime(Timestamp(100, 0), 0));
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());
}

TEST_F(HeartbeatResponseTestV1,
       ScheduleAPriorityTakeoverWhenElectableAndReceiveHeartbeatFromLowerPriorityPrimary) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 5
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017"
                                               << "priority"
                                               << 2)
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 6 << "host"
                                                  << "host7:27017"))
                      << "protocolVersion"
                      << 1
                      << "settings"

                      << BSON("heartbeatTimeoutSecs" << 5)),
                 0);

    setSelfMemberState(MemberState::RS_SECONDARY);

    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(300, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_EQUALS(HeartbeatResponseAction::PriorityTakeover, nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
}

TEST_F(HeartbeatResponseTestV1, UpdateHeartbeatDataTermPreventsPriorityTakeover) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 5
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host0:27017"
                                               << "priority"
                                               << 2)
                                    << BSON("_id" << 1 << "host"
                                                  << "host1:27017"
                                                  << "priority"
                                                  << 3)
                                    << BSON("_id" << 2 << "host"
                                                  << "host2:27017"))
                      << "settings"
                      << BSON("heartbeatTimeoutSecs" << 5)),
                 0);

    setSelfMemberState(MemberState::RS_SECONDARY);

    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(300, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());

    // Host 2 is the current primary in term 1.
    getTopoCoord().updateTerm(1, now());
    ASSERT_EQUALS(getTopoCoord().getTerm(), 1);
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_EQUALS(HeartbeatResponseAction::PriorityTakeover, nextAction.getAction());
    ASSERT_EQUALS(2, getCurrentPrimaryIndex());

    // Heartbeat from a secondary node shouldn't schedule a priority takeover.
    nextAction = receiveUpHeartbeat(HostAndPort("host1"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    election,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(2, getCurrentPrimaryIndex());

    now()++;
    // Host 1 starts an election due to higher priority by sending vote requests.
    // Vote request updates my term.
    getTopoCoord().updateTerm(2, now());

    // This heartbeat shouldn't schedule priority takeover, because the current primary
    // host 1 is not in my term.
    nextAction = receiveUpHeartbeat(HostAndPort("host1"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    election,
                                    lastOpTimeApplied);
    ASSERT_EQUALS(HeartbeatResponseAction::NoAction, nextAction.getAction());
    ASSERT_EQUALS(2, getCurrentPrimaryIndex());
}

TEST_F(HeartbeatResponseTestV1,
       ScheduleElectionIfAMajorityOfVotersIsVisibleEvenThoughATrueMajorityIsNot) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 5
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"
                                                  << "votes"
                                                  << 0
                                                  << "priority"
                                                  << 0)
                                    << BSON("_id" << 3 << "host"
                                                  << "host4:27017"
                                                  << "votes"
                                                  << 0
                                                  << "priority"
                                                  << 0)
                                    << BSON("_id" << 4 << "host"
                                                  << "host5:27017"
                                                  << "votes"
                                                  << 0
                                                  << "priority"
                                                  << 0)
                                    << BSON("_id" << 5 << "host"
                                                  << "host6:27017"
                                                  << "votes"
                                                  << 0
                                                  << "priority"
                                                  << 0)
                                    << BSON("_id" << 6 << "host"
                                                  << "host7:27017"))
                      << "protocolVersion"
                      << 1
                      << "settings"
                      << BSON("heartbeatTimeoutSecs" << 5)),
                 0);

    setSelfMemberState(MemberState::RS_SECONDARY);

    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(300, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    // Make sure all non-voting nodes are down, that way we do not have a majority of nodes
    // but do have a majority of votes since one of two voting members is up and so are we.
    nextAction = receiveDownHeartbeat(HostAndPort("host3"), "rs0", lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    nextAction = receiveDownHeartbeat(HostAndPort("host4"), "rs0", lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    nextAction = receiveDownHeartbeat(HostAndPort("host5"), "rs0", lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    nextAction = receiveDownHeartbeat(HostAndPort("host6"), "rs0", lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    nextAction = receiveUpHeartbeat(HostAndPort("host7"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    lastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    // We are electable now.
    ASSERT_OK(getTopoCoord().becomeCandidateIfElectable(now(), lastOpTimeApplied));
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1, ScheduleElectionWhenPrimaryIsMarkedDownAndWeAreElectable) {
    setSelfMemberState(MemberState::RS_SECONDARY);

    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(399, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    election,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    // We are electable now.
    ASSERT_OK(getTopoCoord().becomeCandidateIfElectable(now(), lastOpTimeApplied));
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotStandForElectionWhenPrimaryIsMarkedDownViaHeartbeatButWeAreAnArbiter) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 5
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017"
                                               << "arbiterOnly"
                                               << true)
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))
                      << "protocolVersion"
                      << 1),
                 0);

    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(300, 0), 0);

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                                            "rs0",
                                                            MemberState::RS_SECONDARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());

    nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                    "rs0",
                                    MemberState::RS_PRIMARY,
                                    election,
                                    election,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotStandForElectionWhenPrimaryIsMarkedDownViaHeartbeatButWeHaveStepdownWait) {
    setSelfMemberState(MemberState::RS_SECONDARY);

    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(300, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    election,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // Freeze node to set stepdown wait.
    BSONObjBuilder response;
    getTopoCoord().prepareFreezeResponse(now()++, 20, &response);

    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotStandForElectionWhenPrimaryIsMarkedDownViaHeartbeatButWeAreInRecovering) {
    setSelfMemberState(MemberState::RS_RECOVERING);

    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(300, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotStandForElectionWhenPrimaryIsMarkedDownViaHeartbeatButWeAreInStartup) {
    setSelfMemberState(MemberState::RS_STARTUP);

    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(300, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    election,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotStandForElectionWhenPrimaryIsMarkedDownViaHeartbeatButWeHaveZeroPriority) {
    setSelfMemberState(MemberState::RS_SECONDARY);

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 5
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017"
                                               << "priority"
                                               << 0)
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))
                      << "protocolVersion"
                      << 1),
                 0);

    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(300, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    election,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotStandForElectionWhenPrimaryIsMarkedDownViaHeartbeatButWeCannotSeeMajority) {
    setSelfMemberState(MemberState::RS_SECONDARY);

    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(300, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1, NodeDoesNotStepDownSelfWhenRemoteNodeWasElectedMoreRecently) {
    // This test exists to ensure we do not resolve multiprimary states via heartbeats in PV1.
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    makeSelfPrimary(Timestamp(2, 0));

    OpTime election = OpTime(Timestamp(4, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    ASSERT_EQUALS(0, getCurrentPrimaryIndex());
    receiveUpHeartbeat(HostAndPort("host3"),
                       "rs0",
                       MemberState::RS_SECONDARY,
                       election,
                       election,
                       lastOpTimeApplied);
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    // If the other PRIMARY falls down, this node should set its primaryIndex to itself.
    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotStepDownRemoteWhenHeartbeatResponseContainsALessFreshHigherPriorityNode) {
    // In this test, the Topology coordinator sees a PRIMARY ("host2") and then sees a higher
    // priority and stale node ("host3"). It responds with NoAction, as it should in all
    // multiprimary states in PV1.
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 6
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"
                                                  << "priority"
                                                  << 3))
                      << "protocolVersion"
                      << 1
                      << "settings"
                      << BSON("heartbeatTimeoutSecs" << 5)),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    OpTime election = OpTime(Timestamp(1000, 0), 0);
    OpTime stale = OpTime();

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(
        HostAndPort("host2"), "rs0", MemberState::RS_PRIMARY, election, election, election);
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveUpHeartbeat(
        HostAndPort("host3"), "rs0", MemberState::RS_SECONDARY, election, stale, election);
    ASSERT_NO_ACTION(nextAction.getAction());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotStepDownSelfWhenHeartbeatResponseContainsALessFreshHigherPriorityNode) {
    // In this test, the Topology coordinator becomes PRIMARY and then sees a higher priority
    // and stale node ("host3"). It responds with NoAction, as it should in all
    // multiprimary states in PV1.
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 6
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"
                                                  << "priority"
                                                  << 3))
                      << "protocolVersion"
                      << 1
                      << "settings"
                      << BSON("heartbeatTimeoutSecs" << 5)),
                 0);
    OpTime election = OpTime(Timestamp(1000, 0), 0);
    OpTime staleTime = OpTime();

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    makeSelfPrimary(election.getTimestamp());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(
        HostAndPort("host3"), "rs0", MemberState::RS_SECONDARY, election, staleTime, election);
    ASSERT_NO_ACTION(nextAction.getAction());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotStepDownSelfWhenHeartbeatResponseContainsAFresherHigherPriorityNode) {
    // In this test, the Topology coordinator becomes PRIMARY and then sees a higher priority
    // and equally fresh node ("host3"). It responds with NoAction, as it should in all
    // multiprimary states in PV1.
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 6
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"
                                                  << "priority"
                                                  << 3))
                      << "protocolVersion"
                      << 1
                      << "settings"
                      << BSON("heartbeatTimeoutSecs" << 5)),
                 0);
    OpTime election = OpTime(Timestamp(1000, 0), 0);

    getTopoCoord().setFollowerMode(MemberState::RS_SECONDARY);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    makeSelfPrimary(election.getTimestamp());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(
        HostAndPort("host3"), "rs0", MemberState::RS_SECONDARY, election, election, election);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(-1, nextAction.getPrimaryConfigIndex());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotStepDownRemoteWhenHeartbeatResponseContainsAFresherHigherPriorityNode) {
    // In this test, the Topology coordinator sees a PRIMARY ("host2") and then sees a higher
    // priority and similarly fresh node ("host3"). It responds with NoAction, as it should
    // in all multiprimary states in PV1.
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 6
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"
                                                  << "priority"
                                                  << 3))
                      << "protocolVersion"
                      << 1
                      << "settings"
                      << BSON("heartbeatTimeoutSecs" << 5)),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(13, 0), 0);
    OpTime slightlyLessFreshLastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    slightlyLessFreshLastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_EQUALS(HeartbeatResponseAction::NoAction, nextAction.getAction());
}

TEST_F(HeartbeatResponseTestV1, NodeDoesNotStepDownSelfWhenRemoteNodeWasElectedLessRecently) {
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    makeSelfPrimary(Timestamp(5, 0));

    OpTime election = OpTime(Timestamp(4, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    ASSERT_EQUALS(0, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotUpdatePrimaryIndexWhenAHeartbeatMakesNodeAwareOfANewerPrimary) {
    OpTime election = OpTime(Timestamp(4, 0), 0);
    OpTime election2 = OpTime(Timestamp(5, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_PRIMARY,
                                    election2,
                                    election,
                                    lastOpTimeApplied);
    // Second primary does not change primary index.
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1,
       NodeDoesNotUpdatePrimaryIndexWhenAHeartbeatMakesNodeAwareOfAnOlderPrimary) {
    OpTime election = OpTime(Timestamp(5, 0), 0);
    OpTime election2 = OpTime(Timestamp(4, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_PRIMARY,
                                    election2,
                                    election,
                                    lastOpTimeApplied);
    // Second primary does not change primary index.
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1, UpdatePrimaryIndexWhenAHeartbeatMakesNodeAwareOfANewPrimary) {
    OpTime election = OpTime(Timestamp(5, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTestV1, NodeDoesNotRetryHeartbeatIfTheFirstFailureTakesTheFullTime) {
    // Confirm that the topology coordinator does not schedule an immediate heartbeat retry
    // if the heartbeat timeout period expired before the initial request completed.

    HostAndPort target("host2", 27017);
    Date_t firstRequestDate = unittest::assertGet(dateFromISOString("2014-08-29T13:00Z"));

    // Initial heartbeat request prepared, at t + 0.
    std::pair<ReplSetHeartbeatArgsV1, Milliseconds> request =
        getTopoCoord().prepareHeartbeatRequestV1(firstRequestDate, "rs0", target);
    // 5 seconds to successfully complete the heartbeat before the timeout expires.
    ASSERT_EQUALS(5000, durationCount<Milliseconds>(request.second));

    // Initial heartbeat request fails at t + 5000ms
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate + Milliseconds(5000),  // Entire heartbeat period elapsed;
                                                // no retry allowed.
        Milliseconds(4990),                     // Spent 4.99 of the 5 seconds in the network.
        target,
        StatusWith<ReplSetHeartbeatResponse>(ErrorCodes::ExceededTimeLimit, "Took too long"),
        OpTime(Timestamp(0, 0), 0));  // We've never applied anything.

    ASSERT_EQUALS(HeartbeatResponseAction::NoAction, action.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    // Because the heartbeat timed out, we'll retry in half of the election timeout.
    ASSERT_EQUALS(firstRequestDate + Milliseconds(5000) +
                      ReplicaSetConfig::kDefaultElectionTimeoutPeriod / 2,
                  action.getNextHeartbeatStartDate());
}

TEST_F(HeartbeatResponseTestV1, ShouldNotChangeSyncSourceWhenFresherMemberDoesNotBuildIndexes) {
    // In this test, the TopologyCoordinator should not tell us to change sync sources away
    // from "host2" and to "host3" despite "host2" being more than maxSyncSourceLagSecs(30) behind
    // "host3", since "host3" does not build indexes
    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(4, 0), 0);
    // ahead by more than maxSyncSourceLagSecs (30)
    OpTime fresherLastOpTimeApplied = OpTime(Timestamp(3005, 0), 0);

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 6
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "hself")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3"
                                                  << "buildIndexes"
                                                  << false
                                                  << "priority"
                                                  << 0))
                      << "protocolVersion"
                      << 1),
                 0);
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_SECONDARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    fresherLastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // set up complete, time for actual check
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(lastOpTimeApplied), now()));
}

TEST_F(HeartbeatResponseTestV1, ShouldNotChangeSyncSourceWhenFresherMemberIsNotReadable) {
    // In this test, the TopologyCoordinator should not tell us to change sync sources away
    // from "host2" and to "host3" despite "host2" being more than maxSyncSourceLagSecs(30) behind
    // "host3", since "host3" is in a non-readable mode (RS_ROLLBACK)
    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(4, 0), 0);
    // ahead by more than maxSyncSourceLagSecs (30)
    OpTime fresherLastOpTimeApplied = OpTime(Timestamp(3005, 0), 0);

    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_SECONDARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_ROLLBACK,
                                    election,
                                    fresherLastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // set up complete, time for actual check
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(lastOpTimeApplied), now()));
}

class HeartbeatResponseTestOneRetryV1 : public HeartbeatResponseTestV1 {
public:
    virtual void setUp() {
        HeartbeatResponseTestV1::setUp();

        // Bring up the node we are heartbeating.
        _target = HostAndPort("host2", 27017);
        Date_t _upRequestDate = unittest::assertGet(dateFromISOString("2014-08-29T12:55Z"));
        std::pair<ReplSetHeartbeatArgsV1, Milliseconds> uppingRequest =
            getTopoCoord().prepareHeartbeatRequestV1(_upRequestDate, "rs0", _target);
        HeartbeatResponseAction upAction = getTopoCoord().processHeartbeatResponse(
            _upRequestDate,
            Milliseconds(0),
            _target,
            makeStatusWith<ReplSetHeartbeatResponse>(),
            OpTime(Timestamp(0, 0), 0));  // We've never applied anything.
        ASSERT_EQUALS(HeartbeatResponseAction::NoAction, upAction.getAction());
        ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());


        // Time of first request for this heartbeat period
        _firstRequestDate = unittest::assertGet(dateFromISOString("2014-08-29T13:00Z"));

        // Initial heartbeat attempt prepared, at t + 0.
        std::pair<ReplSetHeartbeatArgsV1, Milliseconds> request =
            getTopoCoord().prepareHeartbeatRequestV1(_firstRequestDate, "rs0", _target);
        // 5 seconds to successfully complete the heartbeat before the timeout expires.
        ASSERT_EQUALS(5000, durationCount<Milliseconds>(request.second));

        // Initial heartbeat request fails at t + 4000ms
        HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
            _firstRequestDate + Seconds(4),  // 4 seconds elapsed, retry allowed.
            Milliseconds(3990),              // Spent 3.99 of the 4 seconds in the network.
            _target,
            StatusWith<ReplSetHeartbeatResponse>(ErrorCodes::ExceededTimeLimit, "Took too long"),
            OpTime(Timestamp(0, 0), 0));  // We've never applied anything.

        ASSERT_EQUALS(HeartbeatResponseAction::NoAction, action.getAction());
        ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
        // Because the heartbeat failed without timing out, we expect to retry immediately.
        ASSERT_EQUALS(_firstRequestDate + Seconds(4), action.getNextHeartbeatStartDate());

        // First heartbeat retry prepared, at t + 4000ms.
        request = getTopoCoord().prepareHeartbeatRequestV1(
            _firstRequestDate + Milliseconds(4000), "rs0", _target);
        // One second left to complete the heartbeat.
        ASSERT_EQUALS(1000, durationCount<Milliseconds>(request.second));

        // Ensure a single failed heartbeat did not cause the node to be marked down
        BSONObjBuilder statusBuilder;
        Status resultStatus(ErrorCodes::InternalError, "prepareStatusResponse didn't set result");
        getTopoCoord().prepareStatusResponse(
            TopologyCoordinator::ReplSetStatusArgs{_firstRequestDate + Milliseconds(4000),
                                                   10,
                                                   OpTime(Timestamp(100, 0), 0),
                                                   OpTime(Timestamp(100, 0), 0),
                                                   OpTime(),
                                                   OpTime()},
            &statusBuilder,
            &resultStatus);
        ASSERT_OK(resultStatus);
        BSONObj rsStatus = statusBuilder.obj();
        std::vector<BSONElement> memberArray = rsStatus["members"].Array();
        BSONObj member1Status = memberArray[1].Obj();

        ASSERT_EQUALS(1, member1Status["_id"].Int());
        ASSERT_EQUALS(1, member1Status["health"].Double());

        ASSERT_EQUALS(Timestamp(0, 0),
                      Timestamp(rsStatus["optimes"]["lastCommittedOpTime"]["ts"].timestampValue()));
        ASSERT_EQUALS(-1LL, rsStatus["optimes"]["lastCommittedOpTime"]["t"].numberLong());
        ASSERT_FALSE(rsStatus["optimes"].Obj().hasField("readConcernMajorityOpTime"));
    }

    Date_t firstRequestDate() {
        return _firstRequestDate;
    }

    HostAndPort target() {
        return _target;
    }

private:
    Date_t _firstRequestDate;
    HostAndPort _target;
};

TEST_F(HeartbeatResponseTestOneRetryV1,
       NodeDoesNotRetryHeartbeatIfTheFirstAndSecondFailuresExhaustTheFullTime) {
    // Confirm that the topology coordinator does not schedule an second heartbeat retry if
    // the heartbeat timeout period expired before the first retry completed.
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(5010),  // Entire heartbeat period elapsed;
                                                  // no retry allowed.
        Milliseconds(1000),                       // Spent 1 of the 1.01 seconds in the network.
        target(),
        StatusWith<ReplSetHeartbeatResponse>(ErrorCodes::ExceededTimeLimit, "Took too long"),
        OpTime(Timestamp(0, 0), 0));  // We've never applied anything.

    ASSERT_EQUALS(HeartbeatResponseAction::NoAction, action.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    // Because the heartbeat timed out, we'll retry in half of the election timeout.
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(5010) +
                      ReplicaSetConfig::kDefaultElectionTimeoutPeriod / 2,
                  action.getNextHeartbeatStartDate());
}

class HeartbeatResponseTestTwoRetriesV1 : public HeartbeatResponseTestOneRetryV1 {
public:
    virtual void setUp() {
        HeartbeatResponseTestOneRetryV1::setUp();
        // First retry fails at t + 4500ms
        HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
            firstRequestDate() + Milliseconds(4500),  // 4.5 of the 5 seconds elapsed;
                                                      // could retry.
            Milliseconds(400),  // Spent 0.4 of the 0.5 seconds in the network.
            target(),
            StatusWith<ReplSetHeartbeatResponse>(ErrorCodes::NodeNotFound, "Bad DNS?"),
            OpTime(Timestamp(0, 0), 0));  // We've never applied anything.
        ASSERT_EQUALS(HeartbeatResponseAction::NoAction, action.getAction());
        ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
        // Because the first retry failed without timing out, we expect to retry immediately.
        ASSERT_EQUALS(firstRequestDate() + Milliseconds(4500), action.getNextHeartbeatStartDate());

        // Second retry prepared at t + 4500ms.
        std::pair<ReplSetHeartbeatArgsV1, Milliseconds> request =
            getTopoCoord().prepareHeartbeatRequestV1(
                firstRequestDate() + Milliseconds(4500), "rs0", target());
        // 500ms left to complete the heartbeat.
        ASSERT_EQUALS(500, durationCount<Milliseconds>(request.second));

        // Ensure a second failed heartbeat did not cause the node to be marked down
        BSONObjBuilder statusBuilder;
        Status resultStatus(ErrorCodes::InternalError, "prepareStatusResponse didn't set result");
        getTopoCoord().prepareStatusResponse(
            TopologyCoordinator::ReplSetStatusArgs{firstRequestDate() + Seconds(4),
                                                   10,
                                                   OpTime(Timestamp(100, 0), 0),
                                                   OpTime(Timestamp(100, 0), 0),
                                                   OpTime(),
                                                   OpTime()},
            &statusBuilder,
            &resultStatus);
        ASSERT_OK(resultStatus);
        BSONObj rsStatus = statusBuilder.obj();
        std::vector<BSONElement> memberArray = rsStatus["members"].Array();
        BSONObj member1Status = memberArray[1].Obj();

        ASSERT_EQUALS(1, member1Status["_id"].Int());
        ASSERT_EQUALS(1, member1Status["health"].Double());
    }
};

TEST_F(HeartbeatResponseTestTwoRetriesV1, NodeDoesNotRetryHeartbeatsAfterFailingTwiceInARow) {
    // Confirm that the topology coordinator attempts to retry a failed heartbeat two times
    // after initial failure, assuming that the heartbeat timeout (set to 5 seconds in the
    // fixture) has not expired.
    //
    // Failed heartbeats propose taking no action, other than scheduling the next heartbeat.
    // We can detect a retry vs the next regularly scheduled heartbeat because retries are
    // scheduled immediately, while subsequent heartbeats are scheduled after the hard-coded
    // heartbeat interval of 2 seconds.

    // Second retry fails at t + 4800ms
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(4800),  // 4.8 of the 5 seconds elapsed;
                                                  // could still retry.
        Milliseconds(100),                        // Spent 0.1 of the 0.3 seconds in the network.
        target(),
        StatusWith<ReplSetHeartbeatResponse>(ErrorCodes::NodeNotFound, "Bad DNS?"),
        OpTime(Timestamp(0, 0), 0));  // We've never applied anything.
    ASSERT_EQUALS(HeartbeatResponseAction::NoAction, action.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    // Because this is the second retry, rather than retry again, we expect to wait for half
    // of the election timeout interval of 2 seconds to elapse.
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(4800) +
                      ReplicaSetConfig::kDefaultElectionTimeoutPeriod / 2,
                  action.getNextHeartbeatStartDate());

    // Ensure a third failed heartbeat caused the node to be marked down
    BSONObjBuilder statusBuilder;
    Status resultStatus(ErrorCodes::InternalError, "prepareStatusResponse didn't set result");
    getTopoCoord().prepareStatusResponse(
        TopologyCoordinator::ReplSetStatusArgs{firstRequestDate() + Milliseconds(4900),
                                               10,
                                               OpTime(Timestamp(100, 0), 0),
                                               OpTime(Timestamp(100, 0), 0),
                                               OpTime(),
                                               OpTime()},
        &statusBuilder,
        &resultStatus);
    ASSERT_OK(resultStatus);
    BSONObj rsStatus = statusBuilder.obj();
    std::vector<BSONElement> memberArray = rsStatus["members"].Array();
    BSONObj member1Status = memberArray[1].Obj();

    ASSERT_EQUALS(1, member1Status["_id"].Int());
    ASSERT_EQUALS(0, member1Status["health"].Double());
}

TEST_F(HeartbeatResponseTestTwoRetriesV1, HeartbeatThreeNonconsecutiveFailures) {
    // Confirm that the topology coordinator does not mark a node down on three
    // nonconsecutive heartbeat failures.
    ReplSetHeartbeatResponse response;
    response.noteReplSet();
    response.setSetName("rs0");
    response.setState(MemberState::RS_SECONDARY);
    response.setElectable(true);
    response.setConfigVersion(5);

    // successful response (third response due to the two failures in setUp())
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(4500),
        Milliseconds(400),
        target(),
        StatusWith<ReplSetHeartbeatResponse>(response),
        OpTime(Timestamp(0, 0), 0));  // We've never applied anything.

    ASSERT_EQUALS(HeartbeatResponseAction::NoAction, action.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    // Because the heartbeat succeeded, we'll retry in half of the election timeout.
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(4500) +
                      ReplicaSetConfig::kDefaultElectionTimeoutPeriod / 2,
                  action.getNextHeartbeatStartDate());

    // request next heartbeat
    getTopoCoord().prepareHeartbeatRequestV1(
        firstRequestDate() + Milliseconds(6500), "rs0", target());
    // third failed response
    action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(7100),
        Milliseconds(400),
        target(),
        StatusWith<ReplSetHeartbeatResponse>(Status{ErrorCodes::HostUnreachable, ""}),
        OpTime(Timestamp(0, 0), 0));  // We've never applied anything.

    ASSERT_EQUALS(HeartbeatResponseAction::NoAction, action.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());

    // Ensure a third nonconsecutive heartbeat failure did not cause the node to be marked down
    BSONObjBuilder statusBuilder;
    Status resultStatus(ErrorCodes::InternalError, "prepareStatusResponse didn't set result");
    getTopoCoord().prepareStatusResponse(
        TopologyCoordinator::ReplSetStatusArgs{firstRequestDate() + Milliseconds(7000),
                                               600,
                                               OpTime(Timestamp(100, 0), 0),
                                               OpTime(Timestamp(100, 0), 0),
                                               OpTime(),
                                               OpTime()},
        &statusBuilder,
        &resultStatus);
    ASSERT_OK(resultStatus);
    BSONObj rsStatus = statusBuilder.obj();
    std::vector<BSONElement> memberArray = rsStatus["members"].Array();
    BSONObj member1Status = memberArray[1].Obj();

    ASSERT_EQUALS(1, member1Status["_id"].Int());
    ASSERT_EQUALS(1, member1Status["health"].Double());
}

class HeartbeatResponseHighVerbosityTestV1 : public HeartbeatResponseTestV1 {
public:
    virtual void setUp() {
        HeartbeatResponseTestV1::setUp();
        // set verbosity as high as the highest verbosity log message we'd like to check for
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(3));
    }

    virtual void tearDown() {
        HeartbeatResponseTestV1::tearDown();
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Log());
    }
};

// TODO(dannenberg) change the name and functionality of this to match what this claims it is
TEST_F(HeartbeatResponseHighVerbosityTestV1, UpdateHeartbeatDataOldConfig) {
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    // request heartbeat
    std::pair<ReplSetHeartbeatArgsV1, Milliseconds> request =
        getTopoCoord().prepareHeartbeatRequestV1(now()++, "rs0", HostAndPort("host2"));

    ReplSetHeartbeatResponse believesWeAreDownResponse;
    believesWeAreDownResponse.noteReplSet();
    believesWeAreDownResponse.setSetName("rs0");
    believesWeAreDownResponse.setState(MemberState::RS_SECONDARY);
    believesWeAreDownResponse.setElectable(true);
    believesWeAreDownResponse.noteStateDisagreement();
    startCapturingLogMessages();
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        now()++,            // Time is left.
        Milliseconds(400),  // Spent 0.4 of the 0.5 second in the network.
        HostAndPort("host2"),
        StatusWith<ReplSetHeartbeatResponse>(believesWeAreDownResponse),
        lastOpTimeApplied);
    stopCapturingLogMessages();
    ASSERT_NO_ACTION(action.getAction());
    ASSERT_EQUALS(1, countLogLinesContaining("host2:27017 thinks that we are down"));
}

// TODO(dannenberg) figure out why this test is useful
TEST_F(HeartbeatResponseHighVerbosityTestV1, UpdateHeartbeatDataSameConfig) {
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    // request heartbeat
    std::pair<ReplSetHeartbeatArgsV1, Milliseconds> request =
        getTopoCoord().prepareHeartbeatRequestV1(now()++, "rs0", HostAndPort("host2"));

    // construct a copy of the original config for log message checking later
    // see HeartbeatResponseTest for the origin of the original config
    ReplicaSetConfig originalConfig;
    originalConfig.initialize(BSON("_id"
                                   << "rs0"
                                   << "version"
                                   << 5
                                   << "members"
                                   << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                            << "host1:27017")
                                                 << BSON("_id" << 1 << "host"
                                                               << "host2:27017")
                                                 << BSON("_id" << 2 << "host"
                                                               << "host3:27017"))
                                   << "protocolVersion"
                                   << 1
                                   << "settings"
                                   << BSON("heartbeatTimeoutSecs" << 5)));

    ReplSetHeartbeatResponse sameConfigResponse;
    sameConfigResponse.noteReplSet();
    sameConfigResponse.setSetName("rs0");
    sameConfigResponse.setState(MemberState::RS_SECONDARY);
    sameConfigResponse.setElectable(true);
    sameConfigResponse.noteStateDisagreement();
    sameConfigResponse.setConfigVersion(2);
    sameConfigResponse.setConfig(originalConfig);
    startCapturingLogMessages();
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        now()++,            // Time is left.
        Milliseconds(400),  // Spent 0.4 of the 0.5 second in the network.
        HostAndPort("host2"),
        StatusWith<ReplSetHeartbeatResponse>(sameConfigResponse),
        lastOpTimeApplied);
    stopCapturingLogMessages();
    ASSERT_NO_ACTION(action.getAction());
    ASSERT_EQUALS(1,
                  countLogLinesContaining("Config from heartbeat response was "
                                          "same as ours."));
}

TEST_F(HeartbeatResponseHighVerbosityTestV1,
       LogMessageAndTakeNoActionWhenReceivingAHeartbeatResponseFromANodeThatIsNotInConfig) {
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    // request heartbeat
    std::pair<ReplSetHeartbeatArgsV1, Milliseconds> request =
        getTopoCoord().prepareHeartbeatRequestV1(now()++, "rs0", HostAndPort("host5"));

    ReplSetHeartbeatResponse memberMissingResponse;
    memberMissingResponse.noteReplSet();
    memberMissingResponse.setSetName("rs0");
    memberMissingResponse.setState(MemberState::RS_SECONDARY);
    memberMissingResponse.setElectable(true);
    memberMissingResponse.noteStateDisagreement();
    startCapturingLogMessages();
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        now()++,            // Time is left.
        Milliseconds(400),  // Spent 0.4 of the 0.5 second in the network.
        HostAndPort("host5"),
        StatusWith<ReplSetHeartbeatResponse>(memberMissingResponse),
        lastOpTimeApplied);
    stopCapturingLogMessages();
    ASSERT_NO_ACTION(action.getAction());
    ASSERT_EQUALS(1, countLogLinesContaining("Could not find host5:27017 in current config"));
}

TEST_F(HeartbeatResponseHighVerbosityTestV1,
       LogMessageAndTakeNoActionWhenReceivingAHeartbeatResponseFromANodeThatBelievesWeAreDown) {
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    // request heartbeat
    std::pair<ReplSetHeartbeatArgsV1, Milliseconds> request =
        getTopoCoord().prepareHeartbeatRequestV1(now()++, "rs0", HostAndPort("host2"));

    ReplSetHeartbeatResponse believesWeAreDownResponse;
    believesWeAreDownResponse.noteReplSet();
    believesWeAreDownResponse.setSetName("rs0");
    believesWeAreDownResponse.setState(MemberState::RS_SECONDARY);
    believesWeAreDownResponse.setElectable(true);
    believesWeAreDownResponse.noteStateDisagreement();
    startCapturingLogMessages();
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        now()++,            // Time is left.
        Milliseconds(400),  // Spent 0.4 of the 0.5 second in the network.
        HostAndPort("host2"),
        StatusWith<ReplSetHeartbeatResponse>(believesWeAreDownResponse),
        lastOpTimeApplied);
    stopCapturingLogMessages();
    ASSERT_NO_ACTION(action.getAction());
    ASSERT_EQUALS(1, countLogLinesContaining("host2:27017 thinks that we are down"));
}

}  // namespace
}  // namespace repl
}  // namespace mongo
