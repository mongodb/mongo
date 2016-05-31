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
        ASSERT_OK(config.initialize(cfg));
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

    // Make the metadata coming from sync source. Only set visibleOpTime.
    ReplSetMetadata makeMetadata(OpTime opTime = OpTime()) {
        return ReplSetMetadata(
            _topo->getTerm(), OpTime(), opTime, _currentConfig.getConfigVersion(), OID(), -1, -1);
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

        StatusWith<ReplSetHeartbeatResponse> hbResponse = responseStatus.isOK()
            ? StatusWith<ReplSetHeartbeatResponse>(hb)
            : StatusWith<ReplSetHeartbeatResponse>(responseStatus);

        getTopoCoord().prepareHeartbeatRequest(now(), setName, member);
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
    // h6 is not considered because it is outside the maxSyncLagSeconds window,
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

    // two rounds of heartbeat pings from each member
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

    // force should overrule other defaults
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());
    ASSERT_EQUALS(HostAndPort("h3"), getTopoCoord().getSyncSourceAddress());
    getTopoCoord().setForceSyncSourceIndex(1);
    // force should cause shouldChangeSyncSource() to return true
    // even if the currentSource is the force target
    ASSERT_TRUE(
        getTopoCoord().shouldChangeSyncSource(HostAndPort("h2"), OpTime(), makeMetadata(), now()));
    ASSERT_TRUE(
        getTopoCoord().shouldChangeSyncSource(HostAndPort("h3"), OpTime(), makeMetadata(), now()));
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

    // Mark nodes unauth, ensure that we have no source and are secondary
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
    OpTime oplogProgress(Timestamp(3, 4), 0);
    OpTime oplogDurable(Timestamp(3, 4), 1);
    OpTime lastCommittedOpTime(Timestamp(2, 3), -1);
    OpTime readConcernMajorityOpTime(Timestamp(4, 5), -1);
    std::string setName = "mySet";

    ReplSetHeartbeatResponse hb;
    hb.setConfigVersion(1);
    hb.setState(MemberState::RS_SECONDARY);
    hb.setElectionTime(electionTime);
    hb.setHbMsg("READY");
    hb.setAppliedOpTime(oplogProgress);
    hb.setDurableOpTime(oplogDurable);
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
    getTopoCoord().prepareHeartbeatRequest(startupTime + Milliseconds(1), setName, member);
    getTopoCoord().processHeartbeatResponse(
        startupTime + Milliseconds(2), Milliseconds(1), member, hbResponseGood, OpTime());
    getTopoCoord().prepareHeartbeatRequest(startupTime + Milliseconds(3), setName, member);
    Date_t timeoutTime =
        startupTime + Milliseconds(3) + ReplicaSetConfig::kDefaultHeartbeatTimeoutPeriod;

    StatusWith<ReplSetHeartbeatResponse> hbResponseDown =
        StatusWith<ReplSetHeartbeatResponse>(Status(ErrorCodes::HostUnreachable, ""));

    getTopoCoord().processHeartbeatResponse(
        timeoutTime, Milliseconds(5000), member, hbResponseDown, OpTime());

    member = HostAndPort("test1:1234");
    getTopoCoord().prepareHeartbeatRequest(startupTime + Milliseconds(2), setName, member);
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
        ASSERT_EQUALS(oplogProgress.getTimestamp(), optimes["appliedOpTime"].timestamp());
        ASSERT_EQUALS((oplogDurable).getTimestamp(), optimes["durableOpTime"].timestamp());
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
    ASSERT_EQUALS(Timestamp(), Timestamp(member0Status["optime"].timestampValue()));
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
    ASSERT_EQUALS(oplogProgress.getTimestamp(),
                  Timestamp(member1Status["optime"].timestampValue()));
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
    ASSERT_EQUALS(oplogProgress.getTimestamp(), Timestamp(selfStatus["optime"].timestampValue()));
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

TEST_F(TopoCoordTest, NodeReturnsReplicaSetNotFoundWhenFreshnessIsCheckedPriorToHavingAConfig) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    // if we do not have an index in the config, we should get ErrorCodes::ReplicaSetNotFound
    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_EQUALS(ErrorCodes::ReplicaSetNotFound, status);
    ASSERT_EQUALS("Cannot participate in elections because not initialized", status.reason());
    ASSERT_TRUE(responseBuilder.obj().isEmpty());
}

TEST_F(TopoCoordTest,
       NodeReturnsReplicaSetNotFoundWhenFreshnessIsCheckedWithTheIncorrectReplSetName) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    // Test with incorrect replset name
    args.setName = "fakeset";

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_EQUALS(ErrorCodes::ReplicaSetNotFound, status);
    ASSERT_TRUE(responseBuilder.obj().isEmpty());
}

TEST_F(TopoCoordTest, NodeReturnsFresherWhenFreshnessIsCheckedWithStaleConfigVersion) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    // Test with old config version
    args.setName = "rs0";
    args.cfgver = 5;
    args.id = 20;
    args.who = HostAndPort("h1");
    args.opTime = ourOpTime.getTimestamp();

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_OK(status);
    BSONObj response = responseBuilder.obj();
    ASSERT_EQUALS("config version stale", response["info"].String());
    ASSERT_EQUALS(ourOpTime.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_TRUE(response["fresher"].Bool());
    ASSERT_FALSE(response["veto"].Bool());
    ASSERT_FALSE(response.hasField("errmsg"));
}

TEST_F(TopoCoordTest, VetoWhenFreshnessIsCheckedWithAMemberWhoIsNotInTheConfig) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    // Test with non-existent node.
    args.setName = "rs0";
    args.who = HostAndPort("fakenode");
    args.opTime = ourOpTime.getTimestamp();
    args.cfgver = 10;
    args.id = 0;

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_OK(status);
    BSONObj response = responseBuilder.obj();
    ASSERT_EQUALS(ourOpTime.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].Bool());
    ASSERT_TRUE(response["veto"].Bool());
    ASSERT_EQUALS("replSet couldn't find member with id 0", response["errmsg"].String());
}

TEST_F(TopoCoordTest, VetoWhenFreshnessIsCheckedWhilePrimary) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);


    // Test when we are primary.
    args.setName = "rs0";
    args.opTime = ourOpTime.getTimestamp();
    args.cfgver = 10;
    args.id = 20;
    args.who = HostAndPort("h1");

    makeSelfPrimary();

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_OK(status);
    BSONObj response = responseBuilder.obj();
    ASSERT_FALSE(response.hasField("info"));
    ASSERT_EQUALS(ourOpTime.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].Bool());
    ASSERT_TRUE(response["veto"].Bool());
    ASSERT_EQUALS("I am already primary, h1:27017 can try again once I've stepped down",
                  response["errmsg"].String());
}

TEST_F(TopoCoordTest, VetoWhenFreshnessIsCheckedWhilePrimaryExists) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    args.setName = "rs0";
    args.opTime = ourOpTime.getTimestamp();
    args.cfgver = 10;
    args.id = 20;
    args.who = HostAndPort("h1");

    // Test when someone else is primary.
    heartbeatFromMember(HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, ourOpTime);
    setSelfMemberState(MemberState::RS_SECONDARY);
    getTopoCoord()._setCurrentPrimaryForTest(2);

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_OK(status);
    BSONObj response = responseBuilder.obj();
    ASSERT_FALSE(response.hasField("info"));
    ASSERT_EQUALS(ourOpTime.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].Bool());
    ASSERT_TRUE(response["veto"].Bool());
    ASSERT_EQUALS(
        "h1:27017 is trying to elect itself but h2:27017 is already primary and more "
        "up-to-date",
        response["errmsg"].String());
}

TEST_F(TopoCoordTest, NodeReturnsNotFreshestWhenFreshnessIsCheckedByALowPriorityNode) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    args.setName = "rs0";
    args.opTime = ourOpTime.getTimestamp();
    args.cfgver = 10;
    args.id = 20;
    args.who = HostAndPort("h1");

    // Test trying to elect a node that is caught up but isn't the highest priority node.
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);
    heartbeatFromMember(HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, staleOpTime);
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_OK(status);
    BSONObj response = responseBuilder.obj();
    ASSERT_FALSE(response.hasField("info"));
    ASSERT_EQUALS(ourOpTime.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].Bool());
    ASSERT_TRUE(response["veto"].Bool());
    ASSERT(response["errmsg"].String().find("h1:27017 has lower priority of 1 than") !=
           std::string::npos)
        << response["errmsg"].String();
}

TEST_F(TopoCoordTest, VetoWhenFreshnessIsCheckedByANodeWeBelieveToBeDown) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    args.setName = "rs0";
    args.opTime = ourOpTime.getTimestamp();
    args.cfgver = 10;
    args.id = 40;
    args.who = HostAndPort("h3");

    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);
    heartbeatFromMember(HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, staleOpTime);
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    receiveDownHeartbeat(HostAndPort("h3"), "rs0", OpTime());

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_OK(status);
    BSONObj response = responseBuilder.obj();
    ASSERT_FALSE(response.hasField("info"));
    ASSERT_EQUALS(ourOpTime.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].Bool());
    ASSERT_TRUE(response["veto"].Bool());
    ASSERT_NE(std::string::npos,
              response["errmsg"].String().find(
                  "I don't think h3:27017 is electable because the member is not "
                  "currently a secondary"))
        << response["errmsg"].String();
}

TEST_F(TopoCoordTest, VetoWhenFreshnessIsCheckedByANodeThatIsPrimary) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    args.setName = "rs0";
    args.opTime = ourOpTime.getTimestamp();
    args.cfgver = 10;
    args.id = 40;
    args.who = HostAndPort("h3");

    // Test trying to elect a node that isn't electable because it's PRIMARY
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_PRIMARY, ourOpTime);
    ASSERT_EQUALS(3, getCurrentPrimaryIndex());

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_OK(status);
    BSONObj response = responseBuilder.obj();
    ASSERT_FALSE(response.hasField("info"));
    ASSERT_EQUALS(ourOpTime.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].Bool());
    ASSERT_TRUE(response["veto"].Bool());
    ASSERT_NE(
        std::string::npos,
        response["errmsg"].String().find(
            "I don't think h3:27017 is electable because the member is not currently a secondary"))
        << response["errmsg"].String();
}

TEST_F(TopoCoordTest, VetoWhenFreshnessIsCheckedByANodeThatIsInStartup) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    args.setName = "rs0";
    args.opTime = ourOpTime.getTimestamp();
    args.cfgver = 10;
    args.id = 40;
    args.who = HostAndPort("h3");

    // Test trying to elect a node that isn't electable because it's STARTUP
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_STARTUP, ourOpTime);

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_OK(status);
    BSONObj response = responseBuilder.obj();
    ASSERT_FALSE(response.hasField("info"));
    ASSERT_EQUALS(ourOpTime.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].Bool());
    ASSERT_TRUE(response["veto"].Bool());
    ASSERT_NE(std::string::npos,
              response["errmsg"].String().find(
                  "I don't think h3:27017 is electable because the member is not "
                  "currently a secondary"))
        << response["errmsg"].String();
}

