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

#pragma once

#include <iosfwd>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Timestamp;

namespace repl {

class HeartbeatResponseAction;
class OpTime;
class ReplSetHeartbeatArgs;
class ReplicaSetConfig;
class TagSubgroup;
class LastVote;
struct MemberState;

/**
 * Replication Topology Coordinator interface.
 *
 * This object is responsible for managing the topology of the cluster.
 * Tasks include consensus and leader election, chaining, and configuration management.
 * Methods of this class should be non-blocking.
 */
class TopologyCoordinator {
    MONGO_DISALLOW_COPYING(TopologyCoordinator);

public:
    class Role;

    virtual ~TopologyCoordinator();

    ////////////////////////////////////////////////////////////
    //
    // State inspection methods.
    //
    ////////////////////////////////////////////////////////////

    /**
     * Gets the role of this member in the replication protocol.
     */
    virtual Role getRole() const = 0;

    /**
     * Gets the MemberState of this member in the replica set.
     */
    virtual MemberState getMemberState() const = 0;

    /**
     * Returns the address of the current sync source, or an empty HostAndPort if there is no
     * current sync source.
     */
    virtual HostAndPort getSyncSourceAddress() const = 0;

    /**
     * Retrieves a vector of HostAndPorts containing all nodes that are neither DOWN nor
     * ourself.
     */
    virtual std::vector<HostAndPort> getMaybeUpHostAndPorts() const = 0;

    /**
     * Gets the earliest time the current node will stand for election.
     */
    virtual Date_t getStepDownTime() const = 0;

    /**
     * Gets the current value of the maintenance mode counter.
     */
    virtual int getMaintenanceCount() const = 0;

    /**
     * Gets the latest term this member is aware of. If this member is the primary,
     * it's the current term of the replica set.
     */
    virtual long long getTerm() = 0;

    enum class UpdateTermResult { kAlreadyUpToDate, kTriggerStepDown, kUpdatedTerm };

    /**
     * Sets the latest term this member is aware of to the higher of its current value and
     * the value passed in as "term".
     * Returns the result of setting the term value, or if a stepdown should be triggered.
     */
    virtual UpdateTermResult updateTerm(long long term, Date_t now) = 0;

    ////////////////////////////////////////////////////////////
    //
    // Basic state manipulation methods.
    //
    ////////////////////////////////////////////////////////////

    /**
     * Sets the index into the config used when we next choose a sync source
     */
    virtual void setForceSyncSourceIndex(int index) = 0;

    /**
     * Chooses and sets a new sync source, based on our current knowledge of the world.
     */
    virtual HostAndPort chooseNewSyncSource(Date_t now, const Timestamp& lastTimestampApplied) = 0;

    /**
     * Suppresses selecting "host" as sync source until "until".
     */
    virtual void blacklistSyncSource(const HostAndPort& host, Date_t until) = 0;

    /**
     * Removes a single entry "host" from the list of potential sync sources which we
     * have blacklisted, if it is supposed to be unblacklisted by "now".
     */
    virtual void unblacklistSyncSource(const HostAndPort& host, Date_t now) = 0;

    /**
     * Clears the list of potential sync sources we have blacklisted.
     */
    virtual void clearSyncSourceBlacklist() = 0;

    /**
     * Determines if a new sync source should be chosen, if a better candidate sync source is
     * available.  If the current sync source's last optime ("syncSourceLastOpTime" under
     * protocolVersion 1, but pulled from the MemberHeartbeatData in protocolVersion 0) is more than
     * _maxSyncSourceLagSecs behind any syncable source, this function returns true. If we are
     * running in ProtocolVersion 1, our current sync source is not primary, has no sync source
     * ("syncSourceHasSyncSource" is false), and only has data up to "myLastOpTime", returns true.
     *
     * "now" is used to skip over currently blacklisted sync sources.
     */
    virtual bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                        const OpTime& myLastOpTime,
                                        const rpc::ReplSetMetadata& metadata,
                                        Date_t now) const = 0;

