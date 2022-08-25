/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <functional>
#include <iosfwd>
#include <queue>
#include <string>

#include "mongo/client/read_preference.h"
#include "mongo/db/repl/hello_response.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/repl_set_heartbeat_args_v1.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_set_request_votes_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_metrics_gen.h"
#include "mongo/db/repl/split_horizon.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/db/server_options.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
class CommitQuorumOptions;
class Timestamp;

namespace repl {
class HeartbeatResponseAction;
class MemberData;
class OpTime;
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
    TopologyCoordinator(const TopologyCoordinator&) = delete;
    TopologyCoordinator& operator=(const TopologyCoordinator&) = delete;

public:
    /**
     * RecentSyncSourceChanges stores the times that recent sync source changes happened. It will
     * maintain a max size of maxSyncSourceChangesPerHour. If any additional entries are added,
     * older entries will be removed. It is used to restrict the number of sync source changes that
     * happen per hour when the node already has a valid sync source.
     */
    class RecentSyncSourceChanges {
    public:
        /**
         * Checks if all the entries occurred within the last hour or not. It will remove additional
         * entries if it sees that there are more than maxSyncSourceChangesPerHour entries. If there
         * are fewer than maxSyncSourceChangesPerHour entries, it returns false.
         */
        bool changedTooOftenRecently(Date_t now);

        /**
         * Adds a new entry. It will remove additional entries if it sees that there are more than
         * maxSyncSourceChangesPerHour entries. This should only be called if the sync source was
         * changed to another node, not if the sync source was cleared.
         */
        void addNewEntry(Date_t now);

        /**
         * Return the underlying queue. Used for testing purposes only.
         */
        std::queue<Date_t> getChanges_forTest();

    private:
        std::queue<Date_t> _recentChanges;
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
     * Gets the current topology version of this member.
     */
    TopologyVersion getTopologyVersion() const;

    /**
     * Gets the MemberState of this member in the replica set.
     */
    MemberState getMemberState() const;

    /**
     * Returns the replica set's MemberData.
     */
    std::vector<MemberData> getMemberData() const;

    /**
     * Returns whether this node should be allowed to accept writes.
     */
    bool canAcceptWrites() const;

    /**
     * Returns true if this node is in the process of stepping down unconditionally.
     */
    bool isSteppingDownUnconditionally() const;

    /**
     * Returns true if this node is in the process of stepping down either conditionally or
     * unconditionally. Note that this can be due to an unconditional stepdown that must
     * succeed (for instance from learning about a new term) or due to a stepdown attempt
     * that could fail (for instance from a stepdown cmd that could fail if not enough nodes
     * are caught up).
     */
    bool isSteppingDown() const;

    /**
     * Returns the address of the current sync source, or an empty HostAndPort if there is no
     * current sync source.
     */
    HostAndPort getSyncSourceAddress() const;

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

    /**
     * Returns true if we are a one-node replica set, we're the one member,
     * we're electable, we're not in maintenance mode, and we are currently in followerMode
     * SECONDARY.
     *
     * This is used to decide if we should start an election in a one-node replica set.
     */
    bool isElectableNodeInSingleNodeReplicaSet() const;


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

    /**
     * Chooses and sets a new sync source, based on our current knowledge of the world.
     * If readPreference is PrimaryOnly, only the primary will be selected.
     */
    HostAndPort chooseNewSyncSource(Date_t now,
                                    const OpTime& lastOpTimeFetched,
                                    ReadPreference readPreference);

    /**
     * Suppresses selecting "host" as sync source until "until".
     */
    void denylistSyncSource(const HostAndPort& host, Date_t until);

    /**
     * Removes a single entry "host" from the list of potential sync sources which we
     * have denylisted, if it is supposed to be undenylisted by "now".
     */
    void undenylistSyncSource(const HostAndPort& host, Date_t now);

    /**
     * Clears the list of potential sync sources we have denylisted.
     */
    void clearSyncSourceDenylist();