TEST_F(TopoCoordTest, VetoWhenFreshnessIsCheckedByANodeThatIsRecovering) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    args.setName = "rs0";
    args.opTime = ourOpTime.getTimestamp();
    args.cfgver = 10;
    args.id = 40;
    args.who = HostAndPort("h3");

    // Test trying to elect a node that isn't electable because it's RECOVERING
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_RECOVERING, ourOpTime);

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_OK(status);
    BSONObj response = responseBuilder.obj();
    ASSERT_FALSE(response.hasField("info"));
    ASSERT_EQUALS(ourOpTime.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].Bool());
    ASSERT_TRUE(response["veto"].Bool());
    ASSERT_NE(std::string::npos,
              response["errmsg"].String().find(
                  "I don't think h3:27017 is electable because the member is not "
                  "currently a secondary"))
        << response["errmsg"].String();
}

TEST_F(TopoCoordTest,
       RespondPositivelyWhenFreshnessIsCheckedByAFresherAndLowerPriorityNodeThanExistingPrimary) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    // Test trying to elect a node that is fresher but lower priority than the existing primary
    args.setName = "rs0";
    args.opTime = ourOpTime.getTimestamp();
    args.cfgver = 10;
    args.id = 30;
    args.who = HostAndPort("h2");

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_PRIMARY, ourOpTime);
    ASSERT_EQUALS(3, getCurrentPrimaryIndex());
    heartbeatFromMember(HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, freshestOpTime);

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_OK(status);
    BSONObj response = responseBuilder.obj();
    ASSERT_FALSE(response.hasField("info"));
    ASSERT_EQUALS(ourOpTime.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_TRUE(response["fresher"].Bool());
    ASSERT_FALSE(response["veto"].Bool());
    ASSERT_FALSE(response.hasField("errmsg"));
}

TEST_F(TopoCoordTest, RespondPositivelyWhenFreshnessIsCheckedByAnElectableNode) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);


    // Test trying to elect a valid node
    args.setName = "rs0";
    args.opTime = ourOpTime.getTimestamp();
    args.cfgver = 10;
    args.id = 40;
    args.who = HostAndPort("h3");

    receiveDownHeartbeat(HostAndPort("h2"), "rs0", OpTime());
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_OK(status);
    BSONObj response = responseBuilder.obj();
    ASSERT_FALSE(response.hasField("info")) << response.toString();
    ASSERT_EQUALS(ourOpTime.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].Bool()) << response.toString();
    ASSERT_FALSE(response["veto"].Bool()) << response.toString();
    ASSERT_FALSE(response.hasField("errmsg")) << response.toString();
}

TEST_F(TopoCoordTest, NodeReturnsBadValueWhenFreshnessIsCheckedByANodeWithOurID) {
    ReplicationCoordinator::ReplSetFreshArgs args;
    OpTime freshestOpTime(Timestamp(15, 10), 0);
    OpTime ourOpTime(Timestamp(10, 10), 0);
    OpTime staleOpTime(Timestamp(1, 1), 0);
    Status internalErrorStatus(ErrorCodes::InternalError, "didn't set status");

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 10
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 10 << "host"
                                               << "hself"
                                               << "priority"
                                               << 10)
                                    << BSON("_id" << 20 << "host"
                                                  << "h1")
                                    << BSON("_id" << 30 << "host"
                                                  << "h2")
                                    << BSON("_id" << 40 << "host"
                                                  << "h3"
                                                  << "priority"
                                                  << 10))),
                 0);
    heartbeatFromMember(HostAndPort("h1"), "rs0", MemberState::RS_SECONDARY, ourOpTime);

    // Test with our id
    args.setName = "rs0";
    args.opTime = ourOpTime.getTimestamp();
    args.cfgver = 10;
    args.who = HostAndPort("h3");
    args.id = 10;

    BSONObjBuilder responseBuilder;
    Status status = internalErrorStatus;
    getTopoCoord().prepareFreshResponse(args, Date_t(), ourOpTime, &responseBuilder, &status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_EQUALS(
        "Received replSetFresh command from member with the same member ID as ourself: 10",
        status.reason());
    ASSERT_TRUE(responseBuilder.obj().isEmpty());
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

class HeartbeatResponseTest : public TopoCoordTest {
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
                          << "settings"
                          << BSON("heartbeatTimeoutSecs" << 5)),
                     0);
    }
};

class HeartbeatResponseTestOneRetry : public HeartbeatResponseTest {
public:
    virtual void setUp() {
        HeartbeatResponseTest::setUp();

        // Bring up the node we are heartbeating.
        _target = HostAndPort("host2", 27017);
        Date_t _upRequestDate = unittest::assertGet(dateFromISOString("2014-08-29T12:55Z"));
        std::pair<ReplSetHeartbeatArgs, Milliseconds> uppingRequest =
            getTopoCoord().prepareHeartbeatRequest(_upRequestDate, "rs0", _target);
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
        std::pair<ReplSetHeartbeatArgs, Milliseconds> request =
            getTopoCoord().prepareHeartbeatRequest(_firstRequestDate, "rs0", _target);
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
        request = getTopoCoord().prepareHeartbeatRequest(
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

class HeartbeatResponseTestTwoRetries : public HeartbeatResponseTestOneRetry {
public:
    virtual void setUp() {
        HeartbeatResponseTestOneRetry::setUp();
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
        std::pair<ReplSetHeartbeatArgs, Milliseconds> request =
            getTopoCoord().prepareHeartbeatRequest(
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

class HeartbeatResponseHighVerbosityTest : public HeartbeatResponseTest {
public:
    virtual void setUp() {
        HeartbeatResponseTest::setUp();
        // set verbosity as high as the highest verbosity log message we'd like to check for
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(3));
    }

    virtual void tearDown() {
        HeartbeatResponseTest::tearDown();
        logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Log());
    }
};

TEST_F(HeartbeatResponseHighVerbosityTest,
       LogMessageAndTakeNoActionWhenReceivingAHeartbeatResponseFromANodeThatBelievesWeAreDown) {
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    // request heartbeat
    std::pair<ReplSetHeartbeatArgs, Milliseconds> request =
        getTopoCoord().prepareHeartbeatRequest(now()++, "rs0", HostAndPort("host2"));

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

TEST_F(HeartbeatResponseHighVerbosityTest,
       LogMessageAndTakeNoActionWhenReceivingAHeartbeatResponseFromANodeThatIsNotInConfig) {
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    // request heartbeat
    std::pair<ReplSetHeartbeatArgs, Milliseconds> request =
        getTopoCoord().prepareHeartbeatRequest(now()++, "rs0", HostAndPort("host5"));

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

// TODO(dannenberg) figure out why this test is useful
TEST_F(HeartbeatResponseHighVerbosityTest, UpdateHeartbeatDataSameConfig) {
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    // request heartbeat
    std::pair<ReplSetHeartbeatArgs, Milliseconds> request =
        getTopoCoord().prepareHeartbeatRequest(now()++, "rs0", HostAndPort("host2"));

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
    ASSERT_EQUALS(1, countLogLinesContaining("Config from heartbeat response was same as ours."));
}

// TODO(dannenberg) change the name and functionality of this to match what this claims it is
TEST_F(HeartbeatResponseHighVerbosityTest, UpdateHeartbeatDataOldConfig) {
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    // request heartbeat
    std::pair<ReplSetHeartbeatArgs, Milliseconds> request =
        getTopoCoord().prepareHeartbeatRequest(now()++, "rs0", HostAndPort("host2"));

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

TEST_F(HeartbeatResponseTestOneRetry, ReconfigWhenHeartbeatResponseContainsAConfig) {
    // Confirm that action responses can come back from retries; in this, expect a Reconfig
    // action.
    ReplicaSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version"
                                        << 7
                                        << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "host1:27017")
                                                      << BSON("_id" << 1 << "host"
                                                                    << "host2:27017")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "host3:27017")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "host4:27017"))
                                        << "settings"
                                        << BSON("heartbeatTimeoutSecs" << 5))));
    ASSERT_OK(newConfig.validate());

    ReplSetHeartbeatResponse reconfigResponse;
    reconfigResponse.noteReplSet();
    reconfigResponse.setSetName("rs0");
    reconfigResponse.setState(MemberState::RS_SECONDARY);
    reconfigResponse.setElectable(true);
    reconfigResponse.setConfigVersion(1);
    reconfigResponse.setConfig(newConfig);
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(4500),  // Time is left.
        Milliseconds(400),                        // Spent 0.4 of the 0.5 second in the network.
        target(),
        StatusWith<ReplSetHeartbeatResponse>(reconfigResponse),
        OpTime(Timestamp(0, 0), 0));  // We've never applied anything.
    ASSERT_EQUALS(HeartbeatResponseAction::Reconfig, action.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(6500), action.getNextHeartbeatStartDate());
}

TEST_F(HeartbeatResponseTestOneRetry, StepDownRemotePrimaryWhenWeWereElectedMoreRecently) {
    // Confirm that action responses can come back from retries; in this, expect a
    // StepDownRemotePrimary action.

    // make self primary
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    makeSelfPrimary(Timestamp(5, 0));
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    ReplSetHeartbeatResponse electedMoreRecentlyResponse;
    electedMoreRecentlyResponse.noteReplSet();
    electedMoreRecentlyResponse.setSetName("rs0");
    electedMoreRecentlyResponse.setState(MemberState::RS_PRIMARY);
    electedMoreRecentlyResponse.setElectable(true);
    electedMoreRecentlyResponse.setElectionTime(Timestamp(3, 0));
    electedMoreRecentlyResponse.setConfigVersion(5);
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(4500),  // Time is left.
        Milliseconds(400),                        // Spent 0.4 of the 0.5 second in the network.
        target(),
        StatusWith<ReplSetHeartbeatResponse>(electedMoreRecentlyResponse),
        OpTime());  // We've never applied anything.
    ASSERT_EQUALS(HeartbeatResponseAction::StepDownRemotePrimary, action.getAction());
    ASSERT_EQUALS(1, action.getPrimaryConfigIndex());
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(6500), action.getNextHeartbeatStartDate());
}

