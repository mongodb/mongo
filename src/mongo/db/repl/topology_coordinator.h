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
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/server_options.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
class Timestamp;

namespace repl {
class HeartbeatResponseAction;
class MemberData;
class OpTime;
class ReplSetHeartbeatArgs;
class ReplSetConfig;
class TagSubgroup;
struct MemberState;

// Maximum number of retries for a failed heartbeat.
const int kMaxHeartbeatRetries = 2;

/**
 * Replication Topology Coordinator
 *
 * This object is responsible for managing the topology of the cluster.
 * Tasks include consensus and leader election, chaining, and configuration management.
 * Methods of this class should be non-blocking.
 */
class TopologyCoordinator {
    MONGO_DISALLOW_COPYING(TopologyCoordinator);

public:
    /**
     * Type that denotes the role of a node in the replication protocol.
     *
     * The role is distinct from MemberState, in that it only deals with the
     * roles a node plays in the basic protocol -- leader, follower and candidate.
     * The mapping between MemberState and Role is complex -- several MemberStates
     * map to the follower role, and MemberState::RS_SECONDARY maps to either
     * follower or candidate roles, e.g.
     */
    enum class Role { kLeader = 0, kFollower = 1, kCandidate = 2 };

    struct Options {
        // A sync source is re-evaluated after it lags behind further than this amount.
        Seconds maxSyncSourceLagSecs{0};

        // Whether or not this node is running as a config server.
        ClusterRole clusterRole{ClusterRole::None};
    };

    /**
     * Constructs a Topology Coordinator object.
     **/
    TopologyCoordinator(Options options);


    ~TopologyCoordinator();

    /**
     * Different modes a node can be in while still reporting itself as in state PRIMARY.
     *
     * Valid transitions:
     *
     *       kNotLeader <----------------------------------
     *          |                                         |
     *          |                                         |
     *          |                                         |
     *          v                                         |
     *       kLeaderElect-----                            |
     *          |            |                            |
     *          |            |                            |
     *          v            |                            |
     *       kMaster -------------------------            |
     *        |  ^           |                |           |
     *        |  |     -------------------    |           |
     *        |  |     |                 |    |           |
     *        v  |     v                 v    v           |
     *  kAttemptingStepDown----------->kSteppingDown      |
     *        |                              |            |
     *        |                              |            |
     *        |                              |            |
     *        ---------------------------------------------
     *
     */
    enum class LeaderMode {
        kNotLeader,           // This node is not currently a leader.
        kLeaderElect,         // This node has been elected leader, but can't yet accept writes.
        kMaster,              // This node reports ismaster:true and can accept writes.
        kSteppingDown,        // This node is in the middle of a (hb) stepdown that must complete.
        kAttemptingStepDown,  // This node is in the middle of a stepdown (cmd) that might fail.
    };

    ////////////////////////////////////////////////////////////
    //
    // State inspection methods.
    //
    ////////////////////////////////////////////////////////////

    /**
     * Gets the role of this member in the replication protocol.
     */
    Role getRole() const;

    /**
     * Gets the MemberState of this member in the replica set.
     */
    MemberState getMemberState() const;

    /**
     * Returns whether this node should be allowed to accept writes.
     */
    bool canAcceptWrites() const;

    /**
     * Returns true if this node is in the process of stepping down.  Note that this can be
     * due to an unconditional stepdown that must succeed (for instance from learning about a new
     * term) or due to a stepdown attempt that could fail (for instance from a stepdown cmd that
     * could fail if not enough nodes are caught up).
     */
    bool isSteppingDown() const;

    /**
     * Returns the address of the current sync source, or an empty HostAndPort if there is no
     * current sync source.
     */
    HostAndPort getSyncSourceAddress() const;

    /**
     * Retrieves a vector of HostAndPorts containing all nodes that are neither DOWN nor
     * ourself.
     */
    std::vector<HostAndPort> getMaybeUpHostAndPorts() const;

    /**
     * Gets the earliest time the current node will stand for election.
     */
    Date_t getStepDownTime() const;

    /**
     * Gets the current value of the maintenance mode counter.
     */
    int getMaintenanceCount() const;

    /**
     * Gets the latest term this member is aware of. If this member is the primary,
     * it's the current term of the replica set.
     */
    long long getTerm() const;

    enum class UpdateTermResult { kAlreadyUpToDate, kTriggerStepDown, kUpdatedTerm };

    ////////////////////////////////////////////////////////////
    //
    // Basic state manipulation methods.
    //
    ////////////////////////////////////////////////////////////

    /**
     * Sets the latest term this member is aware of to the higher of its current value and
     * the value passed in as "term".
     * Returns the result of setting the term value, or if a stepdown should be triggered.
     */
    UpdateTermResult updateTerm(long long term, Date_t now);

