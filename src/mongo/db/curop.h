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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_acquisition_stats.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/flow_control_ticketholder.h"
#include "mongo/db/local_catalog/lock_manager/lock_stats.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_debug.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_cpu_timer.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/client_cursor/generic_cursor_gen.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_stats/data_bearing_node_metrics.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_stats.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/string_map.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo {

class Client;
class OperationContext;
struct PlanSummaryStats;

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
 * The mutator methods on CurOp who take in a lock as the first argument may only be called by
 * the thread executing the associated OperationContext and Client, and only when that thread has
 * also locked the Client object.  All other mutators may only be called by the thread executing
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
    static void reportCurrentOpForClient(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         Client* client,
                                         bool truncateOps,
                                         BSONObjBuilder* infoBuilder);

    static bool currentOpBelongsToTenant(Client* client, TenantId tenantId);

    /**
     * Serializes the fields of a GenericCursor which do not appear elsewhere in the currentOp
     * output. If 'maxQuerySize' is given, truncates the cursor's originatingCommand but preserves
     * the comment.
     */
    static BSONObj truncateAndSerializeGenericCursor(GenericCursor cursor,
                                                     boost::optional<size_t> maxQuerySize);

    // Convenience helpers for testing metrics that are tracked here.
    static Counter64& totalInterruptChecks_forTest();
    static Counter64& opsWithOverdueInterruptCheck_forTest();

    /**
     * Pushes this CurOp to the top of the given "opCtx"'s CurOp stack.
     */
    void push(OperationContext* opCtx);

    CurOp() = default;

    /**
     * This allows the caller to set the command on the CurOp without using setCommand and
     * having to acquire the Client lock or having to leave a comment indicating why the
     * client lock isn't necessary.
     */
    explicit CurOp(const Command* command) : _command{command} {}

    ~CurOp();

    /**
     * Fills out CurOp and OpDebug with basic info common to all commands. We require the NetworkOp
     * in order to distinguish which protocol delivered this request, e.g. OP_QUERY or OP_MSG. This
     * is set early in the request processing backend and does not typically need to be called
     * thereafter. It is necessary to hold the Client lock while this method executes.
     */
    void setGenericOpRequestDetails(
        WithLock, NamespaceString nss, const Command* command, BSONObj cmdObj, NetworkOp op);

    /**
     * Sets metrics collected at the end of an operation onto curOp's OpDebug instance. Note that
     * this is used in tandem with OpDebug::setPlanSummaryMetrics so should not repeat any metrics
     * collected there.
     */
    void setEndOfOpMetrics(long long nreturned);

    /**
     * Marks the operation end time, records the length of the client response if a valid response
     * exists, and then - subject to the current values of slowMs and sampleRate - logs this CurOp
     * to file under the given LogComponent. Returns 'true' if, in addition to being logged, this
     * operation should also be profiled.
     */
    bool completeAndLogOperation(const logv2::LogOptions& logOptions,
                                 std::shared_ptr<const ProfileFilter> filter,
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

    void enter(WithLock, NamespaceString nss, int dbProfileLevel);
    void enter(WithLock lk, const DatabaseName& dbName, int dbProfileLevel);

    /**
     * Sets the type of the current network operation.
     */
    void setNetworkOp(WithLock, NetworkOp op) {
        _networkOp = op;
        _debug.networkOp = op;
    }

    /**
     * Sets the type of the current logical operation.
     */
    void setLogicalOp(WithLock, LogicalOp op) {
        _logicalOp = op;
        _debug.logicalOp = op;
    }

    /**
     * Marks the current operation as being a command.
     */
    void markCommand(WithLock) {
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
    std::string getNS() const;

    /**
     * Returns a non-const copy of the UserAcquisitionStats shared_ptr. The caller takes shared
     * ownership of the userAcquisitionStats.
     */
    SharedUserAcquisitionStats getUserAcquisitionStats() const {
        return _userAcquisitionStats;
    }

    /**
     * Gets the name of the namespace on which the current operation operates.
     */
    const NamespaceString& getNSS() const {
        return _nss;
    }

    /**
     * Returns true if the elapsed time of this operation is such that it should be profiled or
     * profile level is set to 2. Uses total time if the operation is done, current elapsed time
     * otherwise.
     *
     * When a custom filter is set, we conservatively assume it would match this operation.
     */
    bool shouldDBProfile();

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
        (void)startTime();
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
     * ensureRecordRemoteOpWait() to declare that it wants to report remoteOpWait, and call
     * startRemoteOpWaitTimer()/stopRemoteOpWaitTimer() to measure the time.
     *
     * This timer uses the same clock source as elapsedTimeTotal().
     */
    void ensureRecordRemoteOpWait() {
        if (!_debug.remoteOpWaitTime) {
            _debug.remoteOpWaitTime.emplace(0);
        }
    }

    /**
     * Starts the remoteOpWait timer.
     *
     * Does nothing if ensureRecordRemoteOpWait() was not called or the current operation was not
     * marked as started.
     */
    void startRemoteOpWaitTimer() {
        // There are some commands that send remote operations but do not mark the current operation
        // as started. We do not record remote op wait time for those commands.
        if (!isStarted()) {
            return;
        }
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
     * Does nothing if ensureRecordRemoteOpWait() was not called or the current operation was not
     * marked as started.
     */
    void stopRemoteOpWaitTimer() {
        // There are some commands that send remote operations but do not mark the current operation
        // as started. We do not record remote op wait time for those commands.
        if (!isStarted()) {
            return;
        }
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
    Microseconds elapsedTimeTotal() const {
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
     * The planningTimeMicros metric, reported in the system profiler and in queryStats, is measured
     * using the Curop instance's _tickSource. Currently, _tickSource is only paused in places where
     * logical work is being done. If this were to change, and _tickSource were to be paused during
     * query planning for reasons unrelated to the work of planning/optimization, it would break the
     * planning time measurement below.
     */
    void beginQueryPlanningTimer() {
        // If we've already started the query planning timer, we could be processing a command that
        // is being retried. It's also possible that we're processing a command on a view that has
        // been rewritten to an aggregation. To handle the former case, reset the start time here,
        // even though it means excluding view-related work from the query planning timer.
        _queryPlanningStart = _tickSource->getTicks();
    }

    void stopQueryPlanningTimer() {
        // The planningTime metric is defined as being done once PrepareExecutionHelper::prepare()
        // is hit, which calls this function to stop the timer. As certain queries like $lookup
        // require inner cursors/executors that will follow this same codepath, it is important to
        // make sure the metric exclusively captures the time associated with the outermost cursor.
        // This is done by making sure planningTime has not already been set and that start has been
        // marked (as inner executors are prepared outside of the codepath that begins the planning
        // timer).
        auto start = _queryPlanningStart.load();
        if (debug().planningTime == Microseconds{0} && start != 0) {
            _queryPlanningEnd = _tickSource->getTicks();
            debug().planningTime = computeElapsedTimeTotal(start, _queryPlanningEnd.load());
        }
    }

    /**
     * Starts the waitForWriteConcern timer.
     *
     * The timer must be ended before it can be started again.
     */
    void beginWaitForWriteConcernTimer() {
        invariant(_waitForWriteConcernStart.load() == 0);
        _waitForWriteConcernStart = _tickSource->getTicks();
        _waitForWriteConcernEnd = 0;
    }

    /**
     * Stops the waitForWriteConcern timer.
     *
     * Does nothing if the timer has not been started.
     */
    void stopWaitForWriteConcernTimer() {
        auto start = _waitForWriteConcernStart.load();
        if (start != 0) {
            _waitForWriteConcernEnd = _tickSource->getTicks();
            auto duration = duration_cast<Milliseconds>(
                computeElapsedTimeTotal(start, _waitForWriteConcernEnd.load()));
            _atomicWaitForWriteConcernDurationMillis =
                _atomicWaitForWriteConcernDurationMillis.load() + duration;
            debug().waitForWriteConcernDurationMillis = _atomicWaitForWriteConcernDurationMillis;
            _waitForWriteConcernStart = 0;
        }
    }

    /**
     * If the platform supports the CPU timer, and we haven't collected this operation's CPU time
     * already, then calculates this operation's CPU time and stores it on the 'OpDebug'.
     */
    void calculateCpuTime();

    /**
     * 'opDescription' must be either an owned BSONObj or guaranteed to outlive the OperationContext
     * it is associated with.
     */
    void setOpDescription(WithLock, const BSONObj& opDescription);

    /**
     * Sets the original command object.
     */
    void setOriginatingCommand(WithLock, const BSONObj& commandObj) {
        _originatingCommand = commandObj.getOwned();
    }

    const Command* getCommand() const {
        return _command;
    }
    void setCommand(WithLock, const Command* command) {
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
    void reportState(BSONObjBuilder* builder,
                     const SerializationContext& serializationContext,
                     bool truncateOps = false);

    /**
     * Sets the message for FailPoints used.
     */
    void setFailPointMessage(WithLock, StringData message) {
        _failPointMessage = std::string{message};
    }

    /**
     * Sets the message for this CurOp.
     */
    void setMessage(WithLock lk, StringData message);

    /**
     * Sets the message and the progress meter for this CurOp.
     *
     * Accessors and modifiers of ProgressMeter associated with the CurOp must follow the same
     * locking scheme as CurOp. It is necessary to hold the lock while this method executes.
     */
    ProgressMeter& setProgress(WithLock,
                               StringData name,
                               unsigned long long progressMeterTotal = 0,
                               int secondsBetween = 3);

    /**
     * Captures stats on the locker and recovery unit after transaction resources are unstashed to
     * the operation context to be able to correctly ignore stats from outside this CurOp instance.
     * Assumes that operation will only unstash transaction resources once. Requires holding the
     * client lock.
     */
    void updateStatsOnTransactionUnstash(ClientLock&);

    /**
     * Captures stats on the locker and recovery unit that happened during this CurOp instance
     * before transaction resources are stashed. Also cleans up stats taken when transaction
     * resources were unstashed. Assumes that operation will only stash transaction resources once.
     * Requires holding the client lock.
     */
    void updateStatsOnTransactionStash(ClientLock&);

    /**
     * Sets the current and max used memory for this CurOp instance.
     *
     * Callers do not need to hold the client lock because CurOp's memory tracking metrics are
     * wrapped in atomics. The intent of this is to improve performance. The other mutators in CurOp
     * must hold the client lock, but updates to memory usage may be frequent and cause contention
     * on the client lock.
     */
    void setMemoryTrackingStats(int64_t inUseTrackedMemBytes, int64_t peakTrackedMemBytes);

    int64_t getInUseTrackedMemoryBytes() const {
        return _inUseTrackedMemoryBytes.load();
    }

    int64_t getPeakTrackedMemoryBytes() const {
        return _peakTrackedMemoryBytes.load();
    }

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

    CurOp* parent() const {
        return _parent;
    }

    bool isTop() const {
        return parent() == nullptr;
    }

    boost::optional<GenericCursor> getGenericCursor(ClientLock) const {
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
    void setNS(WithLock, NamespaceString nss);
    void setNS(WithLock, const DatabaseName& dbName);

    StringData getPlanSummary() const {
        return _planSummary;
    }

    void setPlanSummary(WithLock, StringData summary) {
        _planSummary = std::string{summary};
    }

    void setPlanSummary(WithLock, std::string summary) {
        _planSummary = std::move(summary);
    }

    void setGenericCursor(WithLock, GenericCursor gc);

    boost::optional<SingleThreadedLockStats> getLockStatsBase() const {
        if (!_resourceStatsBase) {
            return boost::none;
        }
        return _resourceStatsBase->lockStats;
    }

    void setTickSource_forTest(TickSource* tickSource) {
        _tickSource = tickSource;
    }

    void setShouldOmitDiagnosticInformation(WithLock, bool shouldOmitDiagnosticInfo) {
        _shouldOmitDiagnosticInformation = shouldOmitDiagnosticInfo;
    }
    bool getShouldOmitDiagnosticInformation() const {
        return _shouldOmitDiagnosticInformation;
    }

    /**
     * Walks the whole CurOp stack starting at the given object, returning true of any CurOp should
     * omit diagnostic info.
     */
    static bool shouldCurOpStackOmitDiagnosticInformation(CurOp*);

    /**
     * Returns storage metrics for the current operation by accounting for metrics accrued outside
     * of this operation.
     */
    SingleThreadedStorageMetrics getOperationStorageMetrics() const;

    long long getPrepareReadConflicts() const;

    void updateSpillStorageStats(std::unique_ptr<StorageStats> operationStorageStats);

private:
    class CurOpStack;

    /**
     * A set of additive resource stats that CurOp tracks during it's lifecycle.
     */
    struct AdditiveResourceStats {
        /**
         * Add stats that have accrued before unstashing the Locker and Recovery Unit for a
         * transaction. Does not add timeQueuedForTickets, which is handled separately.
         */
        void addForUnstash(const AdditiveResourceStats& other);

        /**
         * Subtract stats that have accrued on this transaction's Locker and Recovery Unit since
         * unstashing. Does not subtract timeQueuedForTickets, which is handled separately.
         */
        void subtractForStash(const AdditiveResourceStats& other);

        /**
         * Snapshot of locker lock stats.
         */
        SingleThreadedLockStats lockStats;

        /**
         * Total time spent waiting on locks.
         */
        Microseconds cumulativeLockWaitTime{0};

        /**
         * Total time spent queued for tickets.
         */
        Microseconds timeQueuedForTickets{0};

        /**
         * Total time spent queued for flow control tickets.
         */
        Microseconds timeQueuedForFlowControl{0};
    };

    /**
     * Gets the OperationContext associated with this CurOp.
     * This must only be called after the CurOp has been pushed to an OperationContext's CurOpStack.
     */
    OperationContext* opCtx();
    OperationContext* opCtx() const;

    TickSource::Tick startTime();
    Microseconds computeElapsedTimeTotal(TickSource::Tick startTime,
                                         TickSource::Tick endTime) const;

    /**
     * Collects and returns additive resource stats
     */
    AdditiveResourceStats getAdditiveResourceStats(
        const boost::optional<ExecutionAdmissionContext>& admCtx);

    void _initializeResourceStatsBaseIfNecessary() {
        if (!_resourceStatsBase) {
            _resourceStatsBase.emplace();
        }
    }

    /**
     * Returns the time operation spends blocked waiting for locks and tickets.
     */
    Milliseconds _sumBlockedTimeTotal();

    /**
     * Handles failpoints that check whether a command has completed or not.
     * Used for testing purposes instead of the getLog command.
     */
    void _checkForFailpointsAfterCommandLogged();

    /**
     * Fetches storage stats and stores them in the OpDebug if they're not already present.
     * Can throw if interrupted while waiting for the global lock.
     */
    void _fetchStorageStatsIfNecessary(Date_t deadline);

    static const OperationContext::Decoration<CurOpStack> _curopStack;

    // The stack containing this CurOp instance.
    // This is set when this instance is pushed to the stack.
    CurOpStack* _stack{nullptr};

    // The CurOp beneath this CurOp instance in its stack, if any.
    // This is set when this instance is pushed to a non-empty stack.
    CurOp* _parent{nullptr};

    const Command* _command{nullptr};

    // The time at which this CurOp instance was marked as started.
    std::atomic<TickSource::Tick> _start{0};  // NOLINT

    // The time at which this CurOp instance was marked as done or 0 if the CurOp is not yet done.
    std::atomic<TickSource::Tick> _end{0};  // NOLINT

    // This CPU timer tracks the CPU time spent for this operation. Will be nullptr on unsupported
    // platforms.
    boost::optional<OperationCPUTimer> _cpuTimer;

    // The time at which this CurOp instance had its timer paused, or 0 if the timer is not
    // currently paused.
    TickSource::Tick _lastPauseTime{0};

    // The cumulative duration for which the timer has been paused.
    Microseconds _totalPausedDuration{0};

    // The elapsedTimeTotal() value at which the remoteOpWait timer was started, or empty if the
    // remoteOpWait timer is not currently running.
    boost::optional<Microseconds> _remoteOpStartTime;

    // _networkOp represents the network-level op code: OP_QUERY, OP_GET_MORE, OP_MSG, etc.
    NetworkOp _networkOp{opInvalid};  // only set this through setNetworkOp() to keep synced
    // _logicalOp is the logical operation type, ie 'dbQuery' regardless of whether this is an
    // OP_QUERY find, a find command using OP_QUERY, or a find command using OP_MSG.
    // Similarly, the return value will be dbGetMore for both OP_GET_MORE and getMore command.
    LogicalOp _logicalOp{LogicalOp::opInvalid};  // only set this through setNetworkOp()

    bool _isCommand{false};
    int _dbprofile{0};  // 0=off, 1=slow, 2=all
    NamespaceString _nss;
    BSONObj _opDescription;
    BSONObj _originatingCommand;  // Used by getMore to display original command.
    OpDebug _debug;
    std::string _failPointMessage;  // Used to store FailPoint information.
    std::string _message;
    boost::optional<ProgressMeter> _progressMeter;
    AtomicWord<int> _numYields{0};
    // A GenericCursor containing information about the active cursor for a getMore operation.
    boost::optional<GenericCursor> _genericCursor;

    std::string _planSummary;

    // Tracks resource statistics from the locker and admission context that accrued outside the
    // current operation.
    //
    // This variable is used to compute the lock stats and storage metrics accrued specifically by
    // this operation by subtracting its value from the final resource stats. For
    // instance, if this variable holds a value of 5, and the total for that metric at the end of
    // the operation is 9, then 4 (9 - 5) was accrued during the operation.
    //
    // Note that this variable accurately reflects metrics accrued outside the operation only when
    // this CurOp is on top of the CurOpStack. When CurOp is stashed, this variable temporarily
    // stores the value accrued by this operation as a negative number.
    //
    // Example:
    // If the metric value is 5 with CurOp on the stack top, and after stashing at metric value 9,
    // the accrued value of 4 (9 - 5) is stored as -4. Upon unstashing and seeing a metric value of
    // 11, we calculate that 7 (11 - (-4)) was accrued outside of this operation.
    boost::optional<AdditiveResourceStats> _resourceStatsBase;

    SharedUserAcquisitionStats _userAcquisitionStats{std::make_shared<UserAcquisitionStats>()};

    TickSource* _tickSource = globalSystemTickSource();
    // These values are used to calculate the amount of time spent planning a query.
    std::atomic<TickSource::Tick> _queryPlanningStart{0};  // NOLINT
    std::atomic<TickSource::Tick> _queryPlanningEnd{0};    // NOLINT

    // These values are used to calculate the amount of time spent waiting for write concern.
    std::atomic<TickSource::Tick> _waitForWriteConcernStart{0};  // NOLINT
    std::atomic<TickSource::Tick> _waitForWriteConcernEnd{0};    // NOLINT
    // This metric is the same value as debug().waitForWriteConcernDurationMillis.
    // We cannot use std::atomic in OpDebug since it is not copy assignable, but using a non-atomic
    // allows for a data race between stopWaitForWriteConcernTimer and curop::reportState.
    std::atomic<Milliseconds> _atomicWaitForWriteConcernDurationMillis{Milliseconds{0}};  // NOLINT

    // Flag to decide if diagnostic information should be omitted.
    bool _shouldOmitDiagnosticInformation{false};

    // TODO SERVER-90937: Remove need to zero out blocked time prior to operation starting.
    Milliseconds _blockedTimeAtStart{0};

    // These memory tracking metrics need to be in CurOp instead of OpDebug because they are
    // reported in $currentOp, and $currentOp only looks at CurOp. These memory tracking metrics are
    // atomics because a non-atomic allows for a data race between acquiring the memory statistics
    // from the operation context and curop::reportState.
    // These metrics refer to local memory use, i.e. on a mongos process, as opposed to rolling up
    // memory from shards.
    AtomicWord<int64_t> _inUseTrackedMemoryBytes{0};
    AtomicWord<int64_t> _peakTrackedMemoryBytes{0};
};
}  // namespace mongo
