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

#include "mongo/db/repl/replication_coordinator_fwd.h"

#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/member_data.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/split_horizon.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class CommitQuorumOptions;
class IndexDescriptor;
class NamespaceString;
class OperationContext;
class ServiceContext;
class Timestamp;
struct WriteConcernOptions;

namespace executor {
struct ConnectionPoolStats;
}  // namespace executor

namespace rpc {

class OplogQueryMetadata;
class ReplSetMetadata;

}  // namespace rpc

namespace repl {

class BackgroundSync;
class HelloResponse;
class OpTime;
class OpTimeAndWallTime;
class ReadConcernArgs;
class ReplSetConfig;
class ReplSetHeartbeatArgsV1;
class ReplSetHeartbeatResponse;
class ReplSetRequestVotesArgs;
class ReplSetRequestVotesResponse;
class UpdatePositionArgs;

/**
 * The ReplicationCoordinator is responsible for coordinating the interaction of replication
 * with the rest of the system.  The public methods on ReplicationCoordinator are the public
 * API that the replication subsystem presents to the rest of the codebase.
 */
class ReplicationCoordinator : public SyncSourceSelector {
    ReplicationCoordinator(const ReplicationCoordinator&) = delete;
    ReplicationCoordinator& operator=(const ReplicationCoordinator&) = delete;

public:
    static ReplicationCoordinator* get(ServiceContext* service);
    static ReplicationCoordinator* get(ServiceContext& service);
    static ReplicationCoordinator* get(OperationContext* ctx);

    static void set(ServiceContext* service,
                    std::unique_ptr<ReplicationCoordinator> replCoordinator);

    struct MONGO_WARN_UNUSED_RESULT_CLASS StatusAndDuration {
    public:
        Status status;
        Milliseconds duration;

        StatusAndDuration(const Status& stat, Milliseconds ms) : status(stat), duration(ms) {}
    };

    virtual ~ReplicationCoordinator();

    /**
     * Does any initial bookkeeping needed to start replication, and instructs the other
     * components of the replication system to start up whatever threads and do whatever
     * initialization they need.
     */
    virtual void startup(OperationContext* opCtx,
                         StorageEngine::LastShutdownState lastShutdownState) = 0;

    /**
     * Start terminal shutdown.  This causes the topology coordinator to refuse to vote in any
     * further elections.  This should only be called from global shutdown after we've passed the
     * point of no return.
     *
     * This should be called once we are sure to call shutdown().
     */
    virtual void enterTerminalShutdown() = 0;

    /**
     * We enter quiesce mode during the shutdown process if we are in secondary mode. While in
     * quiesce mode, we allow reads to continue and accept new reads, but we fail hello requests
     * with ShutdownInProgress. This function causes us to increment the topologyVersion and start
     * failing hello requests with ShutdownInProgress. Returns true if the server entered quiesce
     * mode.
     *
     * We take in quiesceTime only for reporting purposes. The waiting during quiesce mode happens
     * external to the ReplicationCoordinator.
     */
    virtual bool enterQuiesceModeIfSecondary(Milliseconds quiesceTime) = 0;

    /**
     * Returns whether the server is in quiesce mode.
     */
    virtual bool inQuiesceMode() const = 0;

    /**
     * Does whatever cleanup is required to stop replication, including instructing the other
     * components of the replication system to shut down and stop any threads they are using,
     * blocking until all replication-related shutdown tasks are complete.
     */
    virtual void shutdown(OperationContext* opCtx) = 0;

    /**
     * Returns a reference to the parsed command line arguments that are related to replication.
     */
    virtual const ReplSettings& getSettings() const = 0;

    enum Mode { modeNone = 0, modeReplSet };

    /**
     * Returns a value indicating whether this node was configured at start-up to run standalone or
     * as a member of a replica set.
     */
    virtual Mode getReplicationMode() const = 0;

    /**
     * Returns true if this node is configured to be a member of a replica set.
     */
    virtual bool isReplEnabled() const = 0;

    /**
     * Returns the current replica set state of this node (PRIMARY, SECONDARY, STARTUP, etc).
     * It is invalid to call this unless getReplicationMode() == modeReplSet.
     */
    virtual MemberState getMemberState() const = 0;

    /**
     * Returns whether this node can accept writes to databases other than local.
     */
    virtual bool canAcceptNonLocalWrites() const = 0;

    /**
     * Waits for 'timeout' ms for member state to become 'state'.
     * Returns OK if member state is 'state'.
     * Returns ErrorCodes::ExceededTimeLimit if we timed out waiting for the state change.
     * Returns ErrorCodes::BadValue if timeout is negative.
     * Throws if interrupted (pass Interruptible::notInterruptible() if interruption is not desired.
     */
    virtual Status waitForMemberState(Interruptible* interruptible,
                                      MemberState expectedState,
                                      Milliseconds timeout) = 0;

    /**
     * Returns true if this node is in state PRIMARY or SECONDARY.
     *
     * It is invalid to call this unless getReplicationMode() == modeReplSet.
     *
     * This method may be optimized to reduce synchronization overhead compared to
     * reading the current member state with getMemberState().
     */
    virtual bool isInPrimaryOrSecondaryState(OperationContext* opCtx) const = 0;

    /**
     * Version which does not check for the RSTL. Without the RSTL, the return value may be
     * inaccurate by the time the function returns.
     */
    virtual bool isInPrimaryOrSecondaryState_UNSAFE() const = 0;