TEST_F(HeartbeatResponseTestOneRetry, StepDownSelfWhenRemoteNodeWasElectedMoreRecently) {
    // Confirm that action responses can come back from retries; in this, expect a StepDownSelf
    // action.

    // acknowledge the other member so that we see a majority
    HeartbeatResponseAction action =
        receiveDownHeartbeat(HostAndPort("host3"), "rs0", OpTime(Timestamp(100, 0), 0));
    ASSERT_NO_ACTION(action.getAction());

    // make us PRIMARY
    makeSelfPrimary();

    ReplSetHeartbeatResponse electedMoreRecentlyResponse;
    electedMoreRecentlyResponse.noteReplSet();
    electedMoreRecentlyResponse.setSetName("rs0");
    electedMoreRecentlyResponse.setState(MemberState::RS_PRIMARY);
    electedMoreRecentlyResponse.setElectable(false);
    electedMoreRecentlyResponse.setElectionTime(Timestamp(10, 0));
    electedMoreRecentlyResponse.setConfigVersion(5);
    action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(4500),  // Time is left.
        Milliseconds(400),                        // Spent 0.4 of the 0.5 second in the network.
        target(),
        StatusWith<ReplSetHeartbeatResponse>(electedMoreRecentlyResponse),
        OpTime(Timestamp(0, 0), 0));  // We've never applied anything.
    ASSERT_EQUALS(HeartbeatResponseAction::StepDownSelf, action.getAction());
    ASSERT_EQUALS(0, action.getPrimaryConfigIndex());
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(6500), action.getNextHeartbeatStartDate());
    // Doesn't actually do the stepdown until stepDownIfPending is called
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    ASSERT_TRUE(getTopoCoord().stepDownIfPending());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
}

TEST_F(HeartbeatResponseTestOneRetry,
       StartElectionAfterReceivingAHeartbeatWhileNoPrimaryExistsAndWeAreElectable) {
    // Confirm that action responses can come back from retries; in this, expect a StartElection
    // action.

    // acknowledge the other member so that we see a majority
    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(300, 0), 0);
    HeartbeatResponseAction action = receiveUpHeartbeat(HostAndPort("host3"),
                                                        "rs0",
                                                        MemberState::RS_SECONDARY,
                                                        election,
                                                        election,
                                                        lastOpTimeApplied);
    ASSERT_NO_ACTION(action.getAction());

    // make sure we are electable
    setSelfMemberState(MemberState::RS_SECONDARY);

    ReplSetHeartbeatResponse startElectionResponse;
    startElectionResponse.noteReplSet();
    startElectionResponse.setSetName("rs0");
    startElectionResponse.setState(MemberState::RS_SECONDARY);
    startElectionResponse.setElectable(true);
    startElectionResponse.setConfigVersion(5);
    action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(4500),  // Time is left.
        Milliseconds(400),                        // Spent 0.4 of the 0.5 second in the network.
        target(),
        StatusWith<ReplSetHeartbeatResponse>(startElectionResponse),
        election);
    ASSERT_EQUALS(HeartbeatResponseAction::StartElection, action.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(6500), action.getNextHeartbeatStartDate());
}

TEST_F(HeartbeatResponseTestTwoRetries, NodeDoesNotRetryHeartbeatsAfterFailingTwiceInARow) {
    // Confirm that the topology coordinator attempts to retry a failed heartbeat two times
    // after initial failure, assuming that the heartbeat timeout (set to 5 seconds in the
    // fixture) has not expired.
    //
    // Failed heartbeats propose taking no action, other than scheduling the next heartbeat.  We
    // can detect a retry vs the next regularly scheduled heartbeat because retries are
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
    // Because this is the second retry, rather than retry again, we expect to wait for the
    // heartbeat interval of 2 seconds to elapse.
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(6800), action.getNextHeartbeatStartDate());

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

TEST_F(HeartbeatResponseTestTwoRetries, ReconfigWhenHeartbeatResponseContainsAConfig) {
    // Confirm that action responses can come back from retries; in this, expect a Reconfig
    // action.
    ReplicaSetConfig newConfig;
    ASSERT_OK(newConfig.initialize(BSON("_id"
                                        << "rs0"
                                        << "version"
                                        << 7
                                        << "members"
                                        << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                                 << "host1:27017")
                                                      << BSON("_id" << 1 << "host"
                                                                    << "host2:27017")
                                                      << BSON("_id" << 2 << "host"
                                                                    << "host3:27017")
                                                      << BSON("_id" << 3 << "host"
                                                                    << "host4:27017"))
                                        << "settings"
                                        << BSON("heartbeatTimeoutSecs" << 5))));
    ASSERT_OK(newConfig.validate());

    ReplSetHeartbeatResponse reconfigResponse;
    reconfigResponse.noteReplSet();
    reconfigResponse.setSetName("rs0");
    reconfigResponse.setState(MemberState::RS_SECONDARY);
    reconfigResponse.setElectable(true);
    reconfigResponse.setConfigVersion(1);
    reconfigResponse.setConfig(newConfig);
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(4500),  // Time is left.
        Milliseconds(400),                        // Spent 0.4 of the 0.5 second in the network.
        target(),
        StatusWith<ReplSetHeartbeatResponse>(reconfigResponse),
        OpTime(Timestamp(0, 0), 0));  // We've never applied anything.
    ASSERT_EQUALS(HeartbeatResponseAction::Reconfig, action.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(6500), action.getNextHeartbeatStartDate());
}

TEST_F(HeartbeatResponseTestTwoRetries, StepDownRemotePrimaryWhenWeWereElectedMoreRecently) {
    // Confirm that action responses can come back from retries; in this, expect a
    // StepDownRemotePrimary action.

    // make self primary
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    makeSelfPrimary(Timestamp(5, 0));
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    ReplSetHeartbeatResponse electedMoreRecentlyResponse;
    electedMoreRecentlyResponse.noteReplSet();
    electedMoreRecentlyResponse.setSetName("rs0");
    electedMoreRecentlyResponse.setState(MemberState::RS_PRIMARY);
    electedMoreRecentlyResponse.setElectable(true);
    electedMoreRecentlyResponse.setElectionTime(Timestamp(3, 0));
    electedMoreRecentlyResponse.setConfigVersion(5);
    HeartbeatResponseAction action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(5000),  // Time is left.
        Milliseconds(400),                        // Spent 0.4 of the 0.5 second in the network.
        target(),
        StatusWith<ReplSetHeartbeatResponse>(electedMoreRecentlyResponse),
        OpTime());  // We've never applied anything.
    ASSERT_EQUALS(HeartbeatResponseAction::StepDownRemotePrimary, action.getAction());
    ASSERT_EQUALS(1, action.getPrimaryConfigIndex());
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(7000), action.getNextHeartbeatStartDate());
}

TEST_F(HeartbeatResponseTestTwoRetries, StepDownSelfWhenRemoteNodeWasElectedMoreRecently) {
    // Confirm that action responses can come back from retries; in this, expect a StepDownSelf
    // action.

    // acknowledge the other member so that we see a majority
    HeartbeatResponseAction action =
        receiveDownHeartbeat(HostAndPort("host3"), "rs0", OpTime(Timestamp(100, 0), 0));
    ASSERT_NO_ACTION(action.getAction());

    // make us PRIMARY
    makeSelfPrimary();

    ReplSetHeartbeatResponse electedMoreRecentlyResponse;
    electedMoreRecentlyResponse.noteReplSet();
    electedMoreRecentlyResponse.setSetName("rs0");
    electedMoreRecentlyResponse.setState(MemberState::RS_PRIMARY);
    electedMoreRecentlyResponse.setElectable(false);
    electedMoreRecentlyResponse.setElectionTime(Timestamp(10, 0));
    electedMoreRecentlyResponse.setConfigVersion(5);
    action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(5000),  // Time is left.
        Milliseconds(400),                        // Spent 0.4 of the 0.5 second in the network.
        target(),
        StatusWith<ReplSetHeartbeatResponse>(electedMoreRecentlyResponse),
        OpTime(Timestamp(0, 0), 0));  // We've never applied anything.
    ASSERT_EQUALS(HeartbeatResponseAction::StepDownSelf, action.getAction());
    ASSERT_EQUALS(0, action.getPrimaryConfigIndex());
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(7000), action.getNextHeartbeatStartDate());
    // Doesn't actually do the stepdown until stepDownIfPending is called
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    ASSERT_TRUE(getTopoCoord().stepDownIfPending());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
}

TEST_F(HeartbeatResponseTestTwoRetries,
       StartElectionAfterReceivingAHeartbeatWhileNoPrimaryExistsAndWeAreElectable) {
    // Confirm that action responses can come back from retries; in this, expect a StartElection
    // action.

    // acknowledge the other member so that we see a majority
    OpTime election = OpTime(Timestamp(400, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(300, 0), 0);
    HeartbeatResponseAction action = receiveUpHeartbeat(HostAndPort("host3"),
                                                        "rs0",
                                                        MemberState::RS_SECONDARY,
                                                        election,
                                                        election,
                                                        lastOpTimeApplied);
    ASSERT_NO_ACTION(action.getAction());

    // make sure we are electable
    setSelfMemberState(MemberState::RS_SECONDARY);

    ReplSetHeartbeatResponse startElectionResponse;
    startElectionResponse.noteReplSet();
    startElectionResponse.setSetName("rs0");
    startElectionResponse.setState(MemberState::RS_SECONDARY);
    startElectionResponse.setElectable(true);
    startElectionResponse.setConfigVersion(5);
    action = getTopoCoord().processHeartbeatResponse(
        firstRequestDate() + Milliseconds(5000),  // Time is left.
        Milliseconds(400),                        // Spent 0.4 of the 0.5 second in the network.
        target(),
        StatusWith<ReplSetHeartbeatResponse>(startElectionResponse),
        election);
    ASSERT_EQUALS(HeartbeatResponseAction::StartElection, action.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(7000), action.getNextHeartbeatStartDate());
}

TEST_F(HeartbeatResponseTest, NodeDoesNotRetryHeartbeatIfTheFirstFailureTakesTheFullTime) {
    // Confirm that the topology coordinator does not schedule an immediate heartbeat retry if
    // the heartbeat timeout period expired before the initial request completed.

    HostAndPort target("host2", 27017);
    Date_t firstRequestDate = unittest::assertGet(dateFromISOString("2014-08-29T13:00Z"));

    // Initial heartbeat request prepared, at t + 0.
    std::pair<ReplSetHeartbeatArgs, Milliseconds> request =
        getTopoCoord().prepareHeartbeatRequest(firstRequestDate, "rs0", target);
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
    // Because the heartbeat timed out, we'll retry in 2 seconds.
    ASSERT_EQUALS(firstRequestDate + Milliseconds(7000), action.getNextHeartbeatStartDate());
}

TEST_F(HeartbeatResponseTestOneRetry,
       NodeDoesNotRetryHeartbeatIfTheFirstAndSecondFailuresExhaustTheFullTime) {
    // Confirm that the topology coordinator does not schedule a second heartbeat retry if
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
    // Because the heartbeat timed out, we'll retry in 2 seconds.
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(7010), action.getNextHeartbeatStartDate());
}

