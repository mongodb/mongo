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

#include <memory>

#include "mongo/config.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_acquisition_stats.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

#ifndef MONGO_CONFIG_USE_RAW_LATCHES
#include "mongo/util/diagnostic_info.h"
#endif

namespace mongo {

class Client;
class CurOp;
class OperationContext;
struct PlanSummaryStats;

/* lifespan is different than CurOp because of recursives with DBDirectClient */
class OpDebug {
public:
    /**
     * Holds counters for execution statistics that are meaningful both for multi-statement
     * transactions and for individual operations outside of a transaction.
     */
    class AdditiveMetrics {
    public:
        AdditiveMetrics() = default;
        AdditiveMetrics(const AdditiveMetrics& other) {
            this->add(other);
        }

        AdditiveMetrics& operator=(const AdditiveMetrics& other) {
            reset();
            add(other);
            return *this;
        }

        /**
         * Adds all the fields of another AdditiveMetrics object together with the fields of this
         * AdditiveMetrics instance.
         */
        void add(const AdditiveMetrics& otherMetrics);

        /**
         * Resets all members to the default state.
         */
        void reset();

        /**
         * Returns true if the AdditiveMetrics object we are comparing has the same field values as
         * this AdditiveMetrics instance.
         */
        bool equals(const AdditiveMetrics& otherMetrics) const;

        /**
         * Increments writeConflicts by n.
         */
        void incrementWriteConflicts(long long n);

        /**
         * Increments temporarilyUnavailableErrors by n.
         */
        void incrementTemporarilyUnavailableErrors(long long n);

        /**
         * Increments keysInserted by n.
         */
        void incrementKeysInserted(long long n);

        /**
         * Increments keysDeleted by n.
         */
        void incrementKeysDeleted(long long n);

        /**
         * Increments ninserted by n.
         */
        void incrementNinserted(long long n);

        /**
         * Increments nUpserted by n.
         */
        void incrementNUpserted(long long n);

        /**
         * Increments prepareReadConflicts by n.
         */
        void incrementPrepareReadConflicts(long long n);

        /**
         * Generates a string showing all non-empty fields. For every non-empty field field1,
         * field2, ..., with corresponding values value1, value2, ..., we will output a string in
         * the format: "<field1>:<value1> <field2>:<value2> ...".
         */
        std::string report() const;
        BSONObj reportBSON() const;

        void report(logv2::DynamicAttributes* pAttrs) const;

        boost::optional<long long> keysExamined;
        boost::optional<long long> docsExamined;

        // Number of records that match the query.
        boost::optional<long long> nMatched;
        // Number of records written (no no-ops).
        boost::optional<long long> nModified;
        boost::optional<long long> ninserted;
        boost::optional<long long> ndeleted;
        boost::optional<long long> nUpserted;

        // Number of index keys inserted.
        boost::optional<long long> keysInserted;
        // Number of index keys removed.
        boost::optional<long long> keysDeleted;

        // The following fields are atomic because they are reported by CurrentOp. This is an
        // exception to the prescription that OpDebug only be used by the owning thread because
        // these metrics are tracked over the course of a transaction by SingleTransactionStats,
        // which is built on OpDebug.

        // Number of read conflicts caused by a prepared transaction.
        AtomicWord<long long> prepareReadConflicts{0};
        AtomicWord<long long> writeConflicts{0};
        AtomicWord<long long> temporarilyUnavailableErrors{0};
    };

    OpDebug() = default;

    void report(OperationContext* opCtx,
                const SingleThreadedLockStats* lockStats,
                const ResourceConsumption::OperationMetrics* operationMetrics,
                logv2::DynamicAttributes* pAttrs) const;

    /**
     * Appends information about the current operation to "builder"
     *
     * @param curop reference to the CurOp that owns this OpDebug
     * @param lockStats lockStats object containing locking information about the operation
     */
    void append(OperationContext* opCtx,
                const SingleThreadedLockStats& lockStats,
                FlowControlTicketholder::CurOp flowControlStats,
                BSONObjBuilder& builder) const;