    /**
     * Returns how secondary delayed this node is configured to be, or 0 seconds if this node is not
     * a member of the current replica set configuration.
     */
    virtual Seconds getSecondaryDelaySecs() const = 0;

    /**
     * Blocks the calling thread for up to writeConcern.wTimeout millis, or until "opTime" has
     * been replicated to at least a set of nodes that satisfies the writeConcern, whichever
     * comes first. A writeConcern.wTimeout of 0 indicates no timeout (block forever) and a
     * writeConcern.wTimeout of -1 indicates return immediately after checking. Return codes:
     * ErrorCodes::WriteConcernFailed if the writeConcern.wTimeout is reached before
     *     the data has been sufficiently replicated
     * ErrorCodes::ExceededTimeLimit if the opCtx->getMaxTimeMicrosRemaining is reached before
     *     the data has been sufficiently replicated
     * ErrorCodes::NotWritablePrimary if the node is not a writable primary
     * ErrorCodes::UnknownReplWriteConcern if the writeConcern.w contains a write concern
     *     mode that is not known
     * ErrorCodes::ShutdownInProgress if we are mid-shutdown
     * ErrorCodes::Interrupted if the operation was killed with killop()
     */
    virtual StatusAndDuration awaitReplication(OperationContext* opCtx,
                                               const OpTime& opTime,
                                               const WriteConcernOptions& writeConcern) = 0;

    /**
     * Returns a future that will be set when the given writeConcern has been satisfied or the node
     * is not a writable primary, is interrupted, or shuts down. Notably this will ignore the
     * wTimeout in the given write concern.
     */
    virtual SharedSemiFuture<void> awaitReplicationAsyncNoWTimeout(
        const OpTime& opTime, const WriteConcernOptions& writeConcern) = 0;

    /**
     * Causes this node to relinquish being primary for at least 'stepdownTime'.  If 'force' is
     * false, before doing so it will wait for 'waitTime' for one other electable node to be caught
     * up before stepping down. Throws on error.
     */
    virtual void stepDown(OperationContext* opCtx,
                          bool force,
                          const Milliseconds& waitTime,
                          const Milliseconds& stepdownTime) = 0;

    /**
     * Returns true if the primary can be considered writable for the purpose of introspective
     * commands such as hello() and rs.status().
     */
    virtual bool isWritablePrimaryForReportingPurposes() = 0;

    /**
     * Returns true if it is valid for this node to accept writes on the given database.  Currently
     * this is true only if this node is Primary, a standalone, or is writing to the local database.
     *
     * If a node was started with the replSet argument, but has not yet received a config, it
     * will not be able to receive writes to a database other than local (it will not be
     * treated as standalone node).
     *
     * NOTE: This function can only be meaningfully called while the caller holds the
     * ReplicationStateTransitionLock in some mode other than MODE_NONE.
     */
    virtual bool canAcceptWritesForDatabase(OperationContext* opCtx, StringData dbName) = 0;

    /**
     * Version which does not check for the RSTL.  Do not use in new code. Without the RSTL, the
     * return value may be inaccurate by the time the function returns.
     */
    virtual bool canAcceptWritesForDatabase_UNSAFE(OperationContext* opCtx, StringData dbName) = 0;

    /**
     * Returns true if it is valid for this node to accept writes on the given namespace.
     *
     * The result of this function should be consistent with canAcceptWritesForDatabase()
     * for the database the namespace refers to, with additional checks on the collection.
     */
    virtual bool canAcceptWritesFor(OperationContext* opCtx,
                                    const NamespaceStringOrUUID& nsOrUUID) = 0;

    /**
     * Version which does not check for the RSTL.  Do not use in new code. Without the RSTL held,
     * the return value may be inaccurate by the time the function returns.
     */
    virtual bool canAcceptWritesFor_UNSAFE(OperationContext* opCtx,
                                           const NamespaceStringOrUUID& nsOrUUID) = 0;

    /**
     * Checks if the current replica set configuration can satisfy the given write concern.
     *
     * Things that are taken into consideration include:
     * 1. If the set has enough data-bearing members.
     * 2. If the write concern mode exists.
     * 3. If there are enough members for the write concern mode specified.
     */
    virtual Status checkIfWriteConcernCanBeSatisfied(
        const WriteConcernOptions& writeConcern) const = 0;

    /**
     * Checks if the 'commitQuorum' can be satisfied by all the members in the replica set; if it
     * cannot be satisfied, then the 'UnsatisfiableCommitQuorum' error code is returned.
     *
     * Returns the 'NoReplicationEnabled' error code if this is called without replication enabled.
     */
    virtual Status checkIfCommitQuorumCanBeSatisfied(
        const CommitQuorumOptions& commitQuorum) const = 0;

    /**
     * Checks if the 'members' list can satisfy the 'commitQuorum'.
     */
    virtual bool isCommitQuorumSatisfied(const CommitQuorumOptions& commitQuorum,
                                         const std::vector<mongo::HostAndPort>& members) const = 0;

    /**
     * Returns Status::OK() if it is valid for this node to serve reads on the given collection
     * and an errorcode indicating why the node cannot if it cannot.
     *
     * Should not be called if namespace is locked is a view. The user should instead check if we
     * can serve reads for the backing collection.
     */
    virtual Status checkCanServeReadsFor(OperationContext* opCtx,
                                         const NamespaceString& ns,
                                         bool secondaryOk) = 0;

    /**
     * Version which does not check for the RSTL.  Do not use in new code. Without the RSTL held,
     * the return value may be inaccurate by the time the function returns.
     */
    virtual Status checkCanServeReadsFor_UNSAFE(OperationContext* opCtx,
                                                const NamespaceString& ns,
                                                bool secondaryOk) = 0;