TEST_F(HeartbeatResponseTestTwoRetries,
       NodeDoesNotMarkANodeAsDownAfterThreeNonConsecutiveFailedHeartbeats) {
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
    // Because the heartbeat succeeded, we'll retry in 2 seconds.
    ASSERT_EQUALS(firstRequestDate() + Milliseconds(6500), action.getNextHeartbeatStartDate());

    // request next heartbeat
    getTopoCoord().prepareHeartbeatRequest(
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

TEST_F(HeartbeatResponseTest, UpdatePrimaryIndexWhenAHeartbeatMakesNodeAwareOfANewPrimary) {
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

TEST_F(HeartbeatResponseTest,
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
    // second primary does not change primary index
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTest,
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
    // second primary does not change primary index
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTest,
       StepDownRemotePrimaryWhenAHeartbeatMakesNodeAwareOfAnOlderPrimaryWhilePrimary) {
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
    ASSERT_EQUALS(HeartbeatResponseAction::StepDownRemotePrimary, nextAction.getAction());
    ASSERT_EQUALS(1, nextAction.getPrimaryConfigIndex());
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
}

// TODO(dannenberg) figure out what this is about...
TEST_F(HeartbeatResponseTest, UpdateHeartbeatDataStepDownPrimaryForHighPriorityFreshNode) {
    // In this test, the Topology coordinator sees a PRIMARY ("host2") and then sees a higher
    // priority and similarly fresh node ("host3"). However, since the coordinator's node
    // (host1) is not the higher priority node, it takes no action.
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

TEST_F(
    HeartbeatResponseTest,
    StepDownSelfButRemainElectableWhenHeartbeatResponseContainsAnEquallyFreshHigherPriorityNode) {
    // In this test, the Topology coordinator becomes PRIMARY and then sees a higher priority
    // and equally fresh node ("host3"). As a result it responds with a StepDownSelf action.
    //
    // Despite having stepped down, we should remain electable, in order to dissuade lower
    // priority nodes from standing for election.
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
    ASSERT_EQUALS(HeartbeatResponseAction::StepDownSelf, nextAction.getAction());
    ASSERT_EQUALS(0, nextAction.getPrimaryConfigIndex());

    // Process a heartbeat response to confirm that this node, which is no longer primary,
    // still tells other nodes that it is electable.  This will stop lower priority nodes
    // from standing for election.
    ReplSetHeartbeatArgs hbArgs;
    hbArgs.setSetName("rs0");
    hbArgs.setProtocolVersion(1);
    hbArgs.setConfigVersion(6);
    hbArgs.setSenderId(1);
    hbArgs.setSenderHost(HostAndPort("host3", 27017));
    ReplSetHeartbeatResponse hbResp;
    ASSERT_OK(
        getTopoCoord().prepareHeartbeatResponse(now(), hbArgs, "rs0", election, election, &hbResp));
    ASSERT(!hbResp.hasIsElectable() || hbResp.isElectable()) << hbResp.toString();
}

TEST_F(HeartbeatResponseTest,
       NodeDoesNotStepDownSelfWhenHeartbeatResponseContainsALessFreshHigherPriorityNode) {
    // In this test, the Topology coordinator becomes PRIMARY and then sees a higher priority
    // and stale node ("host3"). As a result it responds with NoAction.
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

TEST_F(HeartbeatResponseTest,
       NodeDoesNotStepDownRemoteWhenHeartbeatResponseContainsALessFreshHigherPriorityNode) {
    // In this test, the Topology coordinator sees a PRIMARY ("host2") and then sees a higher
    // priority and stale node ("host3"). As a result it responds with NoAction.
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

TEST_F(HeartbeatResponseTest, StepDownSelfWhenRemoteNodeWasElectedMoreRecently) {
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    makeSelfPrimary(Timestamp(2, 0));

    OpTime election = OpTime(Timestamp(4, 0), 0);
    OpTime lastOpTimeApplied = OpTime(Timestamp(3, 0), 0);

    ASSERT_EQUALS(0, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            election,
                                                            lastOpTimeApplied);
    ASSERT_EQUALS(HeartbeatResponseAction::StepDownSelf, nextAction.getAction());
    ASSERT_EQUALS(0, nextAction.getPrimaryConfigIndex());
    // Doesn't actually do the stepdown until stepDownIfPending is called
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    ASSERT_TRUE(getTopoCoord().stepDownIfPending());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());
}

TEST_F(HeartbeatResponseTest,
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

TEST_F(HeartbeatResponseTest,
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
                                                  << "host3:27017"))),
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

TEST_F(HeartbeatResponseTest,
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

TEST_F(HeartbeatResponseTest,
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

TEST_F(HeartbeatResponseTest,
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

    // freeze node to set stepdown wait
    BSONObjBuilder response;
    getTopoCoord().prepareFreezeResponse(now()++, 20, &response);

    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTest,
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
                                                  << "host3:27017"))),
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

TEST_F(HeartbeatResponseTest, StartElectionWhenPrimaryIsMarkedDownAndWeAreElectable) {
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
    ASSERT_EQUALS(HeartbeatResponseAction::StartElection, nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTest, NodeDoesNotStartElectionWhileAlreadyCandidate) {
    // In this test, the TopologyCoordinator goes through the steps of a successful election,
    // during which it receives a heartbeat that would normally trigger it to become a candidate
    // and respond with a StartElection HeartbeatResponseAction. However, since it is already in
    // candidate state, it responds with a NoAction HeartbeatResponseAction. Then finishes by
    // being winning the election.

    // 1. All nodes heartbeat to indicate that they are up and that "host2" is PRIMARY.
    // 2. "host2" goes down, triggering an election.
    // 3. "host2" comes back, which would normally trigger election, but since the
    //     TopologyCoordinator is already in candidate mode, does not.
    // 4. TopologyCoordinator concludes its freshness round successfully and wins the election.

    setSelfMemberState(MemberState::RS_SECONDARY);
    now() += Seconds(30);  // we need to be more than LastVote::leaseTime from the start of time or
                           // else some Date_t math goes horribly awry

    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(130, 0), 0);
    OID round = OID::gen();

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    lastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // candidate time!
    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_EQUALS(HeartbeatResponseAction::StartElection, nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    // see the downed node as SECONDARY and decide to take no action, but are still a candidate
    nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    lastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());

    // normally this would trigger StartElection, but we are already a candidate
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    // now voteForSelf as though we received all our fresh responses
    ASSERT_TRUE(getTopoCoord().voteForMyself(now()++));

    // now win election and ensure _electionId and _electionTime are set properly
    getTopoCoord().processWinElection(round, election.getTimestamp());
    ASSERT_EQUALS(round, getTopoCoord().getElectionId());
    ASSERT_EQUALS(election.getTimestamp(), getTopoCoord().getElectionTime());
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());
}

TEST_F(HeartbeatResponseTest, LoseElectionWhenVotingForAnotherNodeWhileRunningTheFreshnessChecker) {
    // In this test, the TopologyCoordinator goes through the steps of an election. However,
    // before its freshness round ends, it receives a fresh command followed by an elect command
    // from another node, both of which it responds positively to. The TopologyCoordinator's
    // freshness round then concludes successfully, but it fails to vote for itself, since it
    // recently voted for another node.

    // 1. All nodes heartbeat to indicate that they are up and that "host2" is PRIMARY.
    // 2. "host2" goes down, triggering an election.
    // 3. "host3" sends a fresh command, which the TopologyCoordinator responds to positively.
    // 4. "host3" sends an elect command, which the TopologyCoordinator responds to positively.
    // 5. The TopologyCoordinator's concludes its freshness round successfully.
    // 6. The TopologyCoordinator loses the election.

    setSelfMemberState(MemberState::RS_SECONDARY);
    now() += Seconds(30);  // we need to be more than LastVote::leaseTime from the start of time or
                           // else some Date_t math goes horribly awry

    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(100, 0), 0);
    OpTime fresherOpApplied = OpTime(Timestamp(200, 0), 0);

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    lastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // candidate time!
    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_EQUALS(HeartbeatResponseAction::StartElection, nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    Timestamp originalElectionTime = getTopoCoord().getElectionTime();
    OID originalElectionId = getTopoCoord().getElectionId();
    // prepare an incoming fresh command
    ReplicationCoordinator::ReplSetFreshArgs freshArgs;
    freshArgs.setName = "rs0";
    freshArgs.cfgver = 5;
    freshArgs.id = 2;
    freshArgs.who = HostAndPort("host3");
    freshArgs.opTime = fresherOpApplied.getTimestamp();

    BSONObjBuilder freshResponseBuilder;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    getTopoCoord().prepareFreshResponse(
        freshArgs, now()++, lastOpTimeApplied, &freshResponseBuilder, &result);
    BSONObj response = freshResponseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(lastOpTimeApplied.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].trueValue());
    ASSERT_FALSE(response["veto"].trueValue());
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    // make sure incoming fresh commands do not change electionTime and electionId
    ASSERT_EQUALS(originalElectionTime, getTopoCoord().getElectionTime());
    ASSERT_EQUALS(originalElectionId, getTopoCoord().getElectionId());

    // an elect command comes in
    ReplicationCoordinator::ReplSetElectArgs electArgs;
    OID round = OID::gen();
    electArgs.set = "rs0";
    electArgs.round = round;
    electArgs.cfgver = 5;
    electArgs.whoid = 2;

    BSONObjBuilder electResponseBuilder;
    result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        electArgs, now()++, OpTime(), &electResponseBuilder, &result);
    stopCapturingLogMessages();
    response = electResponseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(1, response["vote"].Int());
    ASSERT_EQUALS(round, response["round"].OID());
    ASSERT_EQUALS(1, countLogLinesContaining("voting yea for host3:27017 (2)"));
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
    // make sure incoming elect commands do not change electionTime and electionId
    ASSERT_EQUALS(originalElectionTime, getTopoCoord().getElectionTime());
    ASSERT_EQUALS(originalElectionId, getTopoCoord().getElectionId());

    // now voteForSelf as though we received all our fresh responses
    ASSERT_FALSE(getTopoCoord().voteForMyself(now()++));

    // receive a heartbeat indicating the other node was elected
    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_PRIMARY,
                                    election,
                                    lastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(2, getCurrentPrimaryIndex());
    // make sure seeing a new primary does not change electionTime and electionId
    ASSERT_EQUALS(originalElectionTime, getTopoCoord().getElectionTime());
    ASSERT_EQUALS(originalElectionId, getTopoCoord().getElectionId());

    // now lose election and ensure _electionTime and _electionId are 0'd out
    getTopoCoord().processLoseElection();
    ASSERT_EQUALS(OID(), getTopoCoord().getElectionId());
    ASSERT_EQUALS(Timestamp(), getTopoCoord().getElectionTime());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(2, getCurrentPrimaryIndex());
}

TEST_F(HeartbeatResponseTest,
       NodeDoesNotLoseElectionWhenRespondingToAFreshnessCheckWhileRunningItsOwnFreshnessCheck) {
    // In this test, the TopologyCoordinator goes through the steps of an election. However,
    // before its freshness round ends, the TopologyCoordinator receives a fresh command from
    // another node, which it responds positively to. Its freshness then ends successfully and
    // it wins the election. The other node's elect command then comes in and is responded to
    // negatively, maintaining the TopologyCoordinator's PRIMARY state.

    // 1. All nodes heartbeat to indicate that they are up and that "host2" is PRIMARY.
    // 2. "host2" goes down, triggering an election.
    // 3. "host3" sends a fresh command, which the TopologyCoordinator responds to positively.
    // 4. The TopologyCoordinator concludes its freshness round successfully and wins
    //    the election.
    // 5. "host3" sends an elect command, which the TopologyCoordinator responds to negatively.

    setSelfMemberState(MemberState::RS_SECONDARY);
    now() += Seconds(30);  // we need to be more than LastVote::leaseTime from the start of time or
                           // else some Date_t math goes horribly awry

    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(100, 0), 0);
    OpTime fresherLastOpTimeApplied = OpTime(Timestamp(200, 0), 0);
    OID round = OID::gen();
    OID remoteRound = OID::gen();

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    lastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // candidate time!
    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_EQUALS(HeartbeatResponseAction::StartElection, nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    // prepare an incoming fresh command
    ReplicationCoordinator::ReplSetFreshArgs freshArgs;
    freshArgs.setName = "rs0";
    freshArgs.cfgver = 5;
    freshArgs.id = 2;
    freshArgs.who = HostAndPort("host3");
    freshArgs.opTime = fresherLastOpTimeApplied.getTimestamp();

    BSONObjBuilder freshResponseBuilder;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    getTopoCoord().prepareFreshResponse(
        freshArgs, now()++, lastOpTimeApplied, &freshResponseBuilder, &result);
    BSONObj response = freshResponseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(lastOpTimeApplied.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].trueValue());
    ASSERT_FALSE(response["veto"].trueValue());
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    // now voteForSelf as though we received all our fresh responses
    ASSERT_TRUE(getTopoCoord().voteForMyself(now()++));
    // now win election and ensure _electionId and _electionTime are set properly
    getTopoCoord().processWinElection(round, election.getTimestamp());
    ASSERT_EQUALS(round, getTopoCoord().getElectionId());
    ASSERT_EQUALS(election.getTimestamp(), getTopoCoord().getElectionTime());
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    // an elect command comes in
    ReplicationCoordinator::ReplSetElectArgs electArgs;
    electArgs.set = "rs0";
    electArgs.round = remoteRound;
    electArgs.cfgver = 5;
    electArgs.whoid = 2;

    BSONObjBuilder electResponseBuilder;
    result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        electArgs, now()++, OpTime(), &electResponseBuilder, &result);
    stopCapturingLogMessages();
    response = electResponseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(-10000, response["vote"].Int());
    ASSERT_EQUALS(remoteRound, response["round"].OID());
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());
}