    /**
     * Determines if a new sync source should be chosen, if a better candidate sync source is
     * available.  If the current sync source's last optime ("syncSourceLastOpTime" under
     * protocolVersion 1, but pulled from the MemberData in protocolVersion 0) is more than
     * _maxSyncSourceLagSecs behind any syncable source, this function returns true. If we are
     * running in ProtocolVersion 1, our current sync source is not primary, has no sync source
     * ("syncSourceHasSyncSource" is false), and only has data up to "myLastOpTime", returns true.
     *
     * "now" is used to skip over currently denylisted sync sources.
     */
    bool shouldChangeSyncSource(const HostAndPort& currentSource,
                                const rpc::ReplSetMetadata& replMetadata,
                                const rpc::OplogQueryMetadata& oqMetadata,
                                const OpTime& lastOpTimeFetched,
                                Date_t now) const;

    /**
     * Determines if a new sync source should be chosen when an error occurs. In this case
     * we do not have current metadata from the sync source and so can only do a subset of
     * the checks we do when we get a response.
     */
    bool shouldChangeSyncSourceOnError(const HostAndPort& currentSource,
                                       const OpTime& lastOpTimeFetched,
                                       Date_t now) const;
    /**
     * Returns true if we find an eligible sync source that is significantly closer than our current
     * sync source.
     */
    bool shouldChangeSyncSourceDueToPingTime(const HostAndPort& currentSource,
                                             const MemberState& memberState,
                                             const OpTime& previousOpTimeFetched,
                                             Date_t now,
                                             ReadPreference readPreference);

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
    bool updateLastCommittedOpTimeAndWallTime();

    /**
     * Updates _lastCommittedOpTime to be 'committedOpTime' if it is more recent than the current
     * last committed OpTime.  Returns true if _lastCommittedOpTime is changed. We ignore
     * 'committedOpTime' if it has a different term than our lastApplied, unless
     * 'fromSyncSource'=true, which guarantees we are on the same branch of history as
     * 'committedOpTime', so we update our commit point to min(committedOpTime, lastApplied).
     * The 'forInitiate' flag is to force-advance our committedOpTime during the execution of
     * the replSetInitiate command.
     */
    bool advanceLastCommittedOpTimeAndWallTime(OpTimeAndWallTime committedOpTimeAndWallTime,
                                               bool fromSyncSource,
                                               bool forInitiate = false);

    /**
     * Resets the commit point to the provided opTime, with a wall time of now.
     */
    void resetLastCommittedOpTime(const OpTime& lastCommittedOpTime);

    /**
     * Returns the OpTime of the latest majority-committed op known to this server.
     */
    OpTime getLastCommittedOpTime() const;

    OpTimeAndWallTime getLastCommittedOpTimeAndWallTime() const;

    /**
     * Returns true if it's safe to transition to LeaderMode::kWritablePrimary.
     */
    bool canCompleteTransitionToPrimary(long long termWhenDrainCompleted) const;

    /**
     * Called by the ReplicationCoordinator to signal that we have finished catchup and drain modes
     * and are ready to fully become primary and start accepting writes.
     * "firstOpTimeOfTerm" is a floor on the OpTimes this node will be allowed to consider committed
     * for this tenure as primary. This prevents entries from before our election from counting as
     * committed in our view, until our election (the "firstOpTimeOfTerm" op) has been committed.
     */
    void completeTransitionToPrimary(const OpTime& firstOpTimeOfTerm);

    /**
     * Adjusts the maintenance mode count by "inc".
     *
     * It is an error to call this method if getRole() does not return Role::follower.
     * It is an error to allow the maintenance count to go negative.
     */
    void adjustMaintenanceCountBy(int inc);

    /**
     * Sets the value of the maintenance mode counter to 0.
     */
    void resetMaintenanceCount();

    ////////////////////////////////////////////////////////////
    //
    // Methods that prepare responses to command requests.
    //
    ////////////////////////////////////////////////////////////

    // produces a reply to a replSetSyncFrom command
    void prepareSyncFromResponse(const HostAndPort& target,
                                 BSONObjBuilder* response,
                                 Status* result);

    // produce a reply to a V1 heartbeat, and return whether the remote node's config has changed.
    StatusWith<bool> prepareHeartbeatResponseV1(Date_t now,
                                                const ReplSetHeartbeatArgsV1& args,
                                                StringData ourSetName,
                                                ReplSetHeartbeatResponse* response);