    /**
     * Returns true if this node should ignore index constraints for idempotency reasons.
     *
     * The namespace "ns" is passed in because the "local" database is usually writable
     * and we need to enforce the constraints for it.
     */
    virtual bool shouldRelaxIndexConstraints(OperationContext* opCtx,
                                             const NamespaceString& ns) = 0;

    /**
     * Updates our internal tracking of the last OpTime applied to this node. Also updates our
     * internal tracking of the wall clock time corresponding to that operation.
     *
     * The new value of "opTime" must be no less than any prior value passed to this method, and
     * it is the caller's job to properly synchronize this behavior.  The exception to this rule
     * is that after calls to resetLastOpTimesFromOplog(), the minimum acceptable value for
     * "opTime" is reset based on the contents of the oplog, and may go backwards due to
     * rollback. Additionally, the optime given MUST represent a consistent database state.
     */
    virtual void setMyLastAppliedOpTimeAndWallTime(const OpTimeAndWallTime& opTimeAndWallTime) = 0;

    /**
     * Updates our internal tracking of the last OpTime durable to this node. Also updates our
     * internal tracking of the wall clock time corresponding to that operation.
     *
     * The new value of "opTime" must be no less than any prior value passed to this method, and
     * it is the caller's job to properly synchronize this behavior.  The exception to this rule
     * is that after calls to resetLastOpTimesFromOplog(), the minimum acceptable value for
     * "opTime" is reset based on the contents of the oplog, and may go backwards due to
     * rollback.
     */
    virtual void setMyLastDurableOpTimeAndWallTime(const OpTimeAndWallTime& opTimeAndWallTime) = 0;

    /**
     * Updates our internal tracking of the last OpTime applied to this node, but only
     * if the supplied optime is later than the current last OpTime known to the replication
     * coordinator.
     *
     * This function is used by logOp() on a primary, since the ops in the oplog do not
     * necessarily commit in sequential order. It is also used when we finish oplog batch
     * application on secondaries, to avoid any potential race conditions around setting the
     * applied optime from more than one thread.
     *
     * Since the last applied op time and wall time might not be visible (i.e. there may be
     * "oplog holes" from oplog entries with earlier timestamps which commit after this one)
     * this method does not notify oplog waiters.  Callers which know the new lastApplied is at
     * a no-holes point should call signalOplogWaiters after calling this method.
     */
    virtual void setMyLastAppliedOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime) = 0;

    /**
     * Updates our internal tracking of the last OpTime durable to this node, but only
     * if the supplied optime is later than the current last OpTime known to the replication
     * coordinator. Also updates the wall clock time corresponding to that operation.
     *
     * This function is used by logOp() on a primary, since the ops in the oplog do not
     * necessarily commit in sequential order.
     */
    virtual void setMyLastDurableOpTimeAndWallTimeForward(
        const OpTimeAndWallTime& opTimeAndWallTime) = 0;
    // virtual void setMyLastDurableOpTimeForward(const OpTimeAndWallTime& opTimeAndWallTime) = 0;

    /**
     * Same as above, but used during places we need to zero our last optime.
     */
    virtual void resetMyLastOpTimes() = 0;

    /**
     * Updates our the message we include in heartbeat responses.
     */
    virtual void setMyHeartbeatMessage(const std::string& msg) = 0;

    /**
     * Returns the last optime recorded by setMyLastAppliedOpTime.
     */
    virtual OpTime getMyLastAppliedOpTime() const = 0;

    /*
     * Returns the same as getMyLastAppliedOpTime() and additionally returns the wall clock time
     * corresponding to that OpTime.
     *
     * When rollbackSafe is true, this returns an empty OpTimeAndWallTime if the node is in ROLLBACK
     * state. The lastAppliedOpTime during ROLLBACK might be temporarily pointing to an oplog entry
     * in the divergent branch of history which would become invalid after the rollback finishes.
     */
    virtual OpTimeAndWallTime getMyLastAppliedOpTimeAndWallTime(
        bool rollbackSafe = false) const = 0;

    /**
     * Returns the last optime recorded by setMyLastDurableOpTime.
     */
    virtual OpTime getMyLastDurableOpTime() const = 0;

    /*
     * Returns the same as getMyLastDurableOpTime() and additionally returns the wall clock time
     * corresponding to that OpTime.
     */
    virtual OpTimeAndWallTime getMyLastDurableOpTimeAndWallTime() const = 0;

    /**
     * Waits until the majority committed snapshot is at least the 'targetOpTime'.
     */
    virtual Status waitUntilMajorityOpTime(OperationContext* opCtx,
                                           OpTime targetOpTime,
                                           boost::optional<Date_t> deadline = boost::none) = 0;

    /**
     * Waits until the optime of the current node is at least the opTime specified in 'settings'.
     *
     * Returns whether the wait was successful.
     */
    virtual Status waitUntilOpTimeForRead(OperationContext* opCtx,
                                          const ReadConcernArgs& settings) = 0;

    /**
     * Waits until the deadline or until the optime of the current node is at least the opTime
     * specified in 'settings'.
     *
     * Returns whether the wait was successful.
     */
    virtual Status waitUntilOpTimeForReadUntil(OperationContext* opCtx,
                                               const ReadConcernArgs& settings,
                                               boost::optional<Date_t> deadline) = 0;