    static std::function<BSONObj(ProfileFilter::Args args)> appendStaged(StringSet requestedFields,
                                                                         bool needWholeDocument);
    static void appendUserInfo(const CurOp&, BSONObjBuilder&, AuthorizationSession*);

    /**
     * Copies relevant plan summary metrics to this OpDebug instance.
     */
    void setPlanSummaryMetrics(const PlanSummaryStats& planSummaryStats);

    /**
     * The resulting object has zeros omitted. As is typical in this file.
     */
    static BSONObj makeFlowControlObject(FlowControlTicketholder::CurOp flowControlStats);

    /**
     * Make object from $search stats with non-populated values omitted.
     */
    BSONObj makeMongotDebugStatsObject() const;

    /**
     * Accumulate resolved views.
     */
    void addResolvedViews(const std::vector<NamespaceString>& namespaces,
                          const std::vector<BSONObj>& pipeline);

    /**
     * Get or append the array with resolved views' info.
     */
    BSONArray getResolvedViewsInfo() const;
    void appendResolvedViewsInfo(BSONObjBuilder& builder) const;

    // -------------------

    // basic options
    // _networkOp represents the network-level op code: OP_QUERY, OP_GET_MORE, OP_MSG, etc.
    NetworkOp networkOp{opInvalid};  // only set this through setNetworkOp_inlock() to keep synced
    // _logicalOp is the logical operation type, ie 'dbQuery' regardless of whether this is an
    // OP_QUERY find, a find command using OP_QUERY, or a find command using OP_MSG.
    // Similarly, the return value will be dbGetMore for both OP_GET_MORE and getMore command.
    LogicalOp logicalOp{LogicalOp::opInvalid};  // only set this through setNetworkOp_inlock()
    bool iscommand{false};

    // detailed options
    long long cursorid{-1};
    bool exhaust{false};

    // For search using mongot.
    boost::optional<long long> mongotCursorId{boost::none};
    boost::optional<long long> msWaitingForMongot{boost::none};
    long long mongotBatchNum = 0;

    bool hasSortStage{false};  // true if the query plan involves an in-memory sort

    bool usedDisk{false};  // true if the given query used disk

    // True if the plan came from the multi-planner (not from the plan cache and not a query with a
    // single solution).
    bool fromMultiPlanner{false};

    // True if a replan was triggered during the execution of this operation.
    boost::optional<std::string> replanReason;

    bool cursorExhausted{
        false};  // true if the cursor has been closed at end a find/getMore operation

    BSONObj execStats;  // Owned here.

    // The hash of the PlanCache key for the query being run. This may change depending on what
    // indexes are present.
    boost::optional<uint32_t> planCacheKey;
    // The hash of the query's "stable" key. This represents the query's shape.
    boost::optional<uint32_t> queryHash;

    // Has a value if this operation is a query. True if the execution tree for the find part of the
    // query was built using the classic query engine, false if it was built in SBE.
    boost::optional<bool> classicEngineUsed;

    // Has a value if this operation is an aggregation query. True if `DocumentSources` were
    // involved in the execution tree for this query, false if they were not.
    boost::optional<bool> documentSourceUsed;

    // Tracks whether an aggregation query has a lookup stage regardless of the engine used.
    bool pipelineUsesLookup{false};

    // Tracks the amount of indexed loop joins in a pushed down lookup stage.
    int indexedLoopJoin{0};

    // Tracks the amount of nested loop joins in a pushed down lookup stage.
    int nestedLoopJoin{0};

    // Tracks the amount of hash lookups in a pushed down lookup stage.
    int hashLookup{0};

    // Tracks the amount of spills by hash lookup in a pushed down lookup stage.
    int hashLookupSpillToDisk{0};

    // Details of any error (whether from an exception or a command returning failure).
    Status errInfo = Status::OK();

