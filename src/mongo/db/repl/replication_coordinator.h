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

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class IndexDescriptor;
class NamespaceString;
class OperationContext;
class ServiceContext;
class SnapshotName;
class Timestamp;
struct WriteConcernOptions;

namespace executor {
struct ConnectionPoolStats;
}  // namespace executor

namespace rpc {

class ReplSetMetadata;
class RequestInterface;
class ReplSetMetadata;

}  // namespace rpc

namespace repl {

class BackgroundSync;
class HandshakeArgs;
class IsMasterResponse;
class OldUpdatePositionArgs;
class OplogReader;
class OpTime;
class ReadConcernArgs;
class ReplicaSetConfig;
class ReplicationExecutor;
class ReplSetHeartbeatArgs;
class ReplSetHeartbeatArgsV1;
class ReplSetHeartbeatResponse;
class ReplSetHtmlSummary;
class ReplSetRequestVotesArgs;
class ReplSetRequestVotesResponse;
class UpdatePositionArgs;

/**
 * Global variable that contains a std::string telling why master/slave halted
 *
 * "dead" means something really bad happened like replication falling completely out of sync.
 * when non-null, we are dead and the string is informational
 *
 * TODO(dannenberg) remove when master slave goes
 */
extern const char* replAllDead;

/**
 * The ReplicationCoordinator is responsible for coordinating the interaction of replication
 * with the rest of the system.  The public methods on ReplicationCoordinator are the public
 * API that the replication subsystem presents to the rest of the codebase.
 */
class ReplicationCoordinator : public SyncSourceSelector {
    MONGO_DISALLOW_COPYING(ReplicationCoordinator);

public:
    static ReplicationCoordinator* get(ServiceContext* service);
    static ReplicationCoordinator* get(ServiceContext& service);
    static ReplicationCoordinator* get(OperationContext* ctx);

    static void set(ServiceContext* service,
                    std::unique_ptr<ReplicationCoordinator> replCoordinator);

    struct StatusAndDuration {
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
    virtual void startup(OperationContext* txn) = 0;

    /**
     * Does whatever cleanup is required to stop replication, including instructing the other
     * components of the replication system to shut down and stop any threads they are using,
     * blocking until all replication-related shutdown tasks are complete.
     */
    virtual void shutdown(OperationContext* txn) = 0;

    /**
     * Returns a pointer to the ReplicationExecutor.
     */
    virtual ReplicationExecutor* getExecutor() = 0;

    /**
     * Returns a reference to the parsed command line arguments that are related to replication.
     */
    virtual const ReplSettings& getSettings() const = 0;

    enum Mode { modeNone = 0, modeReplSet, modeMasterSlave };

    /**
     * Returns a value indicating whether this node was configured at start-up to run
     * standalone, as part of a master-slave pair, or as a member of a replica set.
     */
    virtual Mode getReplicationMode() const = 0;

    /**
     * Returns true if this node is configured to be a member of a replica set or master/slave
     * setup.
     */
    virtual bool isReplEnabled() const = 0;

    /**
     * Returns the current replica set state of this node (PRIMARY, SECONDARY, STARTUP, etc).
     * It is invalid to call this unless getReplicationMode() == modeReplSet.
     */
    virtual MemberState getMemberState() const = 0;

    /**
     * Waits for 'timeout' ms for member state to become 'state'.
     * Returns OK if member state is 'state'.
     * Returns ErrorCodes::ExceededTimeLimit if we timed out waiting for the state change.
     * Returns ErrorCodes::BadValue if timeout is negative.
     */
    virtual Status waitForMemberState(MemberState expectedState, Milliseconds timeout) = 0;

    /**
     * Returns true if this node is in state PRIMARY or SECONDARY.
     *
     * It is invalid to call this unless getReplicationMode() == modeReplSet.
     *
     * This method may be optimized to reduce synchronization overhead compared to
     * reading the current member state with getMemberState().
     */
    virtual bool isInPrimaryOrSecondaryState() const = 0;


    /**
     * Returns how slave delayed this node is configured to be, or 0 seconds if this node is not a
     * member of the current replica set configuration.
     */
    virtual Seconds getSlaveDelaySecs() const = 0;