    struct ReplSetStatusArgs {
        const Date_t now;
        const unsigned selfUptime;
        const OpTime readConcernMajorityOpTime;
        const BSONObj initialSyncStatus;
        const BSONObj electionCandidateMetrics;
        const BSONObj electionParticipantMetrics;

        // boost::none if the storage engine does not support recovery to a timestamp, or if the
        // storage engine is not available.
        // Timestamp::min() if a stable recovery timestamp is yet to be taken.
        //
        // On the replication layer, a non-min() timestamp ensures recoverable rollback is possible,
        // as well as startup recovery without re-initial syncing in the case of durable storage
        // engines.
        const boost::optional<Timestamp> lastStableRecoveryTimestamp;
        bool tooStale;
    };

    // produce a reply to a status request
    void prepareStatusResponse(const ReplSetStatusArgs& rsStatusArgs,
                               BSONObjBuilder* response,
                               Status* result);

    // Produce a replSetUpdatePosition command to be sent to the node's sync source.
    StatusWith<BSONObj> prepareReplSetUpdatePositionCommand(
        OpTime currentCommittedSnapshotOpTime) const;

    // Produce a reply to a hello request.  It is only valid to call this if we are a
    // replset.  Drivers interpret the hello fields according to the Server Discovery and
    // Monitoring Spec, see the "Parsing an isMaster response" section.
    void fillHelloForReplSet(std::shared_ptr<HelloResponse> response,
                             const StringData& horizonString) const;

    // Produce member data for the serverStatus command and diagnostic logging.
    void fillMemberData(BSONObjBuilder* result);

    enum class PrepareFreezeResponseResult { kNoAction, kSingleNodeSelfElect };

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
    std::pair<ReplSetHeartbeatArgsV1, Milliseconds> prepareHeartbeatRequestV1(
        Date_t now, StringData ourSetName, const HostAndPort& target);

    /**
     * Processes a heartbeat response from "target" that arrived around "now", having spent
     * "networkRoundTripTime" millis on the network.
     *
     * Updates internal topology coordinator state, and returns instructions about what action
     * to take next.
     *
     * If the next action indicates "StepDownSelf", the topology coordinator has transitioned
     * to the "follower" role from "leader", and the caller should take any necessary actions
     * to become a follower.
     *
     * If the next action indicates "Reconfig", the caller should verify the configuration in
     * hbResponse is acceptable, perform any other reconfiguration actions it must, and call
     * updateConfig with the new configuration and the appropriate value for "selfIndex".  It
     * must also wrap up any outstanding elections (by calling processLoseElection or
     * processWinElection) before calling updateConfig.
     *
     * This call should be paired (with intervening network communication) with a call to
     * prepareHeartbeatRequestV1 for the same "target".
     */
    HeartbeatResponseAction processHeartbeatResponse(
        Date_t now,
        Milliseconds networkRoundTripTime,
        const HostAndPort& target,
        const StatusWith<ReplSetHeartbeatResponse>& hbResponse);

    /**
     *  Returns whether or not at least 'numNodes' have reached the given opTime with the same term.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     */
    bool haveNumNodesReachedOpTime(const OpTime& opTime, int numNodes, bool durablyWritten);

    /**
     * Returns whether or not at least one node matching the tagPattern has reached the given opTime
     * with the same term.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     */
    bool haveTaggedNodesReachedOpTime(const OpTime& opTime,
                                      const ReplSetTagPattern& tagPattern,
                                      bool durablyWritten);

    using MemberPredicate = std::function<bool(const MemberData&)>;

    /**
     * Return the predicate that tests if a member has reached the target OpTime.
     */
    MemberPredicate makeOpTimePredicate(const OpTime& opTime, bool durablyWritten);

    /**
     * Return the predicate that tests if a member has replicated the given config.
     */
    MemberPredicate makeConfigPredicate();

    /**
     * Returns whether or not at least one node matching the tagPattern has satisfied the given
     * condition.
     */
    bool haveTaggedNodesSatisfiedCondition(MemberPredicate pred,
                                           const ReplSetTagPattern& tagPattern);

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
    bool setMemberAsDown(Date_t now, int memberIndex);