    /**
     * Sets the index into the config used when we next choose a sync source
     */
    void setForceSyncSourceIndex(int index);

    enum class ChainingPreference { kAllowChaining, kUseConfiguration };

    /**
     * Chooses and sets a new sync source, based on our current knowledge of the world.
     */
    HostAndPort chooseNewSyncSource(Date_t now,
                                    const OpTime& lastOpTimeFetched,
                                    ChainingPreference chainingPreference);

    /**
     * Suppresses selecting "host" as sync source until "until".
     */
    void blacklistSyncSource(const HostAndPort& host, Date_t until);

    /**
     * Removes a single entry "host" from the list of potential sync sources which we
     * have blacklisted, if it is supposed to be unblacklisted by "now".
     */
    void unblacklistSyncSource(const HostAndPort& host, Date_t now);

    /**
     * Clears the list of potential sync sources we have blacklisted.
     */
    void clearSyncSourceBlacklist();

    /**
     * Determines if a new sync source should be chosen, if a better candidate sync source is
     * available.  If the current sync source's last optime ("syncSourceLastOpTime" under
     * protocolVersion 1, but pulled from the MemberData in protocolVersion 0) is more than
     * _maxSyncSourceLagSecs behind any syncable source, this function returns true. If we are
     * running in ProtocolVersion 1, our current sync source is not primary, has no sync source
     * ("syncSourceHasSyncSource" is false), and only has data up to "myLastOpTime", returns true.
     *
     * "now" is used to skip over currently blacklisted sync sources.
     *
     * TODO (SERVER-27668): Make OplogQueryMetadata non-optional in mongodb 3.8.
     */
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& replMetadata,
                                boost::optional<rpc::OplogQueryMetadata> oqMetadata,
                                Date_t now) const;

    /**
     * Checks whether we are a single node set and we are not in a stepdown period.  If so,
     * puts us into candidate mode, otherwise does nothing.  This is used to ensure that
     * nodes in a single node replset become primary again when their stepdown period ends.
     */
    bool becomeCandidateIfStepdownPeriodOverAndSingleNodeSet(Date_t now);

    /**
     * Sets the earliest time the current node will stand for election to "newTime".
     *
     * Until this time, while the node may report itself as electable, it will not stand
     * for election.
     */
    void setElectionSleepUntil(Date_t newTime);

    /**
     * Sets the reported mode of this node to one of RS_SECONDARY, RS_STARTUP2, RS_ROLLBACK or
     * RS_RECOVERING, when getRole() == Role::follower.  This is the interface by which the
     * applier changes the reported member state of the current node, and enables or suppresses
     * electability of the current node.  All modes but RS_SECONDARY indicate an unelectable
     * follower state (one that cannot transition to candidate).
     */
    void setFollowerMode(MemberState::MS newMode);

    /**
     * Scan the memberData and determine the highest last applied or last
     * durable optime present on a majority of servers; set _lastCommittedOpTime to this
     * new entry.
     * Whether the last applied or last durable op time is used depends on whether
     * the config getWriteConcernMajorityShouldJournal is set.
     * Returns true if the _lastCommittedOpTime was changed.
     */
    bool updateLastCommittedOpTime();

    /**
     * Updates _lastCommittedOpTime to be "committedOpTime" if it is more recent than the
     * current last committed OpTime.  Returns true if _lastCommittedOpTime is changed.
     */
    bool advanceLastCommittedOpTime(const OpTime& committedOpTime);

    /**
     * Returns the OpTime of the latest majority-committed op known to this server.
     */
    OpTime getLastCommittedOpTime() const;

    /**
     * Returns true if it's safe to transition to LeaderMode::kMaster.
     */
    bool canCompleteTransitionToPrimary(long long termWhenDrainCompleted) const;

    /**
     * Called by the ReplicationCoordinator to signal that we have finished catchup and drain modes
     * and are ready to fully become primary and start accepting writes.
     * "firstOpTimeOfTerm" is a floor on the OpTimes this node will be allowed to consider committed
     * for this tenure as primary. This prevents entries from before our election from counting as
     * committed in our view, until our election (the "firstOpTimeOfTerm" op) has been committed.
     * Returns PrimarySteppedDown if this node is no longer eligible to begin accepting writes.
     */
    Status completeTransitionToPrimary(const OpTime& firstOpTimeOfTerm);

    /**
     * Adjusts the maintenance mode count by "inc".
     *
     * It is an error to call this method if getRole() does not return Role::follower.
     * It is an error to allow the maintenance count to go negative.
     */
    void adjustMaintenanceCountBy(int inc);

    ////////////////////////////////////////////////////////////
    //
    // Methods that prepare responses to command requests.
    //
    ////////////////////////////////////////////////////////////

    // produces a reply to a replSetSyncFrom command
    void prepareSyncFromResponse(const HostAndPort& target,
                                 BSONObjBuilder* response,
                                 Status* result);