    /**
     * Blocks the calling thread for up to writeConcern.wTimeout millis, or until "opTime" has
     * been replicated to at least a set of nodes that satisfies the writeConcern, whichever
     * comes first. A writeConcern.wTimeout of 0 indicates no timeout (block forever) and a
     * writeConcern.wTimeout of -1 indicates return immediately after checking. Return codes:
     * ErrorCodes::WriteConcernFailed if the writeConcern.wTimeout is reached before
     *     the data has been sufficiently replicated
     * ErrorCodes::ExceededTimeLimit if the txn->getMaxTimeMicrosRemaining is reached before
     *     the data has been sufficiently replicated
     * ErrorCodes::NotMaster if the node is not Primary/Master
     * ErrorCodes::UnknownReplWriteConcern if the writeConcern.wMode contains a write concern
     *     mode that is not known
     * ErrorCodes::ShutdownInProgress if we are mid-shutdown
     * ErrorCodes::Interrupted if the operation was killed with killop()
     */
    virtual StatusAndDuration awaitReplication(OperationContext* txn,
                                               const OpTime& opTime,
                                               const WriteConcernOptions& writeConcern) = 0;

    /**
     * Like awaitReplication(), above, but waits for the replication of the last operation
     * performed on the client associated with "txn".
     */
    virtual StatusAndDuration awaitReplicationOfLastOpForClient(
        OperationContext* txn, const WriteConcernOptions& writeConcern) = 0;

    /**
     * Causes this node to relinquish being primary for at least 'stepdownTime'.  If 'force' is
     * false, before doing so it will wait for 'waitTime' for one other node to be within 10
     * seconds of this node's optime before stepping down. Returns a Status with the code
     * ErrorCodes::ExceededTimeLimit if no secondary catches up within waitTime,
     * ErrorCodes::NotMaster if you are no longer primary when trying to step down,
     * ErrorCodes::SecondaryAheadOfPrimary if we are primary but there is another node that
     * seems to be ahead of us in replication, and Status::OK otherwise.
     */
    virtual Status stepDown(OperationContext* txn,
                            bool force,
                            const Milliseconds& waitTime,
                            const Milliseconds& stepdownTime) = 0;

    /**
     * Returns true if the node can be considered master for the purpose of introspective
     * commands such as isMaster() and rs.status().
     */
    virtual bool isMasterForReportingPurposes() = 0;

    /**
     * Returns true if it is valid for this node to accept writes on the given database.
     * Currently this is true only if this node is Primary, master in master/slave,
     * a standalone, or is writing to the local database.
     *
     * If a node was started with the replSet argument, but has not yet received a config, it
     * will not be able to receive writes to a database other than local (it will not be
     * treated as standalone node).
     *
     * NOTE: This function can only be meaningfully called while the caller holds the global
     * lock in some mode other than MODE_NONE.
     */
    virtual bool canAcceptWritesForDatabase(StringData dbName) = 0;

    /**
     * Returns true if it is valid for this node to accept writes on the given namespace.
     *
     * The result of this function should be consistent with canAcceptWritesForDatabase()
     * for the database the namespace refers to, with additional checks on the collection.
     */
    virtual bool canAcceptWritesFor(const NamespaceString& ns) = 0;

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
     * Returns Status::OK() if it is valid for this node to serve reads on the given collection
     * and an errorcode indicating why the node cannot if it cannot.
     */
    virtual Status checkCanServeReadsFor(OperationContext* txn,
                                         const NamespaceString& ns,
                                         bool slaveOk) = 0;

    /**
     * Returns true if this node should ignore unique index constraints on new documents.
     * Currently this is needed for nodes in STARTUP2, RECOVERING, and ROLLBACK states.
     */
    virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx) = 0;

    /**
     * Updates our internal tracking of the last OpTime applied for the given slave
     * identified by "rid".  Only valid to call in master/slave mode
     */
    virtual Status setLastOptimeForSlave(const OID& rid, const Timestamp& ts) = 0;