    // response info
    Microseconds executionTime{0};
    long long nreturned{-1};
    int responseLength{-1};

    // Shard targeting info.
    int nShards{-1};

    // Stores the duration of time spent blocked on prepare conflicts.
    Milliseconds prepareConflictDurationMillis{0};

    // Total time spent looking up database entry in the local catalog cache, including eventual
    // refreshes.
    Milliseconds catalogCacheDatabaseLookupMillis{0};

    // Total time spent looking up collection entry in the local catalog cache, including eventual
    // refreshes.
    Milliseconds catalogCacheCollectionLookupMillis{0};

    // Stores the duration of time spent waiting for the shard to refresh the database and wait for
    // the database critical section.
    Milliseconds databaseVersionRefreshMillis{0};

    // Stores the duration of time spent waiting for the shard to refresh the collection and wait
    // for the collection critical section.
    Milliseconds shardVersionRefreshMillis{0};

    // Stores the amount of the data processed by the throttle cursors in MB/sec.
    boost::optional<float> dataThroughputLastSecond;
    boost::optional<float> dataThroughputAverage;

    // Used to track the amount of time spent waiting for a response from remote operations.
    boost::optional<Microseconds> remoteOpWaitTime;

    // Stores additive metrics.
    AdditiveMetrics additiveMetrics;

    // Stores storage statistics.
    std::shared_ptr<StorageStats> storageStats;

    bool waitingForFlowControl{false};

    // Records the WC that was waited on during the operation. (The WC in opCtx can't be used
    // because it's only set while the Command itself executes.)
    boost::optional<WriteConcernOptions> writeConcern;

    // Whether this is an oplog getMore operation for replication oplog fetching.
    bool isReplOplogGetMore{false};

    // Maps namespace of a resolved view to its dependency chain and the fully unrolled pipeline. To
    // make log line deterministic and easier to test, use ordered map. As we don't expect many
    // resolved views per query, a hash map would unlikely provide any benefits.
    std::map<NamespaceString, std::pair<std::vector<NamespaceString>, std::vector<BSONObj>>>
        resolvedViews;
};

/**
 * Container for data used to report information about an OperationContext.
 *
 * Every OperationContext in a server with CurOp support has a stack of CurOp
 * objects. The entry at the top of the stack is used to record timing and
 * resource statistics for the executing operation or suboperation.
 *
 * All of the accessor methods on CurOp may be called by the thread executing
 * the associated OperationContext at any time, or by other threads that have
 * locked the context's owning Client object.
 *
 * The mutator methods on CurOp whose names end _inlock may only be called by the thread
 * executing the associated OperationContext and Client, and only when that thread has also
 * locked the Client object.  All other mutators may only be called by the thread executing
 * CurOp, but do not require holding the Client lock.  The exception to this is the kill()
 * method, which is self-synchronizing.
 *
 * The OpDebug member of a CurOp, accessed via the debug() accessor should *only* be accessed
 * from the thread executing an operation, and as a result its fields may be accessed without
 * any synchronization.
 */
class CurOp {
    CurOp(const CurOp&) = delete;
    CurOp& operator=(const CurOp&) = delete;

public:
    static CurOp* get(const OperationContext* opCtx);
    static CurOp* get(const OperationContext& opCtx);

    /**
     * Writes a report of the operation being executed by the given client to the supplied
     * BSONObjBuilder, in a format suitable for display in currentOp. Does not include a lockInfo
     * report, since this may be called in either a mongoD or mongoS context and the latter does not
     * supply lock stats. The client must be locked before calling this method.
     */
    static void reportCurrentOpForClient(OperationContext* opCtx,
                                         Client* client,
                                         bool truncateOps,
                                         bool backtraceMode,
                                         BSONObjBuilder* infoBuilder);

    /**
     * Serializes the fields of a GenericCursor which do not appear elsewhere in the currentOp
     * output. If 'maxQuerySize' is given, truncates the cursor's originatingCommand but preserves
     * the comment.
     */
    static BSONObj truncateAndSerializeGenericCursor(GenericCursor* cursor,
                                                     boost::optional<size_t> maxQuerySize);