    // produce a reply to a replSetFresh command
    void prepareFreshResponse(const ReplicationCoordinator::ReplSetFreshArgs& args,
                              Date_t now,
                              BSONObjBuilder* response,
                              Status* result);

    // produce a reply to a received electCmd
    void prepareElectResponse(const ReplicationCoordinator::ReplSetElectArgs& args,
                              Date_t now,
                              BSONObjBuilder* response,
                              Status* result);

    // produce a reply to a heartbeat
    Status prepareHeartbeatResponse(Date_t now,
                                    const ReplSetHeartbeatArgs& args,
                                    const std::string& ourSetName,
                                    ReplSetHeartbeatResponse* response);

    // produce a reply to a V1 heartbeat
    Status prepareHeartbeatResponseV1(Date_t now,
                                      const ReplSetHeartbeatArgsV1& args,
                                      const std::string& ourSetName,
                                      ReplSetHeartbeatResponse* response);

    struct ReplSetStatusArgs {
        Date_t now;
        unsigned selfUptime;
        const OpTime& readConcernMajorityOpTime;
        const BSONObj& initialSyncStatus;

        // boost::none if the storage engine does not support recovering to a
        // timestamp. Timestamp::min() if a stable checkpoint is yet to be taken.
        const boost::optional<Timestamp> lastStableCheckpointTimestamp;
    };

    // produce a reply to a status request
    void prepareStatusResponse(const ReplSetStatusArgs& rsStatusArgs,
                               BSONObjBuilder* response,
                               Status* result);

    // Produce a replSetUpdatePosition command to be sent to the node's sync source.
    StatusWith<BSONObj> prepareReplSetUpdatePositionCommand(
        OpTime currentCommittedSnapshotOpTime) const;

    // produce a reply to an ismaster request.  It is only valid to call this if we are a
    // replset.
    void fillIsMasterForReplSet(IsMasterResponse* response);

    // Produce member data for the serverStatus command and diagnostic logging.
    void fillMemberData(BSONObjBuilder* result);

    enum class PrepareFreezeResponseResult { kNoAction, kElectSelf };

    /**
     * Produce a reply to a freeze request. Returns a PostMemberStateUpdateAction on success that
     * may trigger state changes in the caller.
     */
    StatusWith<PrepareFreezeResponseResult> prepareFreezeResponse(Date_t now,
                                                                  int secs,
                                                                  BSONObjBuilder* response);

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
    void updateConfig(const ReplSetConfig& newConfig, int selfIndex, Date_t now);

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
    std::pair<ReplSetHeartbeatArgs, Milliseconds> prepareHeartbeatRequest(
        Date_t now, const std::string& ourSetName, const HostAndPort& target);
    std::pair<ReplSetHeartbeatArgsV1, Milliseconds> prepareHeartbeatRequestV1(
        Date_t now, const std::string& ourSetName, const HostAndPort& target);

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
    HeartbeatResponseAction processHeartbeatResponse(
        Date_t now,
        Milliseconds networkRoundTripTime,
        const HostAndPort& target,
        const StatusWith<ReplSetHeartbeatResponse>& hbResponse);

    /**
     *  Returns whether or not at least 'numNodes' have reached the given opTime.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     */
    bool haveNumNodesReachedOpTime(const OpTime& opTime, int numNodes, bool durablyWritten);

    /**
     * Returns whether or not at least one node matching the tagPattern has reached
     * the given opTime.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     */
    bool haveTaggedNodesReachedOpTime(const OpTime& opTime,
                                      const ReplSetTagPattern& tagPattern,
                                      bool durablyWritten);