    /**
     * Checks whether we are a single node set and we are not in a stepdown period.  If so,
     * puts us into candidate mode, otherwise does nothing.  This is used to ensure that
     * nodes in a single node replset become primary again when their stepdown period ends.
     */
    virtual bool becomeCandidateIfStepdownPeriodOverAndSingleNodeSet(Date_t now) = 0;

    /**
     * Sets the earliest time the current node will stand for election to "newTime".
     *
     * Until this time, while the node may report itself as electable, it will not stand
     * for election.
     */
    virtual void setElectionSleepUntil(Date_t newTime) = 0;

    /**
     * Sets the reported mode of this node to one of RS_SECONDARY, RS_STARTUP2, RS_ROLLBACK or
     * RS_RECOVERING, when getRole() == Role::follower.  This is the interface by which the
     * applier changes the reported member state of the current node, and enables or suppresses
     * electability of the current node.  All modes but RS_SECONDARY indicate an unelectable
     * follower state (one that cannot transition to candidate).
     */
    virtual void setFollowerMode(MemberState::MS newMode) = 0;

    /**
     * Adjusts the maintenance mode count by "inc".
     *
     * It is an error to call this method if getRole() does not return Role::follower.
     * It is an error to allow the maintenance count to go negative.
     */
    virtual void adjustMaintenanceCountBy(int inc) = 0;

    ////////////////////////////////////////////////////////////
    //
    // Methods that prepare responses to command requests.
    //
    ////////////////////////////////////////////////////////////

    // produces a reply to a replSetSyncFrom command
    virtual void prepareSyncFromResponse(const HostAndPort& target,
                                         const OpTime& lastOpApplied,
                                         BSONObjBuilder* response,
                                         Status* result) = 0;

    // produce a reply to a replSetFresh command
    virtual void prepareFreshResponse(const ReplicationCoordinator::ReplSetFreshArgs& args,
                                      Date_t now,
                                      const OpTime& lastOpApplied,
                                      BSONObjBuilder* response,
                                      Status* result) = 0;

    // produce a reply to a received electCmd
    virtual void prepareElectResponse(const ReplicationCoordinator::ReplSetElectArgs& args,
                                      Date_t now,
                                      const OpTime& lastOpApplied,
                                      BSONObjBuilder* response,
                                      Status* result) = 0;

    // produce a reply to a heartbeat
    virtual Status prepareHeartbeatResponse(Date_t now,
                                            const ReplSetHeartbeatArgs& args,
                                            const std::string& ourSetName,
                                            const OpTime& lastOpApplied,
                                            const OpTime& lastOpDurable,
                                            ReplSetHeartbeatResponse* response) = 0;

    // produce a reply to a V1 heartbeat
    virtual Status prepareHeartbeatResponseV1(Date_t now,
                                              const ReplSetHeartbeatArgsV1& args,
                                              const std::string& ourSetName,
                                              const OpTime& lastOpApplied,
                                              const OpTime& lastOpDurable,
                                              ReplSetHeartbeatResponse* response) = 0;

    struct ReplSetStatusArgs {
        Date_t now;
        unsigned selfUptime;
        const OpTime& lastOpApplied;
        const OpTime& lastOpDurable;
        const OpTime& lastCommittedOpTime;
        const OpTime& readConcernMajorityOpTime;
    };

    // produce a reply to a status request
    virtual void prepareStatusResponse(const ReplSetStatusArgs& rsStatusArgs,
                                       BSONObjBuilder* response,
                                       Status* result) = 0;

    // produce a reply to an ismaster request.  It is only valid to call this if we are a
    // replset.
    virtual void fillIsMasterForReplSet(IsMasterResponse* response) = 0;

    // produce a reply to a freeze request
    virtual void prepareFreezeResponse(Date_t now, int secs, BSONObjBuilder* response) = 0;

    ////////////////////////////////////////////////////////////
    //
    // Methods for sending and receiving heartbeats,
    // reconfiguring and handling the results of standing for
    // election.
    //
    ////////////////////////////////////////////////////////////