    /**
     * Waits until the timestamp of this node's lastCommittedOpTime is >= the given timestamp.
     *
     * Note that it is not meaningful to ask, globally, whether a particular timestamp is majority
     * committed within a replica set, since timestamps do not uniquely identify log entries. Upon
     * returning successfully, this method only provides the guarantee that the given timestamp is
     * now less than or equal to the timestamp of the majority commit point as known by this node.
     * If the given timestamp is associated with an operation in the local oplog, then it is safe to
     * conclude that that operation is majority committed, assuming no rollbacks occurred. It is
     * always safe to compare commit point timestamps to timestamps in a node's local oplog, since
     * they must be on the same branch of oplog history.
     *
     * Returns whether the wait was successful. Will respect the deadline on the given
     * OperationContext, if one has been set.
     */
    virtual Status awaitTimestampCommitted(OperationContext* opCtx, Timestamp ts) = 0;

    /**
     * Retrieves and returns the current election id, which is a unique id that is local to
     * this node and changes every time we become primary.
     * TODO(spencer): Use term instead.
     */
    virtual OID getElectionId() = 0;

    /**
     * Returns the id for this node as specified in the current replica set configuration.
     */
    virtual int getMyId() const = 0;

    /**
     * Returns the host and port pair for this node as specified in the current replica
     * set configuration.
     */
    virtual HostAndPort getMyHostAndPort() const = 0;

    /**
     * Sets this node into a specific follower mode.
     *
     * Returns OK if the follower mode was successfully set.  Returns NotSecondary if the
     * node is a leader when setFollowerMode is called and ElectionInProgess if the node is in the
     * process of trying to elect itself primary.
     *
     * Follower modes are RS_STARTUP2 (initial sync), RS_SECONDARY, RS_ROLLBACK and
     * RS_RECOVERING.  They are the valid states of a node whose topology coordinator has the
     * follower role.
     *
     * This is essentially an interface that allows the applier to prevent the node from
     * becoming a candidate or accepting reads, depending on circumstances in the oplog
     * application process.
     */
    virtual Status setFollowerMode(const MemberState& newState) = 0;

    /**
     * Version which checks for the RSTL in mode X before setting this node into a specific follower
     * mode. This is used for transitioning to RS_ROLLBACK so that we can conflict with readers
     * holding the RSTL in intent mode.
     */
    virtual Status setFollowerModeRollback(OperationContext* opCtx) = 0;

    /**
     * Step-up
     * =======
     * On stepup, repl coord enters catch-up mode. It's the same as the secondary mode from
     * the perspective of producer and applier, so there's nothing to do with them.
     * When a node enters drain mode, producer state = Stopped, applier state = Draining.
     *
     * If the applier state is Draining, it will signal repl coord when there's nothing to apply.
     * The applier goes into Stopped state at the same time.
     *
     * The states go like the following:
     * - secondary and during catchup mode
     * (producer: Running, applier: Running)
     *      |
     *      | finish catch-up, enter drain mode
     *      V
     * - drain mode
     * (producer: Stopped, applier: Draining)
     *      |
     *      | applier signals drain is complete
     *      V
     * - primary is in writable mode
     * (producer: Stopped, applier: Stopped)
     *
     *
     * Step-down
     * =========
     * The state transitions become:
     * - primary is in writable mode
     * (producer: Stopped, applier: Stopped)
     *      |
     *      | step down
     *      V
     * - secondary mode, starting bgsync
     * (producer: Starting, applier: Running)
     *      |
     *      | bgsync runs start()
     *      V
     * - secondary mode, normal
     * (producer: Running, applier: Running)
     *
     * When a node steps down during draining mode, it's OK to change from (producer: Stopped,
     * applier: Draining) to (producer: Starting, applier: Running).
     *
     * When a node steps down during catchup mode, the states remain the same (producer: Running,
     * applier: Running).
     *
     * DrainingForShardSplit follows the same state diagram as Draining, it only exists to hint the
     * signalDrainModeComplete method that it should not follow the primary step-up logic.
     */
    enum class ApplierState { Running, Draining, DrainingForShardSplit, Stopped };

    /**
     * In normal cases: Running -> Draining -> Stopped -> Running.
     * Draining -> Running is also possible if a node steps down during drain mode.
     *
     * Only the applier can make the transition from Draining to Stopped by calling
     * signalDrainComplete().
     */
    virtual ApplierState getApplierState() = 0;

    /**
     * Signals that a previously requested pause and drain of the applier buffer
     * has completed.
     *
     * This is an interface that allows the applier to reenable writes after
     * a successful election triggers the draining of the applier buffer.
     *
     * The applier signals drain complete when the buffer is empty and it's in Draining
     * state. We need to make sure the applier checks both conditions in the same term.
     * Otherwise, it's possible that the applier confirms the empty buffer, but the node
     * steps down and steps up so quickly that the applier signals drain complete in the wrong
     * term.
     */
    virtual void signalDrainComplete(OperationContext* opCtx,
                                     long long termWhenBufferIsEmpty) noexcept = 0;

    /**
     * Signals the sync source feedback thread to wake up and send a handshake and
     * replSetUpdatePosition command to our sync source.
     */
    virtual void signalUpstreamUpdater() = 0;

    /**
     * Prepares a BSONObj describing an invocation of the replSetUpdatePosition command that can
     * be sent to this node's sync source to update it about our progress in replication.
     */
    virtual StatusWith<BSONObj> prepareReplSetUpdatePositionCommand() const = 0;

    enum class ReplSetGetStatusResponseStyle { kBasic, kInitialSync };