    /**
     * Returns a vector of members that have applied the operation with OpTime 'op'.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     * "skipSelf" means to exclude this node whether or not the op has been applied.
     */
    std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op, bool durablyWritten);

    /**
     * Marks a member as down from our perspective and returns a bool which indicates if we can no
     * longer see a majority of the nodes and thus should step down.
     */
    bool setMemberAsDown(Date_t now, const int memberIndex);

    /**
     * Goes through the memberData and determines which member that is currently live
     * has the stalest (earliest) last update time.  Returns (-1, Date_t::max()) if there are
     * no other members.
     */
    std::pair<int, Date_t> getStalestLiveMember() const;

    /**
     * Go through the memberData, and mark nodes which haven't been updated
     * recently (within an election timeout) as "down".  Returns a HeartbeatResponseAction, which
     * will be StepDownSelf if we can no longer see a majority of the nodes, otherwise NoAction.
     */
    HeartbeatResponseAction checkMemberTimeouts(Date_t now);

    /**
     * Set all nodes in memberData to not stale with a lastUpdate of "now".
     */
    void resetAllMemberTimeouts(Date_t now);

    /**
     * Set all nodes in memberData that are present in member_set
     * to not stale with a lastUpdate of "now".
     */
    void resetMemberTimeouts(Date_t now, const stdx::unordered_set<HostAndPort>& member_set);

    /*
     * Returns the last optime that this node has applied, whether or not it has been journaled.
     */
    OpTime getMyLastAppliedOpTime() const;

    /*
     * Sets the last optime that this node has applied, whether or not it has been journaled. Fails
     * with an invariant if 'isRollbackAllowed' is false and we're attempting to set the optime
     * backwards. The Date_t 'now' is used to track liveness; setting a node's applied optime
     * updates its liveness information.
     */
    void setMyLastAppliedOpTime(OpTime opTime, Date_t now, bool isRollbackAllowed);

    /*
     * Returns the last optime that this node has applied and journaled.
     */
    OpTime getMyLastDurableOpTime() const;

    /*
     * Sets the last optime that this node has applied and journaled. Fails with an invariant if
     * 'isRollbackAllowed' is false and we're attempting to set the optime backwards. The Date_t
     * 'now' is used to track liveness; setting a node's durable optime updates its liveness
     * information.
     */
    void setMyLastDurableOpTime(OpTime opTime, Date_t now, bool isRollbackAllowed);

    /*
     * Sets the last optimes for a node, other than this node, based on the data from a
     * replSetUpdatePosition command.
     *
     * Returns a Status if the position could not be set, false if the last optimes for the node
     * did not change, or true if either the last applied or last durable optime did change.
     */
    StatusWith<bool> setLastOptime(const UpdatePositionArgs::UpdateInfo& args,
                                   Date_t now,
                                   long long* configVersion);

    /**
     * If getRole() == Role::candidate and this node has not voted too recently, updates the
     * lastVote tracker and returns true.  Otherwise, returns false.
     */
    bool voteForMyself(Date_t now);

    /**
     * Sets lastVote to be for ourself in this term.
     */
    void voteForMyselfV1();

    /**
     * Sets election id and election optime.
     */
    void setElectionInfo(OID electionId, Timestamp electionOpTime);

    /**
     * Performs state updates associated with winning an election.
     *
     * It is an error to call this if the topology coordinator is not in candidate mode.
     *
     * Exactly one of either processWinElection or processLoseElection must be called if
     * processHeartbeatResponse returns StartElection, to exit candidate mode.
     */
    void processWinElection(OID electionId, Timestamp electionOpTime);

    /**
     * Performs state updates associated with losing an election.
     *
     * It is an error to call this if the topology coordinator is not in candidate mode.
     *
     * Exactly one of either processWinElection or processLoseElection must be called if
     * processHeartbeatResponse returns StartElection, to exit candidate mode.
     */
    void processLoseElection();

    /**
     * Readies the TopologyCoordinator for an attempt to stepdown that may fail.  This is used
     * when we receive a stepdown command (which can fail if not enough secondaries are caught up)
     * to ensure that we never process more than one stepdown request at a time.
     * Returns OK if it is safe to continue with the stepdown attempt, or returns:
     * - NotMaster if this node is not a leader.
     * - ConflictingOperationInProgess if this node is already processing a stepdown request of any
     * kind.
     */
    Status prepareForStepDownAttempt();

    /**
     * If this node is still attempting to process a stepdown attempt, aborts the attempt and
     * returns this node to normal primary/master state.  If this node has already completed
     * stepping down or is now in the process of handling an unconditional stepdown, then this
     * method does nothing.
     */
    void abortAttemptedStepDownIfNeeded();

    /**
     * Tries to transition the coordinator from the leader role to the follower role.
     *
     * A step down succeeds based on the following conditions:
     *
     *      C1. 'force' is true and now > waitUntil
     *
     *      C2. A majority set of nodes, M, in the replica set have optimes greater than or
     *      equal to the last applied optime of the primary.
     *
     *      C3. There exists at least one electable secondary node in the majority set M.
     *
     *
     * If C1 is true, or if both C2 and C3 are true, then the stepdown occurs and this method
     * returns true. If the conditions for successful stepdown aren't met yet, but waiting for more
     * time to pass could make it succeed, returns false.  If the whole stepdown attempt should be
     * abandoned (for example because the time limit expired or because we've already stepped down),
     * throws an exception.
     * TODO(spencer): Unify with the finishUnconditionalStepDown() method.
     */
    bool attemptStepDown(
        long long termAtStart, Date_t now, Date_t waitUntil, Date_t stepDownUntil, bool force);

    /**
     * Returns whether it is safe for a stepdown attempt to complete, ignoring the 'force' argument.
     * This is essentially checking conditions C2 and C3 as described in the comment to
     * attemptStepDown().
     */
    bool isSafeToStepDown();

    /**
     * Readies the TopologyCoordinator for stepdown.  Returns false if we're already in the process
     * of an unconditional step down.  If we are in the middle of a stepdown command attempt when
     * this is called then this unconditional stepdown will supersede the stepdown attempt, which
     * will cause the stepdown to fail.  When this returns true it must be followed by a call to
     * finishUnconditionalStepDown() that is called when holding the global X lock.
     */
    bool prepareForUnconditionalStepDown();

    /**
     * Sometimes a request to step down comes in (like via a heartbeat), but we don't have the
     * global exclusive lock so we can't actually stepdown at that moment. When that happens
     * we record that a stepdown request is pending (by calling prepareForUnconditionalStepDown())
     * and schedule work to stepdown in the global X lock.  This method is called after holding the
     * global lock to perform the actual stepdown.
     * TODO(spencer): Unify with the finishAttemptedStepDown() method.
     */
    void finishUnconditionalStepDown();

    /**
     * Considers whether or not this node should stand for election, and returns true
     * if the node has transitioned to candidate role as a result of the call.
     */
    Status checkShouldStandForElection(Date_t now) const;

    /**
     * Set the outgoing heartbeat message from self
     */
    void setMyHeartbeatMessage(const Date_t now, const std::string& s);

    /**
     * Prepares a ReplSetMetadata object describing the current term, primary, and lastOp
     * information.
     */
    rpc::ReplSetMetadata prepareReplSetMetadata(const OpTime& lastVisibleOpTime) const;

    /**
     * Prepares an OplogQueryMetadata object describing the current sync source, rbid, primary,
     * lastOpApplied, and lastOpCommitted.
     */
    rpc::OplogQueryMetadata prepareOplogQueryMetadata(int rbid) const;

    /**
     * Writes into 'output' all the information needed to generate a summary of the current
     * replication state for use by the web interface.
     */
    void summarizeAsHtml(ReplSetHtmlSummary* output);

    /**
     * Prepares a ReplSetRequestVotesResponse.
     */
    void processReplSetRequestVotes(const ReplSetRequestVotesArgs& args,
                                    ReplSetRequestVotesResponse* response);

    /**
     * Loads an initial LastVote document, which was read from local storage.
     *
     * Called only during replication startup. All other updates are done internally.
     */
    void loadLastVote(const LastVote& lastVote);

    /**
     * Updates the current primary index.
     */
    void setPrimaryIndex(long long primaryIndex);

    /**
     * Returns the current primary index.
     */
    int getCurrentPrimaryIndex() const;

    enum StartElectionReason {
        kElectionTimeout,
        kPriorityTakeover,
        kStepUpRequest,
        kCatchupTakeover,
        kSingleNodeStepDownTimeout
    };

    /**
     * Transitions to the candidate role if the node is electable.
     */
    Status becomeCandidateIfElectable(const Date_t now, StartElectionReason reason);

    /**
     * Updates the storage engine read committed support in the TopologyCoordinator options after
     * creation.
     */
    void setStorageEngineSupportsReadCommitted(bool supported);

    /**
     * Reset the booleans to record the last heartbeat restart.
     */
    void restartHeartbeats();

    /**
     * Scans through all members that are 'up' and return the latest known optime, if we have
     * received (successful or failed) heartbeats from all nodes since heartbeat restart.
     *
     * Returns boost::none if any node hasn't responded to a heartbeat since we last restarted
     * heartbeats.
     * Returns OpTime(Timestamp(0, 0), 0), the smallest OpTime in PV1, if other nodes are all down.
     */
    boost::optional<OpTime> latestKnownOpTimeSinceHeartbeatRestart() const;

    ////////////////////////////////////////////////////////////
    //
    // Test support methods
    //
    ////////////////////////////////////////////////////////////

    // Changes _memberState to newMemberState.  Only for testing.
    void changeMemberState_forTest(const MemberState& newMemberState,
                                   const Timestamp& electionTime = Timestamp(0, 0));

    // Sets "_electionTime" to "newElectionTime".  Only for testing.
    void _setElectionTime(const Timestamp& newElectionTime);

    // Sets _currentPrimaryIndex to the given index.  Should only be used in unit tests!
    // TODO(spencer): Remove this once we can easily call for an election in unit tests to
    // set the current primary.
    void setCurrentPrimary_forTest(int primaryIndex,
                                   const Timestamp& electionTime = Timestamp(0, 0));

    // Returns _electionTime.  Only used in unittests.
    Timestamp getElectionTime() const;

    // Returns _electionId.  Only used in unittests.
    OID getElectionId() const;

    // Returns the name for a role.  Only used in unittests.
    static std::string roleToString(TopologyCoordinator::Role role);