    /**
     * Constructs a nested CurOp at the top of the given "opCtx"'s CurOp stack.
     */
    explicit CurOp(OperationContext* opCtx);
    ~CurOp();

    /**
     * Fills out CurOp and OpDebug with basic info common to all commands. We require the NetworkOp
     * in order to distinguish which protocol delivered this request, e.g. OP_QUERY or OP_MSG. This
     * is set early in the request processing backend and does not typically need to be called
     * thereafter. Locks the client as needed to apply the specified settings.
     */
    void setGenericOpRequestDetails(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const Command* command,
                                    BSONObj cmdObj,
                                    NetworkOp op);

    /**
     * Marks the operation end time, records the length of the client response if a valid response
     * exists, and then - subject to the current values of slowMs and sampleRate - logs this CurOp
     * to file under the given LogComponent. Returns 'true' if, in addition to being logged, this
     * operation should also be profiled.
     */
    bool completeAndLogOperation(OperationContext* opCtx,
                                 logv2::LogComponent logComponent,
                                 boost::optional<size_t> responseLength = boost::none,
                                 boost::optional<long long> slowMsOverride = boost::none,
                                 bool forceLog = false);

    bool haveOpDescription() const {
        return !_opDescription.isEmpty();
    }

    /**
     * The BSONObj returned may not be owned by CurOp. Callers should call getOwned() if they plan
     * to reference beyond the lifetime of this CurOp instance.
     */
    BSONObj opDescription() const {
        return _opDescription;
    }

    /**
     * Returns an owned BSONObj representing the original command. Used only by the getMore
     * command.
     */
    BSONObj originatingCommand() const {
        return _originatingCommand;
    }

    void enter_inlock(const char* ns, int dbProfileLevel);

    /**
     * Sets the type of the current network operation.
     */
    void setNetworkOp_inlock(NetworkOp op) {
        _networkOp = op;
        _debug.networkOp = op;
    }

    /**
     * Sets the type of the current logical operation.
     */
    void setLogicalOp_inlock(LogicalOp op) {
        _logicalOp = op;
        _debug.logicalOp = op;
    }

    /**
     * Marks the current operation as being a command.
     */
    void markCommand_inlock() {
        _isCommand = true;
    }

    /**
     * Returns a structure containing data used for profiling, accessed only by a thread
     * currently executing the operation context associated with this CurOp.
     */
    OpDebug& debug() {
        return _debug;
    }

    /**
     * Gets the name of the namespace on which the current operation operates.
     */
    std::string getNS() const {
        return _ns;
    }

    /**
     * Returns a const pointer to the UserAcquisitionStats for the current operation.
     * This can only be used for reading (i.e., when logging or profiling).
     */
    const UserAcquisitionStats* getReadOnlyUserAcquisitionStats() const {
        return &_userAcquisitionStats;
    }

    /**
     * Returns a non-const raw pointers to UserAcquisitionStats member.
     */
    UserAcquisitionStats* getMutableUserAcquisitionStats() {
        return &_userAcquisitionStats;
    }

    /**
     * Gets the name of the namespace on which the current operation operates.
     */
    NamespaceString getNSS() const {
        return NamespaceString{_ns};
    }

    /**
     * Returns true if the elapsed time of this operation is such that it should be profiled or
     * profile level is set to 2. Uses total time if the operation is done, current elapsed time
     * otherwise.
     *
     * When a custom filter is set, we conservatively assume it would match this operation.
     */
    bool shouldDBProfile(OperationContext* opCtx) {
        // Profile level 2 should override any sample rate or slowms settings.
        if (_dbprofile >= 2)
            return true;

        if (_dbprofile <= 0)
            return false;

        if (CollectionCatalog::get(opCtx)->getDatabaseProfileSettings(getNSS().db()).filter)
            return true;

        return elapsedTimeExcludingPauses() >= Milliseconds{serverGlobalParams.slowMS};
    }