    /**
     * Updates our internal tracking of the last OpTime applied to this node.
     *
     * The new value of "opTime" must be no less than any prior value passed to this method, and
     * it is the caller's job to properly synchronize this behavior.  The exception to this rule
     * is that after calls to resetLastOpTimesFromOplog(), the minimum acceptable value for
     * "opTime" is reset based on the contents of the oplog, and may go backwards due to
     * rollback.
     */
    virtual void setMyLastAppliedOpTime(const OpTime& opTime) = 0;

    /**
     * Updates our internal tracking of the last OpTime durable to this node.
     *
     * The new value of "opTime" must be no less than any prior value passed to this method, and
     * it is the caller's job to properly synchronize this behavior.  The exception to this rule
     * is that after calls to resetLastOpTimesFromOplog(), the minimum acceptable value for
     * "opTime" is reset based on the contents of the oplog, and may go backwards due to
     * rollback.
     */
    virtual void setMyLastDurableOpTime(const OpTime& opTime) = 0;

    /**
     * Updates our internal tracking of the last OpTime applied to this node, but only
     * if the supplied optime is later than the current last OpTime known to the replication
     * coordinator.
     *
     * This function is used by logOp() on a primary, since the ops in the oplog do not
     * necessarily commit in sequential order.
     */
    virtual void setMyLastAppliedOpTimeForward(const OpTime& opTime) = 0;

    /**
     * Updates our internal tracking of the last OpTime durable to this node, but only
     * if the supplied optime is later than the current last OpTime known to the replication
     * coordinator.
     *
     * This function is used by logOp() on a primary, since the ops in the oplog do not
     * necessarily commit in sequential order.
     */
    virtual void setMyLastDurableOpTimeForward(const OpTime& opTime) = 0;

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

    /**
     * Returns the last optime recorded by setMyLastDurableOpTime.
     */
    virtual OpTime getMyLastDurableOpTime() const = 0;

    /**
     * Waits until the optime of the current node is at least the opTime specified in 'settings'.
     *
     * Returns whether the wait was successful.
     */
    virtual Status waitUntilOpTimeForRead(OperationContext* txn,
                                          const ReadConcernArgs& settings) = 0;

    /**
     * Retrieves and returns the current election id, which is a unique id that is local to
     * this node and changes every time we become primary.
     * TODO(spencer): Use term instead.
     */
    virtual OID getElectionId() = 0;

    /**
     * Returns the RID for this node.  The RID is used to identify this node to our sync source
     * when sending updates about our replication progress.
     */
    virtual OID getMyRID() const = 0;

    /**
     * Returns the id for this node as specified in the current replica set configuration.
     */
    virtual int getMyId() const = 0;

    /**
     * Sets this node into a specific follower mode.
     *
     * Returns true if the follower mode was successfully set.  Returns false if the
     * node is or becomes a leader before setFollowerMode completes.
     *
     * Follower modes are RS_STARTUP2 (initial sync), RS_SECONDARY, RS_ROLLBACK and
     * RS_RECOVERING.  They are the valid states of a node whose topology coordinator has the
     * follower role.
     *
     * This is essentially an interface that allows the applier to prevent the node from
     * becoming a candidate or accepting reads, depending on circumstances in the oplog
     * application process.
     */
    virtual bool setFollowerMode(const MemberState& newState) = 0;

    /**
     * Returns true if the coordinator wants the applier to pause application.
     *
     * If this returns true, the applier should call signalDrainComplete() when it has
     * completed draining its operation buffer and no further ops are being applied.
     */
    virtual bool isWaitingForApplierToDrain() = 0;

    /**
     * Signals that a previously requested pause and drain of the applier buffer
     * has completed.
     *
     * This is an interface that allows the applier to reenable writes after
     * a successful election triggers the draining of the applier buffer.
     */
    virtual void signalDrainComplete(OperationContext* txn) = 0;

    /**
     * Waits duration of 'timeout' for applier to finish draining its buffer of operations.
     * Returns OK if isWaitingForApplierToDrain() returns false.
     * Returns ErrorCodes::ExceededTimeLimit if we timed out waiting for the applier to drain its
     * buffer.
     * Returns ErrorCodes::BadValue if timeout is negative.
     */
    virtual Status waitForDrainFinish(Milliseconds timeout) = 0;