private:
    typedef int UnelectableReasonMask;
    class PingStats;

    enum UnelectableReason {
        None = 0,
        CannotSeeMajority = 1 << 0,
        NotCloseEnoughToLatestOptime = 1 << 1,
        ArbiterIAm = 1 << 2,
        NotSecondary = 1 << 3,
        NoPriority = 1 << 4,
        StepDownPeriodActive = 1 << 5,
        NoData = 1 << 6,
        NotInitialized = 1 << 7,
        VotedTooRecently = 1 << 8,
        RefusesToStand = 1 << 9,
        NotCloseEnoughToLatestForPriorityTakeover = 1 << 10,
        NotFreshEnoughForCatchupTakeover = 1 << 11,
    };

    // Set what type of PRIMARY this node currently is.
    void _setLeaderMode(LeaderMode mode);

    // Returns the number of heartbeat pings which have occurred.
    int _getTotalPings();

    // Returns the current "ping" value for the given member by their address
    Milliseconds _getPing(const HostAndPort& host);

    // Determines if we will veto the member specified by "args.id".
    // If we veto, the errmsg will be filled in with a reason
    bool _shouldVetoMember(const ReplicationCoordinator::ReplSetFreshArgs& args,
                           const Date_t& now,
                           std::string* errmsg) const;

    // Returns the index of the member with the matching id, or -1 if none match.
    int _getMemberIndex(int id) const;

    // Sees if a majority number of votes are held by members who are currently "up"
    bool _aMajoritySeemsToBeUp() const;

    // Checks if the node can see a healthy primary of equal or greater priority to the
    // candidate. If so, returns the index of that node. Otherwise returns -1.
    int _findHealthyPrimaryOfEqualOrGreaterPriority(const int candidateIndex) const;

    // Is otherOpTime close enough (within 10 seconds) to the latest known optime to qualify
    // for an election
    bool _isOpTimeCloseEnoughToLatestToElect(const OpTime& otherOpTime) const;

    // Is our optime close enough to the latest known optime to call for a priority takeover.
    bool _amIFreshEnoughForPriorityTakeover() const;

    // Is the primary node still in catchup mode and is our optime the latest
    // known optime of all the up nodes.
    bool _amIFreshEnoughForCatchupTakeover() const;

    // Returns reason why "self" member is unelectable
    UnelectableReasonMask _getMyUnelectableReason(const Date_t now,
                                                  StartElectionReason reason) const;

    // Returns reason why memberIndex is unelectable
    UnelectableReasonMask _getUnelectableReason(int memberIndex) const;

    // Returns the nice text of why the node is unelectable
    std::string _getUnelectableReasonString(UnelectableReasonMask ur) const;

    // Return true if we are currently primary
    bool _iAmPrimary() const;

    // Scans through all members that are 'up' and return the latest known optime.
    OpTime _latestKnownOpTime() const;

    // Scans the electable set and returns the highest priority member index
    int _getHighestPriorityElectableIndex(Date_t now) const;

    // Returns true if "one" member is higher priority than "two" member
    bool _isMemberHigherPriority(int memberOneIndex, int memberTwoIndex) const;

    // Helper shortcut to self config
    const MemberConfig& _selfConfig() const;

    // Helper shortcut to self member data for const members.
    const MemberData& _selfMemberData() const;

    // Helper shortcut to self member data for non-const members.
    MemberData& _selfMemberData();

    // Index of self member in member data.
    const int _selfMemberDataIndex() const;

    /*
     * Returns information we have on the state of the node identified by memberId.  Returns
     * nullptr if memberId is not found in the configuration.
     */
    MemberData* _findMemberDataByMemberId(const int memberId);

    // Returns NULL if there is no primary, or the MemberConfig* for the current primary
    const MemberConfig* _currentPrimaryMember() const;

    /**
     * Performs updating "_currentPrimaryIndex" for processHeartbeatResponse(), and determines if an
     * election or stepdown should commence.
     * _updatePrimaryFromHBDataV1() is a simplified version of _updatePrimaryFromHBData() to be used
     * when in ProtocolVersion1.
     */
    HeartbeatResponseAction _updatePrimaryFromHBData(int updatedConfigIndex,
                                                     const MemberState& originalState,
                                                     Date_t now);
    HeartbeatResponseAction _updatePrimaryFromHBDataV1(int updatedConfigIndex,
                                                       const MemberState& originalState,
                                                       Date_t now);

    /**
     * Updates _memberData based on the newConfig, ensuring that every member in the newConfig
     * has an entry in _memberData.  If any nodes in the newConfig are also present in
     * _currentConfig, copies their heartbeat info into the corresponding entry in the updated
     * _memberData vector.
     */
    void _updateHeartbeatDataForReconfig(const ReplSetConfig& newConfig, int selfIndex, Date_t now);

    /**
     * Returns whether a stepdown attempt should be allowed to proceed.  See the comment for
     * attemptStepDown() for more details on the rules of when stepdown attempts succeed or fail.
     */
    bool _canCompleteStepDownAttempt(Date_t now, Date_t waitUntil, bool force);

    void _stepDownSelfAndReplaceWith(int newPrimary);

    /**
     * Looks up the provided member in the blacklist and returns true if the member's blacklist
     * expire time is after 'now'.  If the member is found but the expire time is before 'now',
     * the function returns false.  If the member is not found in the blacklist, the function
     * returns false.
     **/
    bool _memberIsBlacklisted(const MemberConfig& memberConfig, Date_t now) const;

    /**
     * Returns true if we are a one-node replica set, we're the one member,
     * we're electable, we're not in maintenance mode, and we are currently in followerMode
     * SECONDARY.
     *
     * This is used to decide if we should transition to Role::candidate in a one-node replica set.
     */
    bool _isElectableNodeInSingleNodeReplicaSet() const;

    // This node's role in the replication protocol.
    Role _role;

    // This is a unique id that is generated and set each time we transition to PRIMARY, as the
    // result of an election.
    OID _electionId;
    // The time at which the current PRIMARY was elected.
    Timestamp _electionTime;

    // This node's election term.  The term is used as part of the consensus algorithm to elect
    // and maintain one primary (leader) node in the cluster.
    long long _term;

    // the index of the member we currently believe is primary, if one exists, otherwise -1
    int _currentPrimaryIndex;

    // the hostandport we are currently syncing from
    // empty if no sync source (we are primary, or we cannot connect to anyone yet)
    HostAndPort _syncSource;
    // These members are not chosen as sync sources for a period of time, due to connection
    // issues with them
    std::map<HostAndPort, Date_t> _syncSourceBlacklist;
    // The next sync source to be chosen, requested via a replSetSyncFrom command
    int _forceSyncSourceIndex;

    // Options for this TopologyCoordinator
    Options _options;

    // "heartbeat message"
    // sent in requestHeartbeat respond in field "hbm"
    std::string _hbmsg;
    Date_t _hbmsgTime;  // when it was logged

    // heartbeat msg to send to others; descriptive diagnostic info
    std::string _getHbmsg(Date_t now) const;

    int _selfIndex;  // this node's index in _members and _currentConfig

    ReplSetConfig _rsConfig;  // The current config, including a vector of MemberConfigs

    // Heartbeat, current applied/durable optime, and other state data for each member.  It is
    // guaranteed that this vector will be maintained in the same order as the MemberConfigs in
    // _currentConfig, therefore the member config index can be used to index into this vector as
    // well.
    std::vector<MemberData> _memberData;

    // Time when stepDown command expires
    Date_t _stepDownUntil;

    // A time before which this node will not stand for election.
    // In protocol version 1, this is used to prevent running for election after seeing
    // a new term.
    Date_t _electionSleepUntil;

    // OpTime of the latest committed operation.
    OpTime _lastCommittedOpTime;

    // OpTime representing our transition to PRIMARY and the start of our term.
    // _lastCommittedOpTime cannot be set to an earlier OpTime.
    OpTime _firstOpTimeOfMyTerm;

    // The number of calls we have had to enter maintenance mode
    int _maintenanceModeCalls;

    // The sub-mode of follower that we are in.  Legal values are RS_SECONDARY, RS_RECOVERING,
    // RS_STARTUP2 (initial sync) and RS_ROLLBACK.  Only meaningful if _role == Role::follower.
    // Configured via setFollowerMode().  If the sub-mode is RS_SECONDARY, then the effective
    // sub-mode is either RS_SECONDARY or RS_RECOVERING, depending on _maintenanceModeCalls.
    // Rather than accesing this variable direclty, one should use the getMemberState() method,
    // which computes the replica set node state on the fly.
    MemberState::MS _followerMode;

    // What type of PRIMARY this node currently is.  Don't set this directly, call _setLeaderMode
    // instead.
    LeaderMode _leaderMode = LeaderMode::kNotLeader;

    typedef std::map<HostAndPort, PingStats> PingMap;
    // Ping stats for each member by HostAndPort;
    PingMap _pings;

    // Last vote info from the election
    struct VoteLease {
        static const Seconds leaseTime;

        Date_t when;
        int whoId = -1;
        HostAndPort whoHostAndPort;
    } _voteLease;

    // V1 last vote info for elections
    LastVote _lastVote{OpTime::kInitialTerm, -1};

    enum class ReadCommittedSupport {
        kUnknown,
        kNo,
        kYes,
    };

    // Whether or not the storage engine supports read committed.
    ReadCommittedSupport _storageEngineSupportsReadCommitted{ReadCommittedSupport::kUnknown};
};