    /**
     * Raises the profiling level for this operation to "dbProfileLevel" if it was previously
     * less than "dbProfileLevel".
     *
     * This belongs on OpDebug, and so does not have the _inlock suffix.
     */
    void raiseDbProfileLevel(int dbProfileLevel);

    int dbProfileLevel() const {
        return _dbprofile;
    }

    /**
     * Gets the network operation type. No lock is required if called by the thread executing
     * the operation, but the lock must be held if called from another thread.
     */
    NetworkOp getNetworkOp() const {
        return _networkOp;
    }

    /**
     * Gets the logical operation type. No lock is required if called by the thread executing
     * the operation, but the lock must be held if called from another thread.
     */
    LogicalOp getLogicalOp() const {
        return _logicalOp;
    }

    /**
     * Returns true if the current operation is known to be a command.
     */
    bool isCommand() const {
        return _isCommand;
    }

    //
    // Methods for getting/setting elapsed time. Note that the observed elapsed time may be
    // negative, if the system time has been reset during the course of this operation.
    //

    void ensureStarted() {
        static_cast<void>(startTime());
    }
    bool isStarted() const {
        return _start.load() != 0;
    }
    void done();
    bool isDone() const {
        return _end > 0;
    }
    bool isPaused() {
        return _lastPauseTime != 0;
    }

    /**
     * Stops the operation latency timer from "ticking". Time spent paused is not included in the
     * latencies returned by elapsedTimeExcludingPauses().
     *
     * Illegal to call if either the CurOp has not been started, or the CurOp is already in a paused
     * state.
     */
    void pauseTimer() {
        invariant(isStarted());
        invariant(_lastPauseTime == 0);
        _lastPauseTime = _tickSource->getTicks();
    }

    /**
     * Starts the operation latency timer "ticking" again. Illegal to call if the CurOp has not been
     * started and then subsequently paused.
     */
    void resumeTimer() {
        invariant(isStarted());
        invariant(_lastPauseTime > 0);
        _totalPausedDuration +=
            _tickSource->ticksTo<Microseconds>(_tickSource->getTicks() - _lastPauseTime);
        _lastPauseTime = 0;
    }

    /**
     * Ensures that remoteOpWait will be recorded in the OpDebug.
     *
     * This method is separate from startRemoteOpWait because operation types that do record
     * remoteOpWait, such as a getMore of a sharded aggregation, should always include the
     * remoteOpWait field even if its value is zero. An operation should call
     * enableRecordRemoteOpWait() to declare that it wants to report remoteOpWait, and call
     * startRemoteOpWaitTimer()/stopRemoteOpWaitTimer() to measure the time.
     *
     * This timer uses the same clock source as elapsedTimeTotal().
     */
    void enableRecordRemoteOpWait() {
        if (!_debug.remoteOpWaitTime) {
            _debug.remoteOpWaitTime.emplace(0);
        }
    }

    /**
     * Starts the remoteOpWait timer.
     *
     * Does nothing if enableRecordRemoteOpWait() was not called.
     */
    void startRemoteOpWaitTimer() {
        invariant(isStarted());
        invariant(!isDone());
        invariant(!isPaused());
        invariant(!_remoteOpStartTime);
        if (_debug.remoteOpWaitTime) {
            _remoteOpStartTime.emplace(elapsedTimeTotal());
        }
    }

    /**
     * Stops the remoteOpWait timer.
     *
     * Does nothing if enableRecordRemoteOpWait() was not called.
     */
    void stopRemoteOpWaitTimer() {
        invariant(isStarted());
        invariant(!isDone());
        invariant(!isPaused());
        if (_debug.remoteOpWaitTime) {
            Microseconds end = elapsedTimeTotal();
            invariant(_remoteOpStartTime);
            // On most systems a monotonic clock source will be used to measure time. When a
            // monotonic clock is not available we fallback to using the realtime system clock. When
            // used, a backward shift of the realtime system clock could lead to a negative delta.
            Microseconds delta = std::max((end - *_remoteOpStartTime), Microseconds{0});
            *_debug.remoteOpWaitTime += delta;
            _remoteOpStartTime = boost::none;
        }
        invariant(!_remoteOpStartTime);
    }