TEST_F(HeartbeatResponseTest,
       RespondPositivelyToFreshnessButNegativelyToElectCommandAfterBecomingPrimary) {
    // In this test, the TopologyCoordinator goes through the steps of an election. After
    // being successfully elected, a fresher node sends a fresh command, which the
    // TopologyCoordinator responds positively to. The fresher node then sends an elect command,
    // which the Topology coordinator negatively to since the TopologyCoordinator just elected
    // itself.

    // 1. All nodes heartbeat to indicate that they are up and that "host2" is PRIMARY.
    // 2. "host2" goes down, triggering an election.
    // 3. The TopologyCoordinator concludes its freshness round successfully and wins
    //    the election.
    // 4. "host3" sends a fresh command, which the TopologyCoordinator responds to positively.
    // 5. "host3" sends an elect command, which the TopologyCoordinator responds to negatively.

    setSelfMemberState(MemberState::RS_SECONDARY);
    now() += Seconds(30);  // we need to be more than LastVote::leaseTime from the start of time or
                           // else some Date_t math goes horribly awry

    OpTime election = OpTime();
    OpTime lastOpTimeApplied = OpTime(Timestamp(100, 0), 0);
    OpTime fresherLastOpTimeApplied = OpTime(Timestamp(200, 0), 0);
    OID round = OID::gen();
    OID remoteRound = OID::gen();

    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    HeartbeatResponseAction nextAction = receiveUpHeartbeat(HostAndPort("host2"),
                                                            "rs0",
                                                            MemberState::RS_PRIMARY,
                                                            election,
                                                            lastOpTimeApplied,
                                                            lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());
    ASSERT_EQUALS(1, getCurrentPrimaryIndex());

    nextAction = receiveUpHeartbeat(HostAndPort("host3"),
                                    "rs0",
                                    MemberState::RS_SECONDARY,
                                    election,
                                    lastOpTimeApplied,
                                    lastOpTimeApplied);
    ASSERT_NO_ACTION(nextAction.getAction());

    // candidate time!
    nextAction = receiveDownHeartbeat(HostAndPort("host2"), "rs0", lastOpTimeApplied);
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    ASSERT_EQUALS(HeartbeatResponseAction::StartElection, nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());

    // now voteForSelf as though we received all our fresh responses
    ASSERT_TRUE(getTopoCoord().voteForMyself(now()++));
    // now win election
    getTopoCoord().processWinElection(round, election.getTimestamp());
    ASSERT_EQUALS(0, getTopoCoord().getCurrentPrimaryIndex());
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());

    // prepare an incoming fresh command
    ReplicationCoordinator::ReplSetFreshArgs freshArgs;
    freshArgs.setName = "rs0";
    freshArgs.cfgver = 5;
    freshArgs.id = 2;
    freshArgs.who = HostAndPort("host3");
    freshArgs.opTime = fresherLastOpTimeApplied.getTimestamp();

    BSONObjBuilder freshResponseBuilder;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    getTopoCoord().prepareFreshResponse(
        freshArgs, now()++, lastOpTimeApplied, &freshResponseBuilder, &result);
    BSONObj response = freshResponseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(lastOpTimeApplied.getTimestamp(), Timestamp(response["opTime"].timestampValue()));
    ASSERT_FALSE(response["fresher"].trueValue());
    ASSERT_TRUE(response["veto"].trueValue()) << response["errmsg"];
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    // an elect command comes in
    ReplicationCoordinator::ReplSetElectArgs electArgs;
    electArgs.set = "rs0";
    electArgs.round = remoteRound;
    electArgs.cfgver = 5;
    electArgs.whoid = 2;

    BSONObjBuilder electResponseBuilder;
    result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        electArgs, now()++, OpTime(), &electResponseBuilder, &result);
    stopCapturingLogMessages();
    response = electResponseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(-10000, response["vote"].Int());
    ASSERT_EQUALS(remoteRound, response["round"].OID());
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());
}

TEST_F(HeartbeatResponseTest,
       StartElectionIfAMajorityOfVotersIsVisibleEvenThoughATrueMajorityIsNot) {
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

    // make sure all non-voting nodes are down, that way we do not have a majority of nodes
    // but do have a majority of votes since one of two voting members is up and so are we
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
    ASSERT_EQUALS(HeartbeatResponseAction::StartElection, nextAction.getAction());
    ASSERT_TRUE(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
}

TEST_F(HeartbeatResponseTest, RelinquishPrimaryWhenMajorityOfVotersIsNoLongerVisible) {
    // become PRIMARY
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
    makeSelfPrimary(Timestamp(2, 0));
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    // become aware of other nodes
    heartbeatFromMember(
        HostAndPort("host2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(1, 0), 0));
    heartbeatFromMember(
        HostAndPort("host2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(1, 0), 0));
    heartbeatFromMember(HostAndPort("host3"), "rs0", MemberState::RS_SECONDARY, OpTime());
    heartbeatFromMember(HostAndPort("host3"), "rs0", MemberState::RS_SECONDARY, OpTime());

    // lose that awareness and be sure we are going to stepdown
    HeartbeatResponseAction nextAction =
        receiveDownHeartbeat(HostAndPort("host2"), "rs0", OpTime(Timestamp(100, 0), 0));
    ASSERT_NO_ACTION(nextAction.getAction());
    nextAction = receiveDownHeartbeat(HostAndPort("host3"), "rs0", OpTime(Timestamp(100, 0), 0));
    ASSERT_EQUALS(HeartbeatResponseAction::StepDownSelf, nextAction.getAction());
    ASSERT_EQUALS(0, nextAction.getPrimaryConfigIndex());
    // Doesn't actually do the stepdown until stepDownIfPending is called
    ASSERT_TRUE(TopologyCoordinator::Role::leader == getTopoCoord().getRole());
    ASSERT_EQUALS(0, getCurrentPrimaryIndex());

    ASSERT_TRUE(getTopoCoord().stepDownIfPending());
    ASSERT_TRUE(TopologyCoordinator::Role::follower == getTopoCoord().getRole());
    ASSERT_EQUALS(-1, getCurrentPrimaryIndex());
}

class PrepareElectResponseTest : public TopoCoordTest {
public:
    PrepareElectResponseTest()
        : round(OID::gen()), cbData(NULL, ReplicationExecutor::CallbackHandle(), Status::OK()) {}

    virtual void setUp() {
        TopoCoordTest::setUp();
        updateConfig(BSON("_id"
                          << "rs0"
                          << "version"
                          << 10
                          << "members"
                          << BSON_ARRAY(BSON("_id" << 0 << "host"
                                                   << "hself")
                                        << BSON("_id" << 1 << "host"
                                                      << "h1")
                                        << BSON("_id" << 2 << "host"
                                                      << "h2"
                                                      << "priority"
                                                      << 10)
                                        << BSON("_id" << 3 << "host"
                                                      << "h3"
                                                      << "priority"
                                                      << 10))),
                     0);
    }

protected:
    Date_t now;
    OID round;
    ReplicationExecutor::CallbackArgs cbData;
};

TEST_F(PrepareElectResponseTest, RespondNegativelyWhenElectCommandHasTheWrongReplSetName) {
    // Test with incorrect replset name
    ReplicationCoordinator::ReplSetElectArgs args;
    args.set = "fakeset";
    args.round = round;
    args.cfgver = 10;
    args.whoid = 1;

    BSONObjBuilder responseBuilder;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        args, now += Seconds(60), OpTime(), &responseBuilder, &result);
    stopCapturingLogMessages();
    BSONObj response = responseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(0, response["vote"].Int());
    ASSERT_EQUALS(round, response["round"].OID());
    ASSERT_EQUALS(1,
                  countLogLinesContaining(
                      "received an elect request for 'fakeset' but our set name is 'rs0'"));

    // Make sure nay votes, do not prevent subsequent yeas (the way a yea vote would)
    args.set = "rs0";
    BSONObjBuilder responseBuilder2;
    getTopoCoord().prepareElectResponse(args, now++, OpTime(), &responseBuilder2, &result);
    BSONObj response2 = responseBuilder2.obj();
    ASSERT_EQUALS(1, response2["vote"].Int());
    ASSERT_EQUALS(round, response2["round"].OID());
}

TEST_F(PrepareElectResponseTest, RespondNegativelyWhenElectCommandHasANewerConfig) {
    // Test with us having a stale config version
    ReplicationCoordinator::ReplSetElectArgs args;
    args.set = "rs0";
    args.round = round;
    args.cfgver = 20;
    args.whoid = 1;

    BSONObjBuilder responseBuilder;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        args, now += Seconds(60), OpTime(), &responseBuilder, &result);
    stopCapturingLogMessages();
    BSONObj response = responseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(0, response["vote"].Int());
    ASSERT_EQUALS(round, response["round"].OID());
    ASSERT_EQUALS(1, countLogLinesContaining("not voting because our config version is stale"));

    // Make sure nay votes, do not prevent subsequent yeas (the way a yea vote would)
    args.cfgver = 10;
    BSONObjBuilder responseBuilder2;
    getTopoCoord().prepareElectResponse(args, now++, OpTime(), &responseBuilder2, &result);
    BSONObj response2 = responseBuilder2.obj();
    ASSERT_EQUALS(1, response2["vote"].Int());
    ASSERT_EQUALS(round, response2["round"].OID());
}

TEST_F(PrepareElectResponseTest, RespondWithAVetoWhenElectCommandHasAnOlderConfig) {
    // Test with them having a stale config version
    ReplicationCoordinator::ReplSetElectArgs args;
    args.set = "rs0";
    args.round = round;
    args.cfgver = 5;
    args.whoid = 1;

    BSONObjBuilder responseBuilder;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        args, now += Seconds(60), OpTime(), &responseBuilder, &result);
    stopCapturingLogMessages();
    BSONObj response = responseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(-10000, response["vote"].Int());
    ASSERT_EQUALS(round, response["round"].OID());
    ASSERT_EQUALS(1, countLogLinesContaining("received stale config version # during election"));

    // Make sure nay votes, do not prevent subsequent yeas (the way a yea vote would)
    args.cfgver = 10;
    BSONObjBuilder responseBuilder2;
    getTopoCoord().prepareElectResponse(args, now++, OpTime(), &responseBuilder2, &result);
    BSONObj response2 = responseBuilder2.obj();
    ASSERT_EQUALS(1, response2["vote"].Int());
    ASSERT_EQUALS(round, response2["round"].OID());
}

TEST_F(PrepareElectResponseTest, RespondWithAVetoWhenElectCommandHasANonExistentMember) {
    // Test with a non-existent node
    ReplicationCoordinator::ReplSetElectArgs args;
    args.set = "rs0";
    args.round = round;
    args.cfgver = 10;
    args.whoid = 99;

    BSONObjBuilder responseBuilder;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        args, now += Seconds(60), OpTime(), &responseBuilder, &result);
    stopCapturingLogMessages();
    BSONObj response = responseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(-10000, response["vote"].Int());
    ASSERT_EQUALS(round, response["round"].OID());
    ASSERT_EQUALS(1, countLogLinesContaining("couldn't find member with id 99"));

    // Make sure nay votes, do not prevent subsequent yeas (the way a yea vote would)
    args.whoid = 1;
    BSONObjBuilder responseBuilder2;
    getTopoCoord().prepareElectResponse(args, now++, OpTime(), &responseBuilder2, &result);
    BSONObj response2 = responseBuilder2.obj();
    ASSERT_EQUALS(1, response2["vote"].Int());
    ASSERT_EQUALS(round, response2["round"].OID());
}

TEST_F(PrepareElectResponseTest, RespondWithAVetoWhenElectCommandIsReceivedByPrimary) {
    // Test when we are already primary
    ReplicationCoordinator::ReplSetElectArgs args;
    args.set = "rs0";
    args.round = round;
    args.cfgver = 10;
    args.whoid = 1;

    getTopoCoord()._setCurrentPrimaryForTest(0);

    BSONObjBuilder responseBuilder;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        args, now += Seconds(60), OpTime(), &responseBuilder, &result);
    stopCapturingLogMessages();
    BSONObj response = responseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(-10000, response["vote"].Int());
    ASSERT_EQUALS(round, response["round"].OID());
    ASSERT_EQUALS(1, countLogLinesContaining("I am already primary"));

    // Make sure nay votes, do not prevent subsequent yeas (the way a yea vote would)
    getTopoCoord()._setCurrentPrimaryForTest(-1);
    BSONObjBuilder responseBuilder2;
    getTopoCoord().prepareElectResponse(args, now++, OpTime(), &responseBuilder2, &result);
    BSONObj response2 = responseBuilder2.obj();
    ASSERT_EQUALS(1, response2["vote"].Int());
    ASSERT_EQUALS(round, response2["round"].OID());
}