/**
 * A PingStats object stores data about heartbeat attempts to a particular target node. Over the
 * course of its lifetime, it may be used for multiple rounds of heartbeats. This allows for the
 * collection of statistics like average heartbeat latency to a target. The heartbeat latency
 * measurement it stores for each replica set member is an average weighted 80% to the old value,
 * and 20% to the new value.
 *
 */
class TopologyCoordinator::PingStats {
public:
    /**
     * Starts a new round of heartbeat attempts by transitioning to 'TRYING' and resetting the
     * failure count. Also records that a new heartbeat request started at "now".
     */
    void start(Date_t now);

    /**
     * Records that a heartbeat request completed successfully, and that "millis" milliseconds
     * were spent for a single network roundtrip plus remote processing time.
     */
    void hit(Milliseconds millis);

    /**
     * Records that a heartbeat request failed.
     */
    void miss();

    /**
     * Gets the number of hit() calls.
     */
    unsigned int getCount() const {
        return hitCount;
    }

    /**
     * Gets the weighted average round trip time for heartbeat messages to the target.
     * Returns 0 if there have been no pings recorded yet.
     */
    Milliseconds getMillis() const {
        return averagePingTimeMs == UninitializedPingTime ? Milliseconds(0) : averagePingTimeMs;
    }

    /**
     * Gets the date at which start() was last called, which is used to determine if
     * a heartbeat should be retried or if the time limit has expired.
     */
    Date_t getLastHeartbeatStartDate() const {
        return _lastHeartbeatStartDate;
    }