    /**
     * Updates the topology coordinator's notion of the replica set configuration.
     *
     * "newConfig" is the new configuration, and "selfIndex" is the index of this
     * node's configuration information in "newConfig", or "selfIndex" is -1 to
     * indicate that this node is not a member of "newConfig".
     *
     * newConfig.isInitialized() should be true, though implementations may accept
     * configurations where this is not true, for testing purposes.
     */
    virtual void updateConfig(const ReplicaSetConfig& newConfig,
                              int selfIndex,
                              Date_t now,
                              const OpTime& lastOpApplied) = 0;

    /**
     * Prepares a heartbeat request appropriate for sending to "target", assuming the
     * current time is "now".  "ourSetName" is used as the name for our replica set if
     * the topology coordinator does not have a valid configuration installed.
     *
     * The returned pair contains proper arguments for a replSetHeartbeat command, and
     * an amount of time to wait for the response.
     *
     * This call should be paired (with intervening network communication) with a call to
     * processHeartbeatResponse for the same "target".
     */
    virtual std::pair<ReplSetHeartbeatArgs, Milliseconds> prepareHeartbeatRequest(
        Date_t now, const std::string& ourSetName, const HostAndPort& target) = 0;
    virtual std::pair<ReplSetHeartbeatArgsV1, Milliseconds> prepareHeartbeatRequestV1(
        Date_t now, const std::string& ourSetName, const HostAndPort& target) = 0;

    /**
     * Processes a heartbeat response from "target" that arrived around "now", having
     * spent "networkRoundTripTime" millis on the network.
     *
     * Updates internal topology coordinator state, and returns instructions about what action
     * to take next.
     *
     * If the next action indicates StartElection, the topology coordinator has transitioned to
     * the "candidate" role, and will remain there until processWinElection or
     * processLoseElection are called.
     *
     * If the next action indicates "StepDownSelf", the topology coordinator has transitioned
     * to the "follower" role from "leader", and the caller should take any necessary actions
     * to become a follower.
     *
     * If the next action indicates "StepDownRemotePrimary", the caller should take steps to
     * cause the specified remote host to step down from primary to secondary.
     *
     * If the next action indicates "Reconfig", the caller should verify the configuration in
     * hbResponse is acceptable, perform any other reconfiguration actions it must, and call
     * updateConfig with the new configuration and the appropriate value for "selfIndex".  It
     * must also wrap up any outstanding elections (by calling processLoseElection or
     * processWinElection) before calling updateConfig.
     *
     * This call should be paired (with intervening network communication) with a call to
     * prepareHeartbeatRequest for the same "target".
     */
    virtual HeartbeatResponseAction processHeartbeatResponse(
        Date_t now,
        Milliseconds networkRoundTripTime,
        const HostAndPort& target,
        const StatusWith<ReplSetHeartbeatResponse>& hbResponse,
        const OpTime& myLastOpApplied) = 0;

    /**
     * Marks a member has down from our persepctive and returns a HeartbeatResponseAction, which
     * will be StepDownSelf if we can no longer see a majority of the nodes.
     */
    virtual HeartbeatResponseAction setMemberAsDown(Date_t now,
                                                    const int memberIndex,
                                                    const OpTime& myLastOpApplied) = 0;

    /**
     * If getRole() == Role::candidate and this node has not voted too recently, updates the
     * lastVote tracker and returns true.  Otherwise, returns false.
     */
    virtual bool voteForMyself(Date_t now) = 0;

    /**
     * Sets lastVote to be for ourself in this term.
     */
    virtual void voteForMyselfV1() = 0;

    /**
     * Sets election id and election optime.
     */
    virtual void setElectionInfo(OID electionId, Timestamp electionOpTime) = 0;

    /**
     * Performs state updates associated with winning an election.
     *
     * It is an error to call this if the topology coordinator is not in candidate mode.
     *
     * Exactly one of either processWinElection or processLoseElection must be called if
     * processHeartbeatResponse returns StartElection, to exit candidate mode.
     */
    virtual void processWinElection(OID electionId, Timestamp electionOpTime) = 0;

    /**
     * Performs state updates associated with losing an election.
     *
     * It is an error to call this if the topology coordinator is not in candidate mode.
     *
     * Exactly one of either processWinElection or processLoseElection must be called if
     * processHeartbeatResponse returns StartElection, to exit candidate mode.
     */
    virtual void processLoseElection() = 0;