    /**
     * Signals the sync source feedback thread to wake up and send a handshake and
     * replSetUpdatePosition command to our sync source.
     */
    virtual void signalUpstreamUpdater() = 0;

    enum class ReplSetUpdatePositionCommandStyle {
        kNewStyle,
        kOldStyle  // Pre-3.2.4 servers.
    };

    /**
     * Prepares a BSONObj describing an invocation of the replSetUpdatePosition command that can
     * be sent to this node's sync source to update it about our progress in replication.
     */
    virtual StatusWith<BSONObj> prepareReplSetUpdatePositionCommand(
        ReplSetUpdatePositionCommandStyle commandStyle) const = 0;

    /**
     * Handles an incoming replSetGetStatus command. Adds BSON to 'result'.
     */
    virtual Status processReplSetGetStatus(BSONObjBuilder* result) = 0;

    /**
     * Handles an incoming isMaster command for a replica set node.  Should not be
     * called on a master-slave or standalone node.
     */
    virtual void fillIsMasterForReplSet(IsMasterResponse* result) = 0;

    /**
     * Adds to "result" a description of the slaveInfo data structure used to map RIDs to their
     * last known optimes.
     */
    virtual void appendSlaveInfoData(BSONObjBuilder* result) = 0;

    /**
     * Returns a copy of the current ReplicaSetConfig.
     */
    virtual ReplicaSetConfig getConfig() const = 0;

    /**
     * Handles an incoming replSetGetConfig command. Adds BSON to 'result'.
     */
    virtual void processReplSetGetConfig(BSONObjBuilder* result) = 0;

    /**
     * Processes the ReplSetMetadata returned from a command run against another replica set
     * member and updates protocol version 1 information (most recent optime that is committed,
     * member id of the current PRIMARY, the current config version and the current term).
     *
     * TODO(dannenberg): Move this method to be testing only if it does not end up being used
     * to process the find and getmore metadata responses from the DataReplicator.
     */
    virtual void processReplSetMetadata(const rpc::ReplSetMetadata& replMetadata) = 0;

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
     * PRIMARY, and OperationFailed if we are not currently in maintenance mode
     */
    virtual Status setMaintenanceMode(bool activate) = 0;

    /**
     * Retrieves the current count of maintenanceMode and returns 'true' if greater than 0.
     */
    virtual bool getMaintenanceMode() = 0;

    /**
     * Handles an incoming replSetSyncFrom command. Adds BSON to 'result'
     * returns Status::OK if the sync target could be set and an ErrorCode indicating why it
     * couldn't otherwise.
     */
    virtual Status processReplSetSyncFrom(const HostAndPort& target, BSONObjBuilder* resultObj) = 0;

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
    virtual Status processHeartbeat(const ReplSetHeartbeatArgs& args,
                                    ReplSetHeartbeatResponse* response) = 0;
    virtual Status processHeartbeatV1(const ReplSetHeartbeatArgsV1& args,
                                      ReplSetHeartbeatResponse* response) = 0;


    /**
     * Arguments for the replSetReconfig command.
     */
    struct ReplSetReconfigArgs {
        BSONObj newConfigObj;
        bool force;
    };

    /**
     * Handles an incoming replSetReconfig command. Adds BSON to 'resultObj';
     * returns a Status with either OK or an error message.
     */
    virtual Status processReplSetReconfig(OperationContext* txn,
                                          const ReplSetReconfigArgs& args,
                                          BSONObjBuilder* resultObj) = 0;

    /*
     * Handles an incoming replSetInitiate command. If "configObj" is empty, generates a default
     * configuration to use.
     * Adds BSON to 'resultObj'; returns a Status with either OK or an error message.
     */
    virtual Status processReplSetInitiate(OperationContext* txn,
                                          const BSONObj& configObj,
                                          BSONObjBuilder* resultObj) = 0;