TEST_F(PrepareElectResponseTest, RespondWithAVetoWhenElectCommandIsReceivedWhileAPrimaryExists) {
    // Test when someone else is already primary
    ReplicationCoordinator::ReplSetElectArgs args;
    args.set = "rs0";
    args.round = round;
    args.cfgver = 10;
    args.whoid = 1;
    getTopoCoord()._setCurrentPrimaryForTest(2);

    BSONObjBuilder responseBuilder;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        args, now += Seconds(60), OpTime(), &responseBuilder, &result);
    stopCapturingLogMessages();
    BSONObj response = responseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(-10000, response["vote"].Int());
    ASSERT_EQUALS(round, response["round"].OID());
    ASSERT_EQUALS(1, countLogLinesContaining("h2:27017 is already primary"));

    // Make sure nay votes, do not prevent subsequent yeas (the way a yea vote would)
    getTopoCoord()._setCurrentPrimaryForTest(-1);
    BSONObjBuilder responseBuilder2;
    getTopoCoord().prepareElectResponse(args, now++, OpTime(), &responseBuilder2, &result);
    BSONObj response2 = responseBuilder2.obj();
    ASSERT_EQUALS(1, response2["vote"].Int());
    ASSERT_EQUALS(round, response2["round"].OID());
}

TEST_F(PrepareElectResponseTest, RespondWithAVetoWhenAHigherPriorityNodeExistsDuringElectCommand) {
    // Test trying to elect someone who isn't the highest priority node
    ReplicationCoordinator::ReplSetElectArgs args;
    args.set = "rs0";
    args.round = round;
    args.cfgver = 10;
    args.whoid = 1;

    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, OpTime());

    BSONObjBuilder responseBuilder;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        args, now += Seconds(60), OpTime(), &responseBuilder, &result);
    stopCapturingLogMessages();
    BSONObj response = responseBuilder.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(-10000, response["vote"].Int());
    ASSERT_EQUALS(round, response["round"].OID());
    ASSERT_EQUALS(1, countLogLinesContaining("h1:27017 has lower priority than h3:27017"));

    // Make sure nay votes, do not prevent subsequent yeas (the way a yea vote would)
    args.whoid = 3;
    BSONObjBuilder responseBuilder2;
    getTopoCoord().prepareElectResponse(args, now++, OpTime(), &responseBuilder2, &result);
    BSONObj response2 = responseBuilder2.obj();
    ASSERT_EQUALS(1, response2["vote"].Int());
    ASSERT_EQUALS(round, response2["round"].OID());
}

TEST_F(PrepareElectResponseTest, RespondPositivelyWhenElectCommandComesFromHighestPriorityNode) {
    // Test trying to elect someone who isn't the highest priority node, but all higher nodes
    // are down
    ReplicationCoordinator::ReplSetElectArgs args;
    args.set = "rs0";
    args.round = round;
    args.cfgver = 10;
    args.whoid = 1;

    receiveDownHeartbeat(HostAndPort("h3"), "rs0", OpTime());
    receiveDownHeartbeat(HostAndPort("h2"), "rs0", OpTime());

    BSONObjBuilder responseBuilder;
    Status result = Status::OK();
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        args, now += Seconds(60), OpTime(), &responseBuilder, &result);
    stopCapturingLogMessages();
    BSONObj response = responseBuilder.obj();
    ASSERT_EQUALS(1, response["vote"].Int());
    ASSERT_EQUALS(round, response["round"].OID());
}

TEST_F(PrepareElectResponseTest,
       RespondNegativelyToElectCommandsWhenAPositiveResponseWasGivenInTheVoteLeasePeriod) {
    // Test a valid vote
    ReplicationCoordinator::ReplSetElectArgs args;
    args.set = "rs0";
    args.round = round;
    args.cfgver = 10;
    args.whoid = 2;
    now = Date_t::fromMillisSinceEpoch(100);

    BSONObjBuilder responseBuilder1;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(
        args, now += Seconds(60), OpTime(), &responseBuilder1, &result);
    stopCapturingLogMessages();
    BSONObj response1 = responseBuilder1.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(1, response1["vote"].Int());
    ASSERT_EQUALS(round, response1["round"].OID());
    ASSERT_EQUALS(1, countLogLinesContaining("voting yea for h2:27017 (2)"));

    // Test what would be a valid vote except that we already voted too recently
    args.whoid = 3;

    BSONObjBuilder responseBuilder2;
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(args, now, OpTime(), &responseBuilder2, &result);
    stopCapturingLogMessages();
    BSONObj response2 = responseBuilder2.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(0, response2["vote"].Int());
    ASSERT_EQUALS(round, response2["round"].OID());
    ASSERT_EQUALS(1,
                  countLogLinesContaining("voting no for h3:27017; "
                                          "voted for h2:27017 0 secs ago"));

    // Test that after enough time passes the same vote can proceed
    now += Seconds(30) + Milliseconds(1);  // just over 30 seconds later

    BSONObjBuilder responseBuilder3;
    startCapturingLogMessages();
    getTopoCoord().prepareElectResponse(args, now++, OpTime(), &responseBuilder3, &result);
    stopCapturingLogMessages();
    BSONObj response3 = responseBuilder3.obj();
    ASSERT_OK(result);
    ASSERT_EQUALS(1, response3["vote"].Int());
    ASSERT_EQUALS(round, response3["round"].OID());
    ASSERT_EQUALS(1, countLogLinesContaining("voting yea for h3:27017 (3)"));
}

TEST_F(TopoCoordTest, NodeReturnsReplicaSetNotFoundWhenReceivingElectCommandWhileRemoved) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 5
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017"))),
                 0);
    // Reconfig to remove self.
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
    ASSERT_EQUALS(MemberState::RS_REMOVED, getTopoCoord().getMemberState().s);

    ReplicationCoordinator::ReplSetElectArgs args;
    BSONObjBuilder response;
    Status status = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    getTopoCoord().prepareElectResponse(args, now(), OpTime(), &response, &status);
    ASSERT_EQUALS(ErrorCodes::ReplicaSetNotFound, status);
    ASSERT_EQUALS("Cannot participate in election because not initialized", status.reason());
}

TEST_F(TopoCoordTest, NodeReturnsReplicaSetNotFoundWhenReceivingElectCommandWhileNotInitialized) {
    ReplicationCoordinator::ReplSetElectArgs args;
    BSONObjBuilder response;
    Status status = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    getTopoCoord().prepareElectResponse(args, now(), OpTime(), &response, &status);
    ASSERT_EQUALS(ErrorCodes::ReplicaSetNotFound, status);
    ASSERT_EQUALS("Cannot participate in election because not initialized", status.reason());
}