    /**
     * Goes through the memberData and determines which member that is currently live
     * has the stalest (earliest) last update time.  Returns (MemberId(), Date_t::max()) if there
     * are no other members.
     */
    std::pair<MemberId, Date_t> getStalestLiveMember() const;

    /**
     * Go through the memberData, and mark nodes which haven't been updated
     * recently (within an election timeout) as "down".  Returns a HeartbeatResponseAction, which
     * will be StepDownSelf if we can no longer see a majority of the nodes, otherwise NoAction.
     */
    HeartbeatResponseAction checkMemberTimeouts(Date_t now);

    /**
     * Set all nodes in memberData that are present in member_set
     * to not stale with a lastUpdate of "now".
     */
    void resetMemberTimeouts(Date_t now, const stdx::unordered_set<HostAndPort>& member_set);

    /*
     * Returns the last optime that this node has applied, whether or not it has been journaled.
     */
    OpTime getMyLastAppliedOpTime() const;
    OpTimeAndWallTime getMyLastAppliedOpTimeAndWallTime() const;

    /*
     * Sets the last optime that this node has applied, whether or not it has been journaled. Fails
     * with an invariant if 'isRollbackAllowed' is false and we're attempting to set the optime
     * backwards. The Date_t 'now' is used to track liveness; setting a node's applied optime
     * updates its liveness information.
     */
    void setMyLastAppliedOpTimeAndWallTime(OpTimeAndWallTime opTimeAndWallTime,
                                           Date_t now,
                                           bool isRollbackAllowed);

    /*
     * Returns the last optime that this node has applied and journaled.
     */
    OpTime getMyLastDurableOpTime() const;
    OpTimeAndWallTime getMyLastDurableOpTimeAndWallTime() const;

    /*
     * Sets the last optime that this node has applied and journaled. Fails with an invariant if
     * 'isRollbackAllowed' is false and we're attempting to set the optime backwards. The Date_t
     * 'now' is used to track liveness; setting a node's durable optime updates its liveness
     * information.
     */
    void setMyLastDurableOpTimeAndWallTime(OpTimeAndWallTime opTimeAndWallTime,
                                           Date_t now,
                                           bool isRollbackAllowed);

    /*
     * Sets the last optimes for a node, other than this node, based on the data from a
     * replSetUpdatePosition command.
     *
     * Returns a Status if the position could not be set, false if the last optimes for the node
     * did not change, or true if either the last applied or last durable optime did change.
     */
    StatusWith<bool> setLastOptimeForMember(const UpdatePositionArgs::UpdateInfo& args, Date_t now);

    /**
     * Sets the latest optime committed in the previous config to the current lastCommitted optime.
     */
    void updateLastCommittedInPrevConfig();

    /**
     * Returns the latest optime committed in the previous config.
     */
    OpTime getLastCommittedInPrevConfig();

    /**
     * Returns an optime that must become majority committed in the current config before it is safe
     * for a primary to move to a new config.
     */
    OpTime getConfigOplogCommitmentOpTime();

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


    using StepDownAttemptAbortFn = std::function<void()>;
    /**
     * Readies the TopologyCoordinator for an attempt to stepdown that may fail.  This is used
     * when we receive a stepdown command (which can fail if not enough secondaries are caught up)
     * to ensure that we never process more than one stepdown request at a time.
     * Returns OK if it is safe to continue with the stepdown attempt, or returns:
     * - NotWritablePrimary if this node is not a leader.
     * - ConflictingOperationInProgess if this node is already processing a stepdown request of any
     * kind.
     * On an OK return status also returns a function object that can be called to abort the
     * pending stepdown attempt and return this node to normal (writable) primary state.
     */
    StatusWith<StepDownAttemptAbortFn> prepareForStepDownAttempt();