    /**
     * If this op has been marked as done(), returns the wall clock duration between being marked as
     * started with ensureStarted() and the call to done().
     *
     * Otherwise, returns the wall clock duration between the start time and now.
     *
     * If this op has not yet been started, returns 0.
     */
    Microseconds elapsedTimeTotal() {
        auto start = _start.load();
        if (start == 0) {
            return Microseconds{0};
        }

        return computeElapsedTimeTotal(start, _end.load());
    }

    /**
     * Returns the total elapsed duration minus any time spent in a paused state. See
     * elapsedTimeTotal() for the definition of the total duration and pause/resumeTimer() for
     * details on pausing.
     *
     * If this op has not yet been started, returns 0.
     *
     * Illegal to call while the timer is paused.
     */
    Microseconds elapsedTimeExcludingPauses() {
        invariant(!_lastPauseTime);

        auto start = _start.load();
        if (start == 0) {
            return Microseconds{0};
        }

        return computeElapsedTimeTotal(start, _end.load()) - _totalPausedDuration;
    }

    /**
     * 'opDescription' must be either an owned BSONObj or guaranteed to outlive the OperationContext
     * it is associated with.
     */
    void setOpDescription_inlock(const BSONObj& opDescription) {
        _opDescription = opDescription;
    }

    /**
     * Sets the original command object.
     */
    void setOriginatingCommand_inlock(const BSONObj& commandObj) {
        _originatingCommand = commandObj.getOwned();
    }

    const Command* getCommand() const {
        return _command;
    }
    void setCommand_inlock(const Command* command) {
        _command = command;
    }

    /**
     * Returns whether the current operation is a read, write, or command.
     */
    Command::ReadWriteType getReadWriteType() const;

    /**
     * Appends information about this CurOp to "builder". If "truncateOps" is true, appends a string
     * summary of any objects which exceed the threshold size. If truncateOps is false, append the
     * entire object.
     *
     * If called from a thread other than the one executing the operation associated with this
     * CurOp, it is necessary to lock the associated Client object before executing this method.
     */
    void reportState(OperationContext* opCtx, BSONObjBuilder* builder, bool truncateOps = false);

    /**
     * Sets the message for FailPoints used.
     */
    void setFailPointMessage_inlock(StringData message) {
        _failPointMessage = message.toString();
    }

    /**
     * Sets the message for this CurOp.
     */
    void setMessage_inlock(StringData message);

    /**
     * Sets the message and the progress meter for this CurOp.
     *
     * While it is necessary to hold the lock while this method executes, the
     * "hit" and "finished" methods of ProgressMeter may be called safely from
     * the thread executing the operation without locking the Client.
     */
    ProgressMeter& setProgress_inlock(StringData name,
                                      unsigned long long progressMeterTotal = 0,
                                      int secondsBetween = 3);

    /*
     * Gets the message for FailPoints used.
     */
    const std::string& getFailPointMessage() const {
        return _failPointMessage;
    }

    /**
     * Gets the message for this CurOp.
     */
    const std::string& getMessage() const {
        return _message;
    }
    const ProgressMeter& getProgressMeter() {
        return _progressMeter;
    }
    CurOp* parent() const {
        return _parent;
    }
    boost::optional<GenericCursor> getGenericCursor_inlock() const {
        return _genericCursor;
    }

    void yielded(int numYields = 1) {
        _numYields.fetchAndAdd(numYields);
    }

    /**
     * Returns the number of times yielded() was called.  Callers on threads other
     * than the one executing the operation must lock the client.
     */
    int numYields() const {
        return _numYields.load();
    }