class PrepareFreezeResponseTest : public TopoCoordTest {
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
                                                      << "host2:27017"))),
                     0);
    }

    BSONObj prepareFreezeResponse(int duration) {
        BSONObjBuilder response;
        startCapturingLogMessages();
        getTopoCoord().prepareFreezeResponse(now()++, duration, &response);
        stopCapturingLogMessages();
        return response.obj();
    }
};

TEST_F(PrepareFreezeResponseTest, FreezeForOneSecondWhenToldToFreezeForZeroSeconds) {
    BSONObj response = prepareFreezeResponse(0);
    ASSERT_EQUALS("unfreezing", response["info"].String());
    ASSERT_EQUALS(1, countLogLinesContaining("'unfreezing'"));
    // 1 instead of 0 because it assigns to "now" in this case
    ASSERT_EQUALS(1LL, getTopoCoord().getStepDownTime().asInt64());
}

TEST_F(PrepareFreezeResponseTest, LogAMessageAndFreezeForOneSecondWhenToldToFreezeForOneSecond) {
    BSONObj response = prepareFreezeResponse(1);
    ASSERT_EQUALS("you really want to freeze for only 1 second?", response["warning"].String());
    ASSERT_EQUALS(1, countLogLinesContaining("'freezing' for 1 seconds"));
    // 1001 because "now" was incremented once during initialization + 1000 ms wait
    ASSERT_EQUALS(1001LL, getTopoCoord().getStepDownTime().asInt64());
}

TEST_F(PrepareFreezeResponseTest, FreezeForTheSpecifiedDurationWhenToldToFreeze) {
    BSONObj response = prepareFreezeResponse(20);
    ASSERT_TRUE(response.isEmpty());
    ASSERT_EQUALS(1, countLogLinesContaining("'freezing' for 20 seconds"));
    // 20001 because "now" was incremented once during initialization + 20000 ms wait
    ASSERT_EQUALS(20001LL, getTopoCoord().getStepDownTime().asInt64());
}

TEST_F(PrepareFreezeResponseTest, FreezeForOneSecondWhenToldToFreezeForZeroSecondsWhilePrimary) {
    makeSelfPrimary();
    BSONObj response = prepareFreezeResponse(0);
    ASSERT_EQUALS("unfreezing", response["info"].String());
    // doesn't mention being primary in this case for some reason
    ASSERT_EQUALS(0, countLogLinesContaining("received freeze command but we are primary"));
    // 1 instead of 0 because it assigns to "now" in this case
    ASSERT_EQUALS(1LL, getTopoCoord().getStepDownTime().asInt64());
}

TEST_F(PrepareFreezeResponseTest, NodeDoesNotFreezeWhenToldToFreezeForOneSecondWhilePrimary) {
    makeSelfPrimary();
    BSONObj response = prepareFreezeResponse(1);
    ASSERT_EQUALS("you really want to freeze for only 1 second?", response["warning"].String());
    ASSERT_EQUALS(1, countLogLinesContaining("received freeze command but we are primary"));
    ASSERT_EQUALS(0LL, getTopoCoord().getStepDownTime().asInt64());
}

TEST_F(PrepareFreezeResponseTest, NodeDoesNotFreezeWhenToldToFreezeForSeveralSecondsWhilePrimary) {
    makeSelfPrimary();
    BSONObj response = prepareFreezeResponse(20);
    ASSERT_TRUE(response.isEmpty());
    ASSERT_EQUALS(1, countLogLinesContaining("received freeze command but we are primary"));
    ASSERT_EQUALS(0LL, getTopoCoord().getStepDownTime().asInt64());
}

TEST_F(TopoCoordTest,
       UnfreezeImmediatelyWhenToldToFreezeForZeroSecondsAfterBeingToldToFreezeForLonger) {
    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 5
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017"))),
                 0);
    setSelfMemberState(MemberState::RS_SECONDARY);

    BSONObjBuilder response;
    getTopoCoord().prepareFreezeResponse(now()++, 20, &response);
    ASSERT(response.obj().isEmpty());
    BSONObjBuilder response2;
    getTopoCoord().prepareFreezeResponse(now()++, 0, &response2);
    ASSERT_EQUALS("unfreezing", response2.obj()["info"].String());
    ASSERT(TopologyCoordinator::Role::candidate == getTopoCoord().getRole());
}

class PrepareHeartbeatResponseTest : public TopoCoordTest {
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
                                                      << "h3"))),
                     0);
        setSelfMemberState(MemberState::RS_SECONDARY);
    }

    void prepareHeartbeatResponse(const ReplSetHeartbeatArgs& args,
                                  OpTime lastOpApplied,
                                  ReplSetHeartbeatResponse* response,
                                  Status* result) {
        *result = getTopoCoord().prepareHeartbeatResponse(
            now()++, args, "rs0", lastOpApplied, lastOpApplied, response);
    }
};

TEST_F(PrepareHeartbeatResponseTest,
       NodeReturnsBadValueWhenAHeartbeatRequestHasAnInvalidProtocolVersion) {
    // set up args with bad protocol version
    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(3);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponse(args, OpTime(), &response, &result);
    ASSERT_EQUALS(ErrorCodes::BadValue, result);
    ASSERT_EQUALS("replset: incompatible replset protocol version: 3", result.reason());
    ASSERT_EQUALS("", response.getHbMsg());
}

TEST_F(PrepareHeartbeatResponseTest, NodeReturnsBadValueWhenAHeartbeatRequestIsFromSelf) {
    // set up args with incorrect replset name
    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(1);
    args.setSetName("rs0");
    args.setSenderId(10);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");
    prepareHeartbeatResponse(args, OpTime(), &response, &result);
    ASSERT_EQUALS(ErrorCodes::BadValue, result);
    ASSERT(result.reason().find("from member with the same member ID as our self"))
        << "Actual string was \"" << result.reason() << '"';
    ASSERT_EQUALS("", response.getHbMsg());
}

TEST_F(PrepareHeartbeatResponseTest,
       NodeReturnsInconsistentReplicaSetNamesWhenAHeartbeatRequestHasADifferentReplicaSetName) {
    // set up args with incorrect replset name
    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(1);
    args.setSetName("rs1");
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    startCapturingLogMessages();
    prepareHeartbeatResponse(args, OpTime(), &response, &result);
    stopCapturingLogMessages();
    ASSERT_EQUALS(ErrorCodes::InconsistentReplicaSetNames, result);
    ASSERT(result.reason().find("repl set names do not match")) << "Actual string was \""
                                                                << result.reason() << '"';
    ASSERT_EQUALS(1,
                  countLogLinesContaining("replSet set names do not match, ours: rs0; remote "
                                          "node's: rs1"));
    ASSERT_TRUE(response.isMismatched());
    ASSERT_EQUALS("", response.getHbMsg());
}