    /**
     * Tries to transition the coordinator's leader mode from kAttemptingStepDown to
     * kSteppingDown only if we are able to meet the below requirements for stepdown.
     *
     *      C1. 'force' is true and now > waitUntil
     *
     *      C2. A majority set of nodes, M, in the replica set have optimes greater than or
     *      equal to the last applied optime of the primary.
     *
     *      C3. There exists at least one electable secondary node in the majority set M.
     *
     * If C1 is true, or if both C2 and C3 are true, then the transition succeeds and this
     * method returns true. If the conditions for successful stepdown aren't met yet, but waiting
     * for more time to pass could make it succeed, returns false.  If the whole stepdown attempt
     * should be abandoned (for example because the time limit expired or because we've already
     * stepped down), throws an exception.
     * TODO(spencer): Unify with the finishUnconditionalStepDown() method.
     */
    bool tryToStartStepDown(
        long long termAtStart, Date_t now, Date_t waitUntil, Date_t stepDownUntil, bool force);

    /**
     * Returns whether it is safe for a stepdown attempt to complete, ignoring the 'force' argument.
     * This is essentially checking conditions C2 and C3 as described in the comment to
     * tryToStartStepDown().
     */
    bool isSafeToStepDown();

    /**
     * Readies the TopologyCoordinator for stepdown.  Returns false if we're already in the process
     * of an unconditional step down.  If we are in the middle of a stepdown command attempt when
     * this is called then this unconditional stepdown will supersede the stepdown attempt, which
     * will cause the stepdown to fail.  When this returns true, step down via heartbeat and
     * reconfig should call finishUnconditionalStepDown() and updateConfig respectively by holding
     * the RSTL lock in X mode.
     *
     * An unconditional step down can be caused due to below reasons.
     *     1) Learning new term via heartbeat.
     *     2) Liveness timeout.
     *     3) Force reconfig command.
     *     4) Force reconfig via heartbeat.
     * At most 2 operations can be in the middle of unconditional step down. And, out of 2
     * operations, one should be due to reason #1 or #2 and other should be due to reason #3 or #4,
     * in which case only one succeeds in stepping down and other does nothing.
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
     * Returns the index of the most suitable candidate for an election handoff. The node must be
     * caught up and electable. Ties are resolved first by highest priority, then by lowest member
     * id.
     */
    int chooseElectionHandoffCandidate();

    /**
     * Set the outgoing heartbeat message from self
     */
    void setMyHeartbeatMessage(Date_t now, const std::string& s);

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

    /**
     * Transitions to the candidate role if the node is electable.
     */
    Status becomeCandidateIfElectable(Date_t now, StartElectionReasonEnum reason);

    /**
     * Updates the storage engine read committed support in the TopologyCoordinator options after
     * creation.
     */
    void setStorageEngineSupportsReadCommitted(bool supported);

    /**
     * Reset the booleans to record the last heartbeat restart for the target node.
     */
    void restartHeartbeat(Date_t now, const HostAndPort& target);

    /**
     * Increments the counter field of the current TopologyVersion.
     */
    void incrementTopologyVersion();

    // Scans through all members that are 'up' and returns the latest known optime.
    OpTime latestKnownOpTime() const;

    /**
     * Scans through all members that are 'up' and return the latest known optime, if we have
     * received (successful or failed) heartbeats from all nodes since heartbeat restart.
     *
     * Returns boost::none if any node hasn't responded to a heartbeat since we last restarted
     * heartbeats.
     * Returns OpTime(Timestamp(0, 0), 0), the smallest OpTime in PV1, if other nodes are all down.
     */
    boost::optional<OpTime> latestKnownOpTimeSinceHeartbeatRestart() const;

    /**
     * Similar to latestKnownOpTimeSinceHeartbeatRestart(), but returns the latest known optime for
     * each member in the config. If the member is not up or hasn't responded to a heartbeat since
     * we last restarted, then its value will be boost::none.
     */
    std::map<MemberId, boost::optional<OpTime>> latestKnownOpTimeSinceHeartbeatRestartPerMember()
        const;

    /**
     * Checks if the 'commitQuorum' can be satisifed by the current replica set config. Returns an
     * OK Status if it can be satisfied, and an error otherwise.
     */
    Status checkIfCommitQuorumCanBeSatisfied(const CommitQuorumOptions& commitQuorum) const;