    /*
     * Handles an incoming replSetGetRBID command.
     * Adds BSON to 'resultObj'; returns a Status with either OK or an error message.
     */
    virtual Status processReplSetGetRBID(BSONObjBuilder* resultObj) = 0;

    /**
     * Increments this process's rollback id.  Called every time a rollback occurs.
     */
    virtual void incrementRollbackID() = 0;

    /**
     * Arguments to the replSetFresh command.
     */
    struct ReplSetFreshArgs {
        std::string setName;  // Name of the replset
        HostAndPort who;      // host and port of the member that sent the replSetFresh command
        unsigned id;          // replSet id of the member that sent the replSetFresh command
        int cfgver;  // replSet config version that the member who sent the command thinks it has
        Timestamp opTime;  // last optime seen by the member who sent the replSetFresh command
    };

    /*
     * Handles an incoming replSetFresh command.
     * Adds BSON to 'resultObj'; returns a Status with either OK or an error message.
     */
    virtual Status processReplSetFresh(const ReplSetFreshArgs& args, BSONObjBuilder* resultObj) = 0;

    /**
     * Arguments to the replSetElect command.
     */
    struct ReplSetElectArgs {
        std::string set;  // Name of the replset
        int whoid;        // replSet id of the member that sent the replSetFresh command
        int cfgver;  // replSet config version that the member who sent the command thinks it has
        OID round;   // unique ID for this election
    };

    /*
     * Handles an incoming replSetElect command.
     * Adds BSON to 'resultObj'; returns a Status with either OK or an error message.
     */
    virtual Status processReplSetElect(const ReplSetElectArgs& args, BSONObjBuilder* resultObj) = 0;

    /**
     * Handles an incoming replSetUpdatePosition command, updating each node's oplog progress.
     * Returns Status::OK() if all updates are processed correctly, NodeNotFound
     * if any updating node cannot be found in the config, InvalidReplicaSetConfig if the
     * "configVersion" sent in any of the updates doesn't match our config version, or
     * NotMasterOrSecondary if we are in state REMOVED or otherwise don't have a valid
     * replica set config.
     * If a non-OK status is returned, it is unspecified whether none or some of the updates
     * were applied.
     * "configVersion" will be populated with our config version if and only if we return
     * InvalidReplicaSetConfig.
     *
     * The OldUpdatePositionArgs version provides support for the pre-3.2.4 format of
     * UpdatePositionArgs.
     */
    virtual Status processReplSetUpdatePosition(const OldUpdatePositionArgs& updates,
                                                long long* configVersion) = 0;
    virtual Status processReplSetUpdatePosition(const UpdatePositionArgs& updates,
                                                long long* configVersion) = 0;

    /**
     * Handles an incoming Handshake command. Associates the node's 'remoteID' with its
     * 'handshake' object. This association is used to update internal representation of
     * replication progress and to forward the node's replication progress upstream when this
     * node is being chained through in master/slave replication.
     *
     * Returns ErrorCodes::IllegalOperation if we're not running with master/slave replication.
     */
    virtual Status processHandshake(OperationContext* txn, const HandshakeArgs& handshake) = 0;

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
     * Returns a vector of the members other than ourself in the replica set, as specified in
     * the replica set config.  Invalid to call if we are not in replica set mode.  Returns
     * an empty vector if we do not have a valid config.
     */
    virtual std::vector<HostAndPort> getOtherNodesInReplSet() const = 0;

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
    virtual void resetLastOpTimesFromOplog(OperationContext* txn) = 0;

    /**
     * Returns the OpTime of the latest replica set-committed op known to this server.
     * Committed means a majority of the voting nodes of the config are known to have the
     * operation in their oplogs.  This implies such ops will never be rolled back.
     */
    virtual OpTime getLastCommittedOpTime() const = 0;

    /*
    * Handles an incoming replSetRequestVotes command.
    * Adds BSON to 'resultObj'; returns a Status with either OK or an error message.
    */
    virtual Status processReplSetRequestVotes(OperationContext* txn,
                                              const ReplSetRequestVotesArgs& args,
                                              ReplSetRequestVotesResponse* response) = 0;