    /**
     * Handles an incoming replSetGetStatus command. Adds BSON to 'result'. If kInitialSync is
     * requested but initial sync is not running, kBasic will be used.
     */
    virtual Status processReplSetGetStatus(OperationContext* opCtx,
                                           BSONObjBuilder* result,
                                           ReplSetGetStatusResponseStyle responseStyle) = 0;

    /**
     * Adds to "result" a description of the memberData data structure used to map RIDs to their
     * last known optimes.
     */
    virtual void appendSecondaryInfoData(BSONObjBuilder* result) = 0;

    /**
     * Returns a copy of the current ReplSetConfig.
     *
     * To avoid unnecessarily copying the ReplSetConfig through this method, please use the
     * getConfig* methods below if you are only accessing a specific field of the ReplSetConfig,
     * i.e. replCoord->getConfigNumMembers() instead of replCoord->getConfig().getNumMembers(). See
     * SERVER-47828.
     */
    virtual ReplSetConfig getConfig() const = 0;

    /**
     * Returns the current ReplSetConfig's connection string.
     */
    virtual ConnectionString getConfigConnectionString() const = 0;

    /**
     * Returns the current ReplSetConfig's election timeout period.
     */
    virtual Milliseconds getConfigElectionTimeoutPeriod() const = 0;

    /**
     * Returns the current ReplSetConfig's voting members.
     */
    virtual std::vector<MemberConfig> getConfigVotingMembers() const = 0;

    /**
     * Returns the current ReplSetConfig's term.
     */
    virtual std::int64_t getConfigTerm() const = 0;

    /**
     * Returns the current ReplSetConfig's version.
     */
    virtual std::int64_t getConfigVersion() const = 0;

    /**
     * Returns the (version, term) pair of the current ReplSetConfig.
     */
    virtual ConfigVersionAndTerm getConfigVersionAndTerm() const = 0;

    /**
     * Returns the number of members in the current ReplSetConfig.
     */
    virtual int getConfigNumMembers() const = 0;

    /**
     * Returns the amount of time to wait for a response to heartbeats sent to other
     * nodes in the current ReplSetConfig.
     */
    virtual Milliseconds getConfigHeartbeatTimeoutPeriodMillis() const = 0;

    /**
     * Returns the BSON of the current ReplSetConfig.
     */
    virtual BSONObj getConfigBSON() const = 0;

    /**
     * Validates the given WriteConcernOptions on the current ReplSetConfig.
     */
    virtual Status validateWriteConcern(const WriteConcernOptions& writeConcern) const = 0;

    /**
     * Returns a pointer to the MemberConfig corresponding to the member with the given
     * HostAndPort in the current ReplSetConfig, or NULL if there is no member with that address.
     */
    virtual const MemberConfig* findConfigMemberByHostAndPort(const HostAndPort& hap) const = 0;

    /**
     * Returns whether all members of the current ReplSetConfig set have hostname localhost.
     */
    virtual bool isConfigLocalHostAllowed() const = 0;

    /**
     * Returns the interval between the time the last heartbeat from a node was received
     * successfully, or the time when we gave up retrying, and when the next heartbeat should be
     * sent to a target, for the current ReplSetConfig.
     */
    virtual Milliseconds getConfigHeartbeatInterval() const = 0;

    /**
     * Handles an incoming replSetGetConfig command. Adds BSON to 'result'.
     *
     * If commitmentStatus is true, adds a boolean 'commitmentStatus' field to 'result' indicating
     * whether the current config is committed.
     *
     * If includeNewlyAdded is true, does not omit 'newlyAdded' fields from the config.
     */
    virtual void processReplSetGetConfig(BSONObjBuilder* result,
                                         bool commitmentStatus = false,
                                         bool includeNewlyAdded = false) = 0;