    /**
     * Returns true if the number of failed heartbeats for the most recent round of attempts has
     * exceeded the max number of heartbeat retries.
     */
    bool failed() const {
        return _state == FAILED;
    }

    /**
     * Returns true if a good heartbeat has been received for the most recent round of heartbeat
     * attempts before the maximum number of retries has been exceeded. Returns false otherwise.
     */
    bool succeeded() const {
        return _state == SUCCEEDED;
    }

    /**
     * Returns true if a heartbeat attempt is currently in progress and there are still retries
     * left.
     */
    bool trying() const {
        return _state == TRYING;
    }

    /**
     * Returns true if 'start' has never been called on this instance of PingStats. Otherwise
     * returns false.
     */
    bool uninitialized() const {
        return _state == UNINITIALIZED;
    }

    /**
     * Gets the number of retries left for this heartbeat attempt. Invalid to call if the current
     * state is 'UNINITIALIZED'.
    */
    int retriesLeft() const {
        return kMaxHeartbeatRetries - _numFailuresSinceLastStart;
    }

private:
    /**
     * Represents the current state of this PingStats object.
     *
     * At creation time, a PingStats object is in the 'UNINITIALIZED' state, and will remain so
     * until the first heartbeat attempt is initiated. Heartbeat attempts are initiated by calls to
     * 'start', which puts the object into 'TRYING' state. If all heartbeat retries are used up
     * before receiving a good response, it will enter the 'FAILED' state. If a good heartbeat
     * response is received before exceeding the maximum number of retries, the object enters the
     * 'SUCCEEDED' state. From either the 'SUCCEEDED' or 'FAILED' state, the object can go back into
     * 'TRYING' state, to begin a new heartbeat attempt. The following is a simple state transition
     * table illustrating this behavior:
     *
     * UNINITIALIZED:   [TRYING]
     * TRYING:          [SUCCEEDED, FAILED]
     * SUCCEEDED:       [TRYING]
     * FAILED:          [TRYING]
     *
     */
    enum HeartbeatState { UNINITIALIZED, TRYING, SUCCEEDED, FAILED };