    /**
     * Prepares a metadata object describing the current term, primary, and lastOp information.
     */
    virtual void prepareReplMetadata(const OpTime& lastOpTimeFromClient,
                                     BSONObjBuilder* builder) const = 0;

    /**
     * Returns true if the V1 election protocol is being used and false otherwise.
     */
    virtual bool isV1ElectionProtocol() const = 0;

    /**
     * Returns whether or not majority write concerns should implicitly journal, if j has not been
     * explicitly set.
     */
    virtual bool getWriteConcernMajorityShouldJournal() = 0;

    /**
     * Writes into 'output' all the information needed to generate a summary of the current
     * replication state for use by the web interface.
     */
    virtual void summarizeAsHtml(ReplSetHtmlSummary* output) = 0;

    /**
     * Returns the current term.
     */
    virtual long long getTerm() = 0;

    /**
     * Attempts to update the current term for the V1 election protocol. If the term changes and
     * this node is primary, relinquishes primary.
     * Returns a Status OK if the term was *not* updated (meaning, it is safe to proceed with
     * the rest of the work, because the term is still the same).
     * Returns StaleTerm if the supplied term was higher than the current term.
     */
    virtual Status updateTerm(OperationContext* txn, long long term) = 0;

    /**
     * Reserves a unique SnapshotName.
     *
     * This name is guaranteed to compare > all names reserved before and < all names reserved
     * after.
     *
     * This method will not take any locks or attempt to access storage using the passed-in
     * OperationContext. It will only be used to track reserved SnapshotNames by each operation so
     * that awaitReplicationOfLastOpForClient() can correctly wait for the reserved snapshot to be
     * visible.
     *
     * A null OperationContext can be used in cases where the snapshot to wait for should not be
     * adjusted.
     */
    virtual SnapshotName reserveSnapshotName(OperationContext* txn) = 0;

    /**
     * Signals the SnapshotThread, if running, to take a forced snapshot even if the global
     * timestamp hasn't changed.
     *
     * Does not wait for the snapshot to be taken.
     */
    virtual void forceSnapshotCreation() = 0;

    /**
     * Called when a new snapshot is created.
     */
    virtual void onSnapshotCreate(OpTime timeOfSnapshot, SnapshotName name) = 0;

    /**
     * Blocks until either the current committed snapshot is at least as high as 'untilSnapshot',
     * or we are interrupted for any reason, including shutdown or maxTimeMs expiration.
     * 'txn' is used to checkForInterrupt and enforce maxTimeMS.
     */
    virtual void waitUntilSnapshotCommitted(OperationContext* txn,
                                            const SnapshotName& untilSnapshot) = 0;

    /**
     * Resets all information related to snapshotting.
     */
    virtual void dropAllSnapshots() = 0;

    /**
     * Gets the latest OpTime of the currentCommittedSnapshot.
     */
    virtual OpTime getCurrentCommittedSnapshotOpTime() const = 0;

    /**
     * Appends connection information to the provided BSONObjBuilder.
     */
    virtual void appendConnectionStats(executor::ConnectionPoolStats* stats) const = 0;

    /**
     * Gets the number of uncommitted snapshots currently held.
     * Warning: This value can change at any time and may not even be accurate at the time of
     * return. It should not be used when an exact amount is needed.
     */
    virtual size_t getNumUncommittedSnapshots() = 0;

    /**
     * Returns a new WriteConcernOptions based on "wc" but with UNSET syncMode reset to JOURNAL or
     * NONE based on our rsConfig.
     */
    virtual WriteConcernOptions populateUnsetWriteConcernOptionsSyncMode(
        WriteConcernOptions wc) = 0;

    virtual bool getInitialSyncRequestedFlag() const = 0;
    virtual void setInitialSyncRequestedFlag(bool value) = 0;

    virtual ReplSettings::IndexPrefetchConfig getIndexPrefetchConfig() const = 0;
    virtual void setIndexPrefetchConfig(const ReplSettings::IndexPrefetchConfig cfg) = 0;

    virtual Status stepUpIfEligible() = 0;

protected:
    ReplicationCoordinator();
};

}  // namespace repl
}  // namespace mongo