    /**
     * this should be used very sparingly
     * generally the Context should set this up
     * but sometimes you want to do it ahead of time
     */
    void setNS_inlock(StringData ns);

    StringData getPlanSummary() const {
        return _planSummary;
    }

    void setPlanSummary_inlock(StringData summary) {
        _planSummary = summary.toString();
    }

    void setPlanSummary_inlock(std::string summary) {
        _planSummary = std::move(summary);
    }

    void setGenericCursor_inlock(GenericCursor gc);

    boost::optional<SingleThreadedLockStats> getLockStatsBase() const {
        return _lockStatsBase;
    }

    void setTickSource_forTest(TickSource* tickSource) {
        _tickSource = tickSource;
    }

    /**
     * Merge match counters from the current operation into the global map and stop counting.
     */
    void stopMatchExprCounter();

    /**
     * Increment the counter for the match expression with given name in the current operation.
     */
    void incrementMatchExprCounter(StringData name);

private:
    class CurOpStack;

    TickSource::Tick startTime();
    Microseconds computeElapsedTimeTotal(TickSource::Tick startTime,
                                         TickSource::Tick endTime) const;

    /**
     * Adds 'this' to the stack of active CurOp objects.
     */
    void _finishInit(OperationContext* opCtx, CurOpStack* stack);

    /**
     * Handles failpoints that check whether a command has completed or not.
     * Used for testing purposes instead of the getLog command.
     */
    void _checkForFailpointsAfterCommandLogged();

    static const OperationContext::Decoration<CurOpStack> _curopStack;

    CurOp(OperationContext*, CurOpStack*);

    CurOpStack* _stack;
    CurOp* _parent{nullptr};
    const Command* _command{nullptr};

    // The time at which this CurOp instance was marked as started.
    std::atomic<TickSource::Tick> _start{0};  // NOLINT

    // The time at which this CurOp instance was marked as done or 0 if the CurOp is not yet done.
    std::atomic<TickSource::Tick> _end{0};  // NOLINT

    // The time at which this CurOp instance had its timer paused, or 0 if the timer is not
    // currently paused.
    TickSource::Tick _lastPauseTime{0};

    // The cumulative duration for which the timer has been paused.
    Microseconds _totalPausedDuration{0};

    // The elapsedTimeTotal() value at which the remoteOpWait timer was started, or empty if the
    // remoteOpWait timer is not currently running.
    boost::optional<Microseconds> _remoteOpStartTime;

    // _networkOp represents the network-level op code: OP_QUERY, OP_GET_MORE, OP_MSG, etc.
    NetworkOp _networkOp{opInvalid};  // only set this through setNetworkOp_inlock() to keep synced
    // _logicalOp is the logical operation type, ie 'dbQuery' regardless of whether this is an
    // OP_QUERY find, a find command using OP_QUERY, or a find command using OP_MSG.
    // Similarly, the return value will be dbGetMore for both OP_GET_MORE and getMore command.
    LogicalOp _logicalOp{LogicalOp::opInvalid};  // only set this through setNetworkOp_inlock()

    bool _isCommand{false};
    int _dbprofile{0};  // 0=off, 1=slow, 2=all
    std::string _ns;
    BSONObj _opDescription;
    BSONObj _originatingCommand;  // Used by getMore to display original command.
    OpDebug _debug;
    std::string _failPointMessage;  // Used to store FailPoint information.
    std::string _message;
    ProgressMeter _progressMeter;
    AtomicWord<int> _numYields{0};
    // A GenericCursor containing information about the active cursor for a getMore operation.
    boost::optional<GenericCursor> _genericCursor;

    std::string _planSummary;
    boost::optional<SingleThreadedLockStats>
        _lockStatsBase;  // This is the snapshot of lock stats taken when curOp is constructed.

    UserAcquisitionStats _userAcquisitionStats;

    TickSource* _tickSource = nullptr;
};

}  // namespace mongo