    /**
     * Returns nullptr if there is no primary, or the MemberConfig* for the current primary.
     */
    const MemberConfig* getCurrentPrimaryMember() const;

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

    /**
     * Get a raw pointer to the list of recent sync source changes. It is the caller's
     * responsibility to not use this pointer beyond the lifetime of the object. Used for testing
     * only.
     */
    RecentSyncSourceChanges* getRecentSyncSourceChanges_forTest();

    /**
     * Change config (version, term) of each member in the initial test config so that
     * it will be majority replicated without having to mock heartbeats.
     */
    void populateAllMembersConfigVersionAndTerm_forTest();

    /**
     * Records the ping for the given host. For use only in testing.
     */
    void setPing_forTest(const HostAndPort& host, Milliseconds ping);

    // Returns _electionTime.  Only used in unittests.
    Timestamp getElectionTime() const;

    // Returns _electionId.  Only used in unittests.
    OID getElectionId() const;

    // Returns the name for a role.  Only used in unittests.
    static std::string roleToString(TopologyCoordinator::Role role);

private:
    typedef int UnelectableReasonMask;
    class PingStats;

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
     *       kLeaderElect-----------------                |
     *          |    ^  |                |                |
     *          |    |  |                |                |
     *          v    |  |                |                |
     *       kWritablePrimary -----------------           |
     *        |  ^   |  |                |    |           |
     *        |  |   |  |                |    |           |
     *        |  |   |  |                |    |           |
     *        v  |   |  v                v    v           |
     *  kAttemptingStepDown----------->kSteppingDown------|
     */
    enum class LeaderMode {
        kNotLeader,        // This node is not currently a leader.
        kLeaderElect,      // This node has been elected leader, but can't yet accept writes.
        kWritablePrimary,  // This node can accept writes. Depending on whether the client sent
                           // hello or isMaster, will report isWritablePrimary:true or ismaster:true
        kSteppingDown,     // This node is in the middle of a hb, force reconfig or stepdown
                           // command that must complete.
        kAttemptingStepDown,  // This node is in the middle of a cmd initiated step down that might
                              // fail.
    };

    enum UnelectableReason {
        None = 0,
        CannotSeeMajority = 1 << 0,
        ArbiterIAm = 1 << 1,
        NotSecondary = 1 << 2,
        NoPriority = 1 << 3,
        StepDownPeriodActive = 1 << 4,
        NoData = 1 << 5,
        NotInitialized = 1 << 6,
        NotCloseEnoughToLatestForPriorityTakeover = 1 << 7,
        NotFreshEnoughForCatchupTakeover = 1 << 8,
    };

    // Set what type of PRIMARY this node currently is.
    void _setLeaderMode(LeaderMode mode);

    // Returns a HostAndPort if one is forced via the 'replSetSyncFrom' command.
    boost::optional<HostAndPort> _chooseSyncSourceReplSetSyncFrom(Date_t now);

    // Does preliminary checks involved in choosing sync source
    // * Do we have a valid configuration?
    // * Is the 'forceSyncSourceCandidate' failpoint enabled?
    // * Have we gotten enough pings?
    // Returns a HostAndPort if one is decided (may be empty), boost:none if we need to move to the
    // next step.
    boost::optional<HostAndPort> _chooseSyncSourceInitialChecks(Date_t now);

    // Returns the primary node if it is a valid sync source, otherwise returns an empty
    // HostAndPort.
    HostAndPort _choosePrimaryAsSyncSource(Date_t now, const OpTime& lastOpTimeFetched);

    // Chooses a sync source among available nodes.  ReadPreference may be any value but
    // PrimaryOnly, but PrimaryPreferred is treated the same as "Nearest" (it is assumed
    // the caller will handle PrimaryPreferred by trying _choosePrimaryAsSyncSource() first)
    HostAndPort _chooseNearbySyncSource(Date_t now,
                                        const OpTime& lastOpTimeFetched,
                                        ReadPreference readPreference);