    /**
     * Processes the ReplSetMetadata returned from a command run against another
     * replica set member and so long as the config version in the metadata matches the replica set
     * config version this node currently has, updates the current term.
     *
     * This does NOT update this node's notion of the commit point.
     */
    virtual void processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) = 0;

    /**
     * This updates the node's notion of the commit point. We ignore 'committedOptime' if it has a
     * different term than our lastApplied, unless 'fromSyncSource'=true, which guarantees we are on
     * the same branch of history as 'committedOptime', so we update our commit point to
     * min(committedOptime, lastApplied).
     */
    virtual void advanceCommitPoint(const OpTimeAndWallTime& committedOpTimeAndWallTime,
                                    bool fromSyncSource) = 0;

    /**
     * Elections under protocol version 1 are triggered by a timer.
     * When a node is informed of the primary's liveness (either through heartbeats or
     * while reading a sync source's oplog), it calls this function to postpone the
     * election timer by a duration of at least 'electionTimeoutMillis' (see getConfig()).
     * If the current node is not electable (secondary with priority > 0), this function
     * cancels the existing timer but will not schedule a new one.
     */
    virtual void cancelAndRescheduleElectionTimeout() = 0;

    /**
     * Toggles maintenanceMode to the value expressed by 'activate'
     * return Status::OK if the change worked, NotSecondary if it failed because we are
     * PRIMARY, and OperationFailed if we are not currently in maintenance mode.
     *
     * Takes the ReplicationStateTransitionLock (RSTL) in X mode, since the state can potentially
     * change to and from RECOVERING.
     */
    virtual Status setMaintenanceMode(OperationContext* opCtx, bool activate) = 0;

    /**
     * Retrieves the current count of maintenanceMode and returns 'true' if greater than 0.
     */
    virtual bool getMaintenanceMode() = 0;

    /**
     * Returns true if serverless mode is enabled and the replicaSetId differs from the current
     * replicaSetId. It signals the sync source should be dropped and the new batch of oplogs should
     * be discarded.
     */
    virtual bool shouldDropSyncSourceAfterShardSplit(OID replicaSetId) const = 0;

    /**
     * Handles an incoming replSetSyncFrom command. Adds BSON to 'result'
     * returns Status::OK if the sync target could be set and an ErrorCode indicating why it
     * couldn't otherwise.
     */
    virtual Status processReplSetSyncFrom(OperationContext* opCtx,
                                          const HostAndPort& target,
                                          BSONObjBuilder* resultObj) = 0;

    /**
     * Handles an incoming replSetFreeze command. Adds BSON to 'resultObj'
     * returns Status::OK() if the node is a member of a replica set with a config and an
     * error Status otherwise
     */
    virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj) = 0;

    /**
     * Handles an incoming heartbeat command with arguments 'args'. Populates 'response';
     * returns a Status with either OK or an error message.
     */
    virtual Status processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                      ReplSetHeartbeatResponse* response) = 0;


    /**
     * Arguments for the replSetReconfig command.
     */
    struct ReplSetReconfigArgs {
        BSONObj newConfigObj;
        bool force = false;
    };

    /**
     * Handles an incoming replSetReconfig command. Adds BSON to 'resultObj';
     * returns a Status with either OK or an error message.
     */
    virtual Status processReplSetReconfig(OperationContext* opCtx,
                                          const ReplSetReconfigArgs& args,
                                          BSONObjBuilder* resultObj) = 0;

    /**
     * Install the new config returned by the callback "getNewConfig".
     */
    using GetNewConfigFn = std::function<StatusWith<ReplSetConfig>(const ReplSetConfig& oldConfig,
                                                                   long long currentTerm)>;
    virtual Status doReplSetReconfig(OperationContext* opCtx,
                                     GetNewConfigFn getNewConfig,
                                     bool force) = 0;

    /**
     * Performs a reconfig that skips certain safety checks, including the following:
     * 1) Wait for the current config to be majority committed
     * 2) Wait for oplog commitment
     * 3) Quorum check
     * This function is only intended to be called for internal reconfigs that do not change the
     * consensus group (eg. only bumping the config version or term). These scenarios are expected
     * to be able to bypass certain safety checks because the caller guarantees the reconfig to be
     * safe.
     */
    virtual Status doOptimizedReconfig(OperationContext* opCtx, GetNewConfigFn) = 0;

    /**
     * Waits until the following two conditions are satisfied:
     *  (1) The current config has propagated to a majority of nodes.
     *  (2) Any operations committed in the previous config are committed in the current config.
     */
    virtual Status awaitConfigCommitment(OperationContext* opCtx, bool waitForOplogCommitment) = 0;

    /*
     * Handles an incoming replSetInitiate command. If "configObj" is empty, generates a default
     * configuration to use.
     * Adds BSON to 'resultObj'; returns a Status with either OK or an error message.
     */
    virtual Status processReplSetInitiate(OperationContext* opCtx,
                                          const BSONObj& configObj,
                                          BSONObjBuilder* resultObj) = 0;

    /**
     * Handles an incoming replSetUpdatePosition command, updating each node's oplog progress.
     * Returns Status::OK() if all updates are processed correctly, NodeNotFound
     * if any updating node cannot be found in the config, InvalidReplicaSetConfig if the
     * "configVersion" sent in any of the updates doesn't match our config version, or
     * NotPrimaryOrSecondary if we are in state REMOVED or otherwise don't have a valid
     * replica set config.
     * If a non-OK status is returned, it is unspecified whether none or some of the updates
     * were applied.
     * "configVersion" will be populated with our config version if and only if we return
     * InvalidReplicaSetConfig.
     */
    virtual Status processReplSetUpdatePosition(const UpdatePositionArgs& updates) = 0;

    /**
     * Returns a bool indicating whether or not this node builds indexes.
     */
    virtual bool buildsIndexes() = 0;

    /**
     * Returns a vector of members that have applied the operation with OpTime 'op'.
     * "durablyWritten" indicates whether the operation has to be durably applied.
     */
    virtual std::vector<HostAndPort> getHostsWrittenTo(const OpTime& op, bool durablyWritten) = 0;

    /**
     * Returns a BSONObj containing a representation of the current default write concern.
     */
    virtual WriteConcernOptions getGetLastErrorDefault() = 0;

    /**
     * Checks that the --replSet flag was passed when starting up the node and that the node
     * has a valid replica set config.
     *
     * Returns a Status indicating whether those conditions are met with errorcode
     * NoReplicationEnabled if --replSet was not present during start up or with errorcode
     * NotYetInitialized in the absence of a valid config. Also adds error info to "result".
     */
    virtual Status checkReplEnabledForCommand(BSONObjBuilder* result) = 0;

    /**
     * Loads the optime from the last op in the oplog into the coordinator's lastAppliedOpTime and
     * lastDurableOpTime values.
     */
    virtual void resetLastOpTimesFromOplog(OperationContext* opCtx) = 0;

    /**
     * Returns the OpTime of the latest replica set-committed op known to this server.
     * Committed means a majority of the voting nodes of the config are known to have the
     * operation in their oplogs.  This implies such ops will never be rolled back.
     */
    virtual OpTime getLastCommittedOpTime() const = 0;
    virtual OpTimeAndWallTime getLastCommittedOpTimeAndWallTime() const = 0;

    /**
     * Returns a list of objects that contain this node's knowledge of the state of the members of
     * the replica set.
     */
    virtual std::vector<MemberData> getMemberData() const = 0;

    /*
     * Handles an incoming replSetRequestVotes command.
     *
     * Populates the given 'response' object with the result of the request. If there is a failure
     * processing the vote request, returns an error status. If an error is returned, the value of
     * the populated 'response' object is invalid.
     */
    virtual Status processReplSetRequestVotes(OperationContext* opCtx,
                                              const ReplSetRequestVotesArgs& args,
                                              ReplSetRequestVotesResponse* response) = 0;

    /**
     * Prepares a metadata object with the ReplSetMetadata and the OplogQueryMetadata depending
     * on what has been requested.
     */
    virtual void prepareReplMetadata(const BSONObj& metadataRequestObj,
                                     const OpTime& lastOpTimeFromClient,
                                     BSONObjBuilder* builder) const = 0;

    /**
     * Returns whether or not majority write concerns should implicitly journal, if j has not been
     * explicitly set.
     */
    virtual bool getWriteConcernMajorityShouldJournal() = 0;

    /**
     * Returns the current term.
     */
    virtual long long getTerm() const = 0;

    /**
     * Returns the TopologyVersion. It is possible to return a stale value. This is safe because
     * we expect the 'processId' field to never change and 'counter' should always be increasing.
     */
    virtual TopologyVersion getTopologyVersion() const = 0;

    /**
     * Attempts to update the current term for the V1 election protocol. If the term changes and
     * this node is primary, relinquishes primary.
     * Returns a Status OK if the term was *not* updated (meaning, it is safe to proceed with
     * the rest of the work, because the term is still the same).
     * Returns StaleTerm if the supplied term was higher than the current term.
     */
    virtual Status updateTerm(OperationContext* opCtx, long long term) = 0;

    /**
     * Blocks until either the current committed snapshot is at least as high as 'untilSnapshot',
     * or we are interrupted for any reason, including shutdown or maxTimeMs expiration.
     * 'opCtx' is used to checkForInterrupt and enforce maxTimeMS.
     */
    virtual void waitUntilSnapshotCommitted(OperationContext* opCtx,
                                            const Timestamp& untilSnapshot) = 0;

    /**
     * Clears the current committed snapshot.
     */
    virtual void clearCommittedSnapshot() = 0;

    /**
     * Gets the latest OpTime of the currentCommittedSnapshot.
     */
    virtual OpTime getCurrentCommittedSnapshotOpTime() const = 0;

    /**
     * Appends diagnostics about the replication subsystem.
     */
    virtual void appendDiagnosticBSON(BSONObjBuilder* bob) = 0;

    /**
     * Appends connection information to the provided BSONObjBuilder.
     */
    virtual void appendConnectionStats(executor::ConnectionPoolStats* stats) const = 0;

    /**
     * Creates a waiter that waits for w:majority write concern to be satisfied up to opTime before
     * setting the 'wMajorityWriteAvailabilityDate' election candidate metric.
     */
    virtual void createWMajorityWriteAvailabilityDateWaiter(OpTime opTime) = 0;

    /**
     * Returns a new WriteConcernOptions based on "wc" but with UNSET syncMode reset to JOURNAL or
     * NONE based on our rsConfig.
     */
    virtual WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(
        WriteConcernOptions wc) = 0;

    virtual Status stepUpIfEligible(bool skipDryRun) = 0;

    virtual ServiceContext* getServiceContext() = 0;

    enum PrimaryCatchUpConclusionReason {
        kSucceeded,
        kAlreadyCaughtUp,
        kSkipped,
        kTimedOut,
        kFailedWithError,
        kFailedWithNewTerm,
        kFailedWithReplSetAbortPrimaryCatchUpCmd
    };

    /**
     * Abort catchup if the node is in catchup mode.
     */
    virtual Status abortCatchupIfNeeded(PrimaryCatchUpConclusionReason reason) = 0;

    /**
     * Increment the counter for the number of ops applied during catchup if the node is in catchup
     * mode.
     */
    virtual void incrementNumCatchUpOpsIfCatchingUp(long numOps) = 0;

    /**
     * Signals that drop pending collections have been removed from storage.
     */
    virtual void signalDropPendingCollectionsRemovedFromStorage() = 0;

    /**
     * Returns true if logOp() should not append an entry to the oplog for this operation.
     */
    bool isOplogDisabledFor(OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Returns true if logOp() should never append an entry to the oplog for this namespace. logOp()
     * may not want to append an entry to the oplog for other reasons, even if the namespace is
     * allowed to be replicated in the oplog (e.g. being a secondary).
     */
    static bool isOplogDisabledForNS(const NamespaceString& nss);


    /**
     * Returns the stable timestamp that the storage engine recovered to on startup. If the
     * recovery point was not stable, returns "none".
     */
    virtual boost::optional<Timestamp> getRecoveryTimestamp() = 0;

    /**
     * Returns true if the current replica set config has at least one arbiter.
     */
    virtual bool setContainsArbiter() const = 0;

    /**
     * Instructs the ReplicationCoordinator to recalculate the stable timestamp and advance it for
     * storage if needed.
     */
    virtual void attemptToAdvanceStableTimestamp() = 0;

    /**
     * If our state is RECOVERING and lastApplied is at least minValid, transition to SECONDARY.
     */
    virtual void finishRecoveryIfEligible(OperationContext* opCtx) = 0;

    /**
     * Field name of the newPrimaryMsg within the 'o' field in the new term oplog entry.
     */
    inline static constexpr StringData newPrimaryMsgField = "msg"_sd;

    /**
     * Message string passed in the new term oplog entry after a primary has stepped up.
     */
    inline static constexpr StringData newPrimaryMsg = "new primary"_sd;

    /*
     * Specifies the state transitions that kill user operations. Used for tracking state transition
     * metrics.
     */
    enum class OpsKillingStateTransitionEnum { kStepUp, kStepDown, kRollback };

    /**
     * Updates metrics around user ops when a state transition that kills user ops and select
     * internal operations occurs (i.e. step up, step down, or rollback). Also logs the metrics.
     */
    virtual void updateAndLogStateTransitionMetrics(
        ReplicationCoordinator::OpsKillingStateTransitionEnum stateTransition,
        size_t numOpsKilled,
        size_t numOpsRunning) const = 0;

    /**
     * Increment the server TopologyVersion and fulfill the promise of any currently waiting
     * hello request.
     */
    virtual void incrementTopologyVersion() = 0;

    /**
     * Constructs and returns a HelloResponse. Will block until the given deadline waiting for a
     * significant topology change if the 'counter' field of 'clientTopologyVersion' is equal to the
     * current TopologyVersion 'counter' from the TopologyCoordinator. Returns immediately if
     * 'clientTopologyVersion' < TopologyVersion of the TopologyCoordinator or if the processId
     * differs.
     */
    virtual std::shared_ptr<const HelloResponse> awaitHelloResponse(
        OperationContext* opCtx,
        const SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> clientTopologyVersion,
        boost::optional<Date_t> deadline) = 0;

    /**
     * The futurized version of `awaitHelloResponse()`:
     * * The future is ready for all cases that `awaitHelloResponse()` returns immediately.
     * * For cases that `awaitHelloResponse()` blocks, calling `get()` on the future is blocking.
     */
    virtual SharedSemiFuture<std::shared_ptr<const HelloResponse>> getHelloResponseFuture(
        const SplitHorizon::Parameters& horizonParams,
        boost::optional<TopologyVersion> clientTopologyVersion) = 0;

    /**
     * Returns the OpTime that consists of the timestamp of the latest oplog entry and the current
     * term.
     * This function returns a non-ok status if:
     * 1. It is called on secondaries.
     * 2. OperationContext times out or is interrupted.
     * 3. Oplog collection does not exist.
     * 4. Oplog collection is empty.
     * 5. Getting latest oplog timestamp is not supported by the storage engine.
     */
    virtual StatusWith<OpTime> getLatestWriteOpTime(OperationContext* opCtx) const noexcept = 0;

    /**
     * Returns the HostAndPort of the current primary, or an empty HostAndPort if there is no
     * primary. Note that the primary can change at any time and thus the result may be immediately
     * stale unless run from the primary with the RSTL held.
     */
    virtual HostAndPort getCurrentPrimaryHostAndPort() const = 0;

    /*
     * Cancels the callback referenced in the given callback handle.
     * This function expects the activeHandle to be valid.
     */
    virtual void cancelCbkHandle(executor::TaskExecutor::CallbackHandle activeHandle) = 0;

    using OnRemoteCmdScheduledFn = std::function<void(executor::TaskExecutor::CallbackHandle)>;
    using OnRemoteCmdCompleteFn = std::function<void(executor::TaskExecutor::CallbackHandle)>;
    /**
     * Runs the given command 'cmdObj' on primary and waits till the response for that command is
     * received. If the node is primary, then the command will be executed using DBDirectClient to
     * avoid tcp network calls. Otherwise, the node will execute the remote command using the repl
     * task executor (AsyncDBClient).
     * - 'OnRemoteCmdScheduled' will be called once the remote command is scheduled.
     * - 'OnRemoteCmdComplete' will be called once the response for the remote command is received.
     */
    virtual BSONObj runCmdOnPrimaryAndAwaitResponse(OperationContext* opCtx,
                                                    const std::string& dbName,
                                                    const BSONObj& cmdObj,
                                                    OnRemoteCmdScheduledFn onRemoteCmdScheduled,
                                                    OnRemoteCmdCompleteFn onRemoteCmdComplete) = 0;

    /**
     * A testing only function that cancels and reschedules replication heartbeats immediately.
     */
    virtual void restartScheduledHeartbeats_forTest() = 0;

    /**
     * Records if the cluster-wide write concern is set during sharding initialization.
     *
     * This function will assert if the shard can't talk to config server.
     */
    virtual void recordIfCWWCIsSetOnConfigServerOnStartup(OperationContext* opCtx) = 0;

    /**
     * Interface used to synchronize changes to custom write concern tags in the config and
     * custom default write concern settings.
     *
     * Use [reserve|release]DefaultWriteConcernChanges when making changes to the current
     * default read/write concern.
     * Use [reserve|release]ConfigWriteConcernTagChanges when executing a reconfig that
     * could potentially change read/write concern tags.
     */
    class WriteConcernTagChanges {
    public:
        WriteConcernTagChanges() = default;
        virtual ~WriteConcernTagChanges() = default;
        virtual bool reserveDefaultWriteConcernChange() = 0;
        virtual void releaseDefaultWriteConcernChange() = 0;

        virtual bool reserveConfigWriteConcernTagChange() = 0;
        virtual void releaseConfigWriteConcernTagChange() = 0;
    };

    virtual WriteConcernTagChanges* getWriteConcernTagChanges() = 0;

protected:
    ReplicationCoordinator();
};

}  // namespace repl
}  // namespace mongo