TEST_F(PrepareHeartbeatResponseTest,
       PopulateFullHeartbeatResponseEvenWhenHeartbeatRequestLacksASenderID) {
    // set up args without a senderID
    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(1);
    args.setSetName("rs0");
    args.setConfigVersion(1);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponse(args, OpTime(), &response, &result);
    ASSERT_OK(result);
    ASSERT_FALSE(response.isElectable());
    ASSERT_TRUE(response.isReplSet());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_EQUALS(OpTime(), response.getDurableOpTime());
    ASSERT_EQUALS(0, durationCount<Seconds>(response.getTime()));
    ASSERT_EQUALS("", response.getHbMsg());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(1, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseTest,
       PopulateFullHeartbeatResponseEvenWhenHeartbeatRequestHasAnInvalidSenderID) {
    // set up args with a senderID which is not present in our config
    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(1);
    args.setSetName("rs0");
    args.setConfigVersion(1);
    args.setSenderId(2);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponse(args, OpTime(), &response, &result);
    ASSERT_OK(result);
    ASSERT_FALSE(response.isElectable());
    ASSERT_TRUE(response.isReplSet());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_EQUALS(OpTime(), response.getDurableOpTime());
    ASSERT_EQUALS(0, durationCount<Seconds>(response.getTime()));
    ASSERT_EQUALS("", response.getHbMsg());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(1, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseTest,
       PopulateHeartbeatResponseWithFullConfigWhenHeartbeatRequestHasAnOldConfigVersion) {
    // set up args with a config version lower than ours
    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(1);
    args.setConfigVersion(0);
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponse(args, OpTime(), &response, &result);
    ASSERT_OK(result);
    ASSERT_TRUE(response.hasConfig());
    ASSERT_FALSE(response.isElectable());
    ASSERT_TRUE(response.isReplSet());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_EQUALS(OpTime(), response.getDurableOpTime());
    ASSERT_EQUALS(0, durationCount<Seconds>(response.getTime()));
    ASSERT_EQUALS("", response.getHbMsg());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(1, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseTest,
       PopulateFullHeartbeatResponseWhenHeartbeatRequestHasANewerConfigVersion) {
    // set up args with a config version higher than ours
    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(1);
    args.setConfigVersion(10);
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponse(args, OpTime(), &response, &result);
    ASSERT_OK(result);
    ASSERT_FALSE(response.hasConfig());
    ASSERT_FALSE(response.isElectable());
    ASSERT_TRUE(response.isReplSet());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_EQUALS(OpTime(), response.getDurableOpTime());
    ASSERT_EQUALS(0, durationCount<Seconds>(response.getTime()));
    ASSERT_EQUALS("", response.getHbMsg());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(1, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseTest,
       SetStateDisagreementInHeartbeatResponseWhenHeartbeatRequestIsFromADownNode) {
    // set up args with sender down from our perspective
    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(1);
    args.setConfigVersion(1);
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponse(args, OpTime(), &response, &result);
    ASSERT_OK(result);
    ASSERT_FALSE(response.isElectable());
    ASSERT_TRUE(response.isReplSet());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_EQUALS(OpTime(), response.getDurableOpTime());
    ASSERT_EQUALS(0, durationCount<Seconds>(response.getTime()));
    ASSERT_EQUALS("", response.getHbMsg());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(1, response.getConfigVersion());
    ASSERT_TRUE(response.isStateDisagreement());
}

TEST_F(PrepareHeartbeatResponseTest,
       SetElectableInHeartbeatResponseWhenWeCanSeeAMajorityOfTheNodes) {
    // set up args and acknowledge sender
    heartbeatFromMember(HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, OpTime());
    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(1);
    args.setConfigVersion(1);
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponse(args, OpTime(Timestamp(100, 0), 0), &response, &result);
    ASSERT_OK(result);
    // this change to true because we can now see a majority, unlike in the previous cases
    ASSERT_TRUE(response.isElectable());
    ASSERT_TRUE(response.isReplSet());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_EQUALS(OpTime(Timestamp(100, 0), 0), response.getDurableOpTime());
    ASSERT_EQUALS(0, durationCount<Seconds>(response.getTime()));
    ASSERT_EQUALS("", response.getHbMsg());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(1, response.getConfigVersion());
}

TEST_F(TopoCoordTest, SetConfigVersionToNegativeTwoInHeartbeatResponseWhenNoConfigHasBeenReceived) {
    // set up args and acknowledge sender
    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(1);
    args.setConfigVersion(1);
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    // prepare response and check the results
    Status result = getTopoCoord().prepareHeartbeatResponse(
        now()++, args, "rs0", OpTime(), OpTime(), &response);
    ASSERT_OK(result);
    ASSERT_FALSE(response.isElectable());
    ASSERT_TRUE(response.isReplSet());
    ASSERT_EQUALS(MemberState::RS_STARTUP, response.getState().s);
    ASSERT_EQUALS(OpTime(), response.getDurableOpTime());
    ASSERT_EQUALS(0, durationCount<Seconds>(response.getTime()));
    ASSERT_EQUALS("", response.getHbMsg());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(-2, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseTest,
       SetElectableTrueAndStatePrimaryInHeartbeatResponseWhenPrimary) {
    makeSelfPrimary(Timestamp(10, 0));
    heartbeatFromMember(HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, OpTime());

    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(1);
    args.setConfigVersion(1);
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponse(args, OpTime(Timestamp(11, 0), 0), &response, &result);
    ASSERT_OK(result);
    // electable because we are already primary
    ASSERT_TRUE(response.isElectable());
    ASSERT_TRUE(response.isReplSet());
    ASSERT_EQUALS(MemberState::RS_PRIMARY, response.getState().s);
    ASSERT_EQUALS(OpTime(Timestamp(11, 0), 0), response.getDurableOpTime());
    ASSERT_EQUALS(Timestamp(10, 0), response.getElectionTime());
    ASSERT_EQUALS(0, durationCount<Seconds>(response.getTime()));
    ASSERT_EQUALS("", response.getHbMsg());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
    ASSERT_EQUALS(1, response.getConfigVersion());
}

TEST_F(PrepareHeartbeatResponseTest,
       IncludeSyncingFromMessageInHeartbeatResponseWhenThereIsASyncSource) {
    // get a sync source
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, OpTime());
    heartbeatFromMember(HostAndPort("h3"), "rs0", MemberState::RS_SECONDARY, OpTime());
    heartbeatFromMember(
        HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(1, 0), 0));
    heartbeatFromMember(
        HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(1, 0), 0));
    getTopoCoord().chooseNewSyncSource(now()++, Timestamp());

    // set up args
    ReplSetHeartbeatArgs args;
    args.setProtocolVersion(1);
    args.setConfigVersion(1);
    args.setSetName("rs0");
    args.setSenderId(20);
    ReplSetHeartbeatResponse response;
    Status result(ErrorCodes::InternalError, "prepareHeartbeatResponse didn't set result");

    // prepare response and check the results
    prepareHeartbeatResponse(args, OpTime(Timestamp(100, 0), 0), &response, &result);
    ASSERT_OK(result);
    ASSERT_TRUE(response.isElectable());
    ASSERT_TRUE(response.isReplSet());
    ASSERT_EQUALS(MemberState::RS_SECONDARY, response.getState().s);
    ASSERT_EQUALS(OpTime(Timestamp(100, 0), 0), response.getDurableOpTime());
    ASSERT_EQUALS(0, durationCount<Seconds>(response.getTime()));
    // changed to a syncing message because our sync source changed recently
    ASSERT_EQUALS("syncing from: h2:27017", response.getHbMsg());
    ASSERT_EQUALS("rs0", response.getReplicaSetName());
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

// TODO(dannenberg) figure out what this is trying to test..
TEST_F(HeartbeatResponseTest, ReconfigBetweenHeartbeatRequestAndRepsonse) {
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
    getTopoCoord().prepareHeartbeatRequest(now()++, "rs0", HostAndPort("host3"));

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 2
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 2 << "host"
                                                  << "host3:27017"))),
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

// TODO(dannenberg) figure out what this is trying to test..
TEST_F(HeartbeatResponseTest, ReconfigNodeRemovedBetweenHeartbeatRequestAndRepsonse) {
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
    getTopoCoord().prepareHeartbeatRequest(now()++, "rs0", HostAndPort("host3"));

    updateConfig(BSON("_id"
                      << "rs0"
                      << "version"
                      << 2
                      << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host"
                                               << "host1:27017")
                                    << BSON("_id" << 1 << "host"
                                                  << "host2:27017"))),
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

TEST_F(HeartbeatResponseTest, ShouldChangeSyncSourceWhenMemberNotInConfig) {
    // In this test, the TopologyCoordinator should tell us to change sync sources away from
    // "host4" since "host4" is absent from the config of version 10.
    ReplSetMetadata metadata(0, OpTime(), OpTime(), 10, OID(), -1, -1);
    ASSERT_TRUE(
        getTopoCoord().shouldChangeSyncSource(HostAndPort("host4"), OpTime(), metadata, now()));
}

TEST_F(HeartbeatResponseTest, ShouldChangeSyncSourceWhenMemberHasYetToHeartbeatUs) {
    // In this test, the TopologyCoordinator should not tell us to change sync sources away from
    // "host2" since we do not yet have a heartbeat (and as a result do not yet have an optime)
    // for "host2"
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(), now()));
}

TEST_F(HeartbeatResponseTest, ShouldNotChangeSyncSourceWhenNodeIsFreshByHeartbeatButNotMetadata) {
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
    auto metadata = makeMetadata(lastOpTimeApplied);
    ASSERT_FALSE(
        getTopoCoord().shouldChangeSyncSource(HostAndPort("host2"), OpTime(), metadata, now()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("re-evaluating sync source"));
}

TEST_F(HeartbeatResponseTest, ShouldNotChangeSyncSourceWhenNodeIsStaleByHeartbeatButNotMetadata) {
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
    auto metadata = makeMetadata(fresherLastOpTimeApplied);
    ASSERT_FALSE(
        getTopoCoord().shouldChangeSyncSource(HostAndPort("host2"), OpTime(), metadata, now()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("re-evaluating sync source"));
}

TEST_F(HeartbeatResponseTest, ShouldChangeSyncSourceWhenFresherMemberExists) {
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
        HostAndPort("host2"), OpTime(), makeMetadata(), now()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("re-evaluating sync source"));
}

TEST_F(HeartbeatResponseTest, ShouldNotChangeSyncSourceWhileFresherMemberIsBlackListed) {
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
        HostAndPort("host2"), OpTime(), makeMetadata(), now()));

    // unblacklist with too early a time (node should remained blacklisted)
    getTopoCoord().unblacklistSyncSource(HostAndPort("host3"), now() + Milliseconds(90));
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(), now()));

    // unblacklist and it should succeed
    getTopoCoord().unblacklistSyncSource(HostAndPort("host3"), now() + Milliseconds(100));
    startCapturingLogMessages();
    ASSERT_TRUE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(), now()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("re-evaluating sync source"));
}

TEST_F(HeartbeatResponseTest, ShouldNotChangeSyncSourceWhenFresherMemberIsDown) {
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
        HostAndPort("host2"), OpTime(), makeMetadata(), now()));
}

TEST_F(HeartbeatResponseTest, ShouldNotChangeSyncSourceWhenFresherMemberIsNotReadable) {
    // In this test, the TopologyCoordinator should not tell us to change sync sources away from
    // "host2" and to "host3" despite "host2" being more than maxSyncSourceLagSecs(30) behind
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
        HostAndPort("host2"), OpTime(), makeMetadata(), now()));
}

TEST_F(HeartbeatResponseTest, ShouldNotChangeSyncSourceWhenFresherMemberDoesNotBuildIndexes) {
    // In this test, the TopologyCoordinator should not tell us to change sync sources away from
    // "host2" and to "host3" despite "host2" being more than maxSyncSourceLagSecs(30) behind
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
    ASSERT_FALSE(getTopoCoord().shouldChangeSyncSource(
        HostAndPort("host2"), OpTime(), makeMetadata(), now()));
}

TEST_F(HeartbeatResponseTest,
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
        HostAndPort("host2"), OpTime(), makeMetadata(), now()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("re-evaluating sync source"));
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
    const auto status = getTopoCoord().checkShouldStandForElection(now()++, OpTime());
    ASSERT_EQ(ErrorCodes::NodeNotElectable, status);
    ASSERT_STRING_CONTAINS(status.reason(), "there is a Primary");
}

TEST_F(TopoCoordTest, ShouldNotStandForElectionWhileTooStale) {
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
    const auto status =
        getTopoCoord().checkShouldStandForElection(now()++, OpTime(Timestamp(100, 0), 0));
    ASSERT_EQ(ErrorCodes::NodeNotElectable, status);
    ASSERT_STRING_CONTAINS(status.reason(), "my last optime is");
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
    ASSERT_EQ(ErrorCodes::NodeNotElectable, status);
    ASSERT_STRING_CONTAINS(status.reason(), "not a member of a valid replica set config");
}

TEST_F(TopoCoordTest, ShouldNotStandForElectionWhenAPositiveResponseWasGivenInTheVoteLeasePeriod) {
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
        HostAndPort("h2"), "rs0", MemberState::RS_SECONDARY, OpTime(Timestamp(100, 0), 0));

    // vote for another node
    OID remoteRound = OID::gen();
    ReplicationCoordinator::ReplSetElectArgs electArgs;
    electArgs.set = "rs0";
    electArgs.round = remoteRound;
    electArgs.cfgver = 1;
    electArgs.whoid = 20;

    // need to be 30 secs beyond the start of time to pass last vote lease
    now() += Seconds(30);
    BSONObjBuilder electResponseBuilder;
    Status result = Status(ErrorCodes::InternalError, "status not set by prepareElectResponse");
    getTopoCoord().prepareElectResponse(
        electArgs, now()++, OpTime(Timestamp(100, 0), 0), &electResponseBuilder, &result);
    BSONObj response = electResponseBuilder.obj();
    ASSERT_OK(result);
    std::cout << response;
    ASSERT_EQUALS(1, response["vote"].Int());
    ASSERT_EQUALS(remoteRound, response["round"].OID());

    const auto status =
        getTopoCoord().checkShouldStandForElection(now()++, OpTime(Timestamp(10, 0), 0));
    ASSERT_EQ(ErrorCodes::NodeNotElectable, status);
    ASSERT_STRING_CONTAINS(status.reason(), "I recently voted for ");
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

TEST_F(TopoCoordTest, CSRSConfigServerRejectsPV0Config) {
    ON_BLOCK_EXIT([]() { serverGlobalParams.clusterRole = ClusterRole::None; });
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    TopologyCoordinatorImpl::Options options;
    options.clusterRole = ClusterRole::ConfigServer;
    setOptions(options);
    getTopoCoord().setStorageEngineSupportsReadCommitted(false);

    auto configObj = BSON("_id"
                          << "rs0"
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
                                                      << "h3")));
    ReplicaSetConfig config;
    ASSERT_OK(config.initialize(configObj, false));
    ASSERT_EQ(ErrorCodes::BadValue, config.validate());
}

}  // namespace
}  // namespace repl
}  // namespace mongo