    // The current state of this PingStats object.
    HeartbeatState _state = UNINITIALIZED;

    // Represents the uninitialized value of a counter that should only ever be >=0 after
    // initialization.
    static constexpr int UninitializedCount{-1};

    // The value of 'averagePingTimeMs' before any good heartbeats have been received.
    static constexpr Milliseconds UninitializedPingTime{UninitializedCount};

    // The number of successful heartbeats that have ever been received i.e. the total number of
    // calls to 'PingStats::hit'.
    unsigned int hitCount = 0;

    // The running, weighted average round trip time for heartbeat messages to the target node.
    // Weighted 80% to the old round trip ping time, and 20% to the new round trip ping time.
    Milliseconds averagePingTimeMs = UninitializedPingTime;

    // The time of the most recent call to 'PingStats::start'.
    Date_t _lastHeartbeatStartDate;

    // The number of failed heartbeat attempts since the most recent call to 'PingStats::start'.
    int _numFailuresSinceLastStart = UninitializedCount;
};

//
// Convenience method for unittest code. Please use accessors otherwise.
//

std::ostream& operator<<(std::ostream& os, TopologyCoordinator::Role role);
std::ostream& operator<<(std::ostream& os, TopologyCoordinator::PrepareFreezeResponseResult result);

}  // namespace repl
}  // namespace mongo