    /**
     * Tries to transition the coordinator from the leader role to the follower role.
     *
     * Fails if "force" is not set and no follower is known to be up.  It is illegal
     * to call this method if the node is not leader.
     *
     * Returns whether or not the step down succeeded.
     */
    virtual bool stepDown(Date_t until, bool force, const OpTime& lastOpApplied) = 0;

    /**
     * Sometimes a request to step down comes in (like via a heartbeat), but we don't have the
     * global exclusive lock so we can't actually stepdown at that moment. When that happens
     * we record that a stepdown request is pending and schedule work to stepdown in the global
     * lock.  This method is called after holding the global lock to perform the actual
     * stepdown, but only if the node hasn't already stepped down another way since the work was
     * scheduled.  Returns true if it actually steps down, and false otherwise.
     */
    virtual bool stepDownIfPending() = 0;

    /**
     * Considers whether or not this node should stand for election, and returns true
     * if the node has transitioned to candidate role as a result of the call.
     */
    virtual Status checkShouldStandForElection(Date_t now, const OpTime& lastOpApplied) const = 0;

    /**
     * Set the outgoing heartbeat message from self
     */
    virtual void setMyHeartbeatMessage(const Date_t now, const std::string& s) = 0;

    /**
     * Prepares a BSONObj describing the current term, primary, and lastOp information.
     */
    virtual void prepareReplMetadata(rpc::ReplSetMetadata* metadata,
                                     const OpTime& lastVisibleOpTime,
                                     const OpTime& lastCommittedOpTime) const = 0;

    /**
     * Writes into 'output' all the information needed to generate a summary of the current
     * replication state for use by the web interface.
     */
    virtual void summarizeAsHtml(ReplSetHtmlSummary* output) = 0;

    /**
     * Prepares a ReplSetRequestVotesResponse.
     */
    virtual void processReplSetRequestVotes(const ReplSetRequestVotesArgs& args,
                                            ReplSetRequestVotesResponse* response,
                                            const OpTime& lastAppliedOpTime) = 0;

    /**
     * Loads an initial LastVote document, which was read from local storage.
     *
     * Called only during replication startup. All other updates are done internally.
     */
    virtual void loadLastVote(const LastVote& lastVote) = 0;

    /**
     * Readies the TopologyCoordinator for stepdown.
     */
    virtual void prepareForStepDown() = 0;

    /**
     * Updates the current primary index.
     */
    virtual void setPrimaryIndex(long long primaryIndex) = 0;

    /**
     * Transitions to the candidate role if the node is electable.
     */
    virtual Status becomeCandidateIfElectable(const Date_t now, const OpTime& lastOpApplied) = 0;

    /**
     * Updates the storage engine read committed support in the TopologyCoordinator options after
     * creation.
     */
    virtual void setStorageEngineSupportsReadCommitted(bool supported) = 0;

protected:
    TopologyCoordinator() {}
};

/**
 * Type that denotes the role of a node in the replication protocol.
 *
 * The role is distinct from MemberState, in that it only deals with the
 * roles a node plays in the basic protocol -- leader, follower and candidate.
 * The mapping between MemberState and Role is complex -- several MemberStates
 * map to the follower role, and MemberState::RS_SECONDARY maps to either
 * follower or candidate roles, e.g.
 */
class TopologyCoordinator::Role {
public:
    /**
     * Constant indicating leader role.
     */
    static const Role leader;

    /**
     * Constant indicating follower role.
     */
    static const Role follower;

    /**
     * Constant indicating candidate role
     */
    static const Role candidate;

    Role() {}

    bool operator==(Role other) const {
        return _value == other._value;
    }
    bool operator!=(Role other) const {
        return _value != other._value;
    }

    std::string toString() const;

private:
    explicit Role(int value);

    int _value;
};

//
// Convenience method for unittest code. Please use accessors otherwise.
//

std::ostream& operator<<(std::ostream& os, TopologyCoordinator::Role role);

}  // namespace repl
}  // namespace mongo