    // Does preliminary checkes to see if a new sync source should be chosen
    // * Do we have a valid configuration -- if so, we do not change sync source.
    // * Are we in initial sync -- if so, we do not change sync source.
    // * Do we have a new forced sync source -- if so, we do change sync source.
    // Returns decision and current sync source candidate if decision is kMaybe.
    // (kMaybe indicates to continue with further checks).
    enum class ChangeSyncSourceDecision { kNo, kYes, kMaybe };
    std::pair<ChangeSyncSourceDecision, int> _shouldChangeSyncSourceInitialChecks(
        const HostAndPort& currentSource) const;

    // Returns true if we should choose a new sync source because chaining is disabled
    // and there is a new primary.
    bool _shouldChangeSyncSourceDueToNewPrimary(const HostAndPort& currentSource,
                                                int currentSourceIndex) const;

    // Change sync source if they are not ahead of us, and don't have a sync source.
    // Note 'syncSourceIndex' is the index of our sync source's sync source. The 'currentSource'
    // is our sync source.
    bool _shouldChangeSyncSourceDueToSourceNotAhead(const HostAndPort& currentSource,
                                                    int syncSourceIndex,
                                                    bool syncSourceIsPrimary,
                                                    const OpTime& currentSourceOpTime,
                                                    const OpTime& lastOpTimeFetched) const;

    // Change sync source if our sync source is also syncing from us when we are in primary
    // catchup mode, forming a sync source selection cycle, and the sync source is not ahead
    // of us.
    // Note 'syncSourceHost' and 'syncSourceIndex' are the host and index of ourb sync source's
    // sync source. The 'currentSource' is our sync source.
    bool _shouldChangeSyncSourceToBreakCycle(const HostAndPort& currentSource,
                                             const std::string& syncSourceHost,
                                             int syncSourceIndex,
                                             const OpTime& currentSourceOpTime,
                                             const OpTime& lastOpTimeFetched) const;

    // Returns true if we should choose a new sync source due to our current sync source being
    // greater than maxSyncSourceLagSeconds and a better source being available.
    bool _shouldChangeSyncSourceDueToLag(const HostAndPort& currentSource,
                                         const OpTime& currentSourceOpTime,
                                         const OpTime& lastOpTimeFetched,
                                         Date_t now) const;

    // Returns true if we should choose a new sync source because our current sync source does
    // not match our strict criteria for sync source candidates, but another member does.
    bool _shouldChangeSyncSourceDueToBetterEligibleSource(const HostAndPort& currentSource,
                                                          int currentSourceIndex,
                                                          const OpTime& lastOpTimeFetched,
                                                          Date_t now) const;
    /*
     * Clear this node's sync source.
     */
    void _clearSyncSource();

    /**
     * Sets this node's sync source. It will also update whether the sync source was forced and add
     * a new entry to recent sync source changes.
     */
    void _setSyncSource(HostAndPort newSyncSource, Date_t now, bool forced = false);

    // Returns the oldest acceptable OpTime that a node must have for us to choose it as our sync
    // source.
    OpTime _getOldestSyncOpTime() const;

    // Returns true if the candidate node is viable as our sync source.
    bool _isEligibleSyncSource(int candidateIndex,
                               Date_t now,
                               const OpTime& lastOpTimeFetched,
                               ReadPreference readPreference,
                               bool firstAttempt,
                               bool shouldCheckStaleness) const;

    // Returns the current "ping" value for the given member by their address.
    Milliseconds _getPing(const HostAndPort& host);

    // Returns the index of the member with the matching id, or -1 if none match.
    int _getMemberIndex(int id) const;

    // Sees if a majority number of votes are held by members who are currently "up"
    bool _aMajoritySeemsToBeUp() const;

    // Checks if the node can see a healthy primary of equal or greater priority to the
    // candidate. If so, returns the index of that node. Otherwise returns -1.
    int _findHealthyPrimaryOfEqualOrGreaterPriority(int candidateIndex) const;

    // Is our optime close enough to the latest known optime to call for a priority takeover.
    bool _amIFreshEnoughForPriorityTakeover() const;

    // Is the primary node still in catchup mode and is our optime the latest
    // known optime of all the up nodes.
    bool _amIFreshEnoughForCatchupTakeover() const;

    // Returns reason why "self" member is unelectable
    UnelectableReasonMask _getMyUnelectableReason(Date_t now, StartElectionReasonEnum reason) const;

    // Returns reason why memberIndex is unelectable
    UnelectableReasonMask _getUnelectableReason(int memberIndex) const;

    // Returns the nice text of why the node is unelectable
    std::string _getUnelectableReasonString(UnelectableReasonMask ur) const;

    // Return true if we are currently primary
    bool _iAmPrimary() const;

    // Helper shortcut to self config
    const MemberConfig& _selfConfig() const;

    // Helper shortcut to self member data for const members.
    const MemberData& _selfMemberData() const;

    // Helper shortcut to self member data for non-const members.
    MemberData& _selfMemberData();

    // Index of self member in member data.
    int _selfMemberDataIndex() const;

    /*
     * Returns information we have on the state of the node identified by memberId.  Returns
     * nullptr if memberId is not found in the configuration.
     */
    MemberData* _findMemberDataByMemberId(int memberId);

    /**
     * Performs updating "_currentPrimaryIndex" for processHeartbeatResponse().
     */
    void _updatePrimaryFromHBDataV1(Date_t now);

    /**
     * Determine if the node should run PriorityTakeover or CatchupTakeover.
     */
    HeartbeatResponseAction _shouldTakeOverPrimary(int updatedConfigIndex);
    /**
     * Updates _memberData based on the newConfig, ensuring that every member in the newConfig
     * has an entry in _memberData.  If any nodes in the newConfig are also present in
     * _currentConfig, copies their heartbeat info into the corresponding entry in the updated
     * _memberData vector.
     */
    void _updateHeartbeatDataForReconfig(const ReplSetConfig& newConfig, int selfIndex, Date_t now);

    /**
     * Returns whether a stepdown attempt should be allowed to proceed.  See the comment for
     * tryToStartStepDown() for more details on the rules of when stepdown attempts succeed
     * or fail.
     */
    bool _canCompleteStepDownAttempt(Date_t now, Date_t waitUntil, bool force);

    /**
     * Returns true if a node is both caught up to our last applied opTime and electable.
     */
    bool _isCaughtUpAndElectable(int memberIndex, OpTime lastApplied);

    void _stepDownSelfAndReplaceWith(int newPrimary);

    /**
     * Looks up the provided member in the denylist and returns true if the member's denylist
     * expire time is after 'now'.  If the member is found but the expire time is before 'now',
     * the function returns false.  If the member is not found in the denylist, the function
     * returns false.
     **/
    bool _memberIsDenylisted(const MemberConfig& memberConfig, Date_t now) const;

    // Returns a string representation of the current replica set status for logging purposes.
    std::string _getReplSetStatusString();

    // This node's role in the replication protocol.
    Role _role;

    // This node's topology version. This is updated upon a significant topology change.
    TopologyVersion _topologyVersion;

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
    std::map<HostAndPort, Date_t> _syncSourceDenylist;
    // The next sync source to be chosen, requested via a replSetSyncFrom command
    int _forceSyncSourceIndex;
    // Whether the current sync source has been set via a replSetSyncFrom command
    bool _replSetSyncFromSet;

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
    OpTimeAndWallTime _lastCommittedOpTimeAndWallTime;

    // OpTime representing our transition to PRIMARY and the start of our term.
    // _lastCommittedOpTime cannot be set to an earlier OpTime.
    OpTime _firstOpTimeOfMyTerm;

    // Latest committed optime in the previous config.
    OpTime _lastCommittedInPrevConfig;

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
    // For the purpose of deciding on a sync source, we count only pings for nodes which are in our
    // current config.
    int pingsInConfig = 0;

    // V1 last vote info for elections
    LastVote _lastVote{OpTime::kInitialTerm, -1};

    enum class ReadCommittedSupport {
        kUnknown,
        kNo,
        kYes,
    };

    // Whether or not the storage engine supports read committed.
    ReadCommittedSupport _storageEngineSupportsReadCommitted{ReadCommittedSupport::kUnknown};

    RecentSyncSourceChanges _recentSyncSourceChanges;
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
     * Sets the ping time without considering previous pings. For use only in testing.
     */
    void set_forTest(Milliseconds millis);

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
