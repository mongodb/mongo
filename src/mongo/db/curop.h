// @file curop.h

/*
 *    Copyright (C) 2010 10gen Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/net/message.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Client;
class CurOp;
class OperationContext;
struct PlanSummaryStats;

/* lifespan is different than CurOp because of recursives with DBDirectClient */
class OpDebug {
public:
    OpDebug() = default;

    std::string report(const CurOp& curop, const SingleThreadedLockStats& lockStats) const;

    /**
     * Appends information about the current operation to "builder"
     *
     * @param curop reference to the CurOp that owns this OpDebug
     * @param lockStats lockStats object containing locking information about the operation
     */
    void append(const CurOp& curop,
                const SingleThreadedLockStats& lockStats,
                BSONObjBuilder& builder) const;

    /**
     * Copies relevant plan summary metrics to this OpDebug instance.
     */
    void setPlanSummaryMetrics(const PlanSummaryStats& planSummaryStats);

    // -------------------

    // basic options
    // _networkOp represents the network-level op code: OP_QUERY, OP_GET_MORE, OP_COMMAND, etc.
    NetworkOp networkOp{opInvalid};  // only set this through setNetworkOp_inlock() to keep synced
    // _logicalOp is the logical operation type, ie 'dbQuery' regardless of whether this is an
    // OP_QUERY find, a find command using OP_QUERY, or a find command using OP_COMMAND.
    // Similarly, the return value will be dbGetMore for both OP_GET_MORE and getMore command.
    LogicalOp logicalOp{LogicalOp::opInvalid};  // only set this through setNetworkOp_inlock()
    bool iscommand{false};
    BSONObj updateobj{};

    // detailed options
    long long cursorid{-1};
    long long ntoreturn{-1};
    long long ntoskip{-1};
    bool exhaust{false};

    // debugging/profile info
    long long keysExamined{-1};
    long long docsExamined{-1};

    bool hasSortStage{false};  // true if the query plan involves an in-memory sort

    // True if the plan came from the multi-planner (not from the plan cache and not a query with a
    // single solution).
    bool fromMultiPlanner{false};

    // True if a replan was triggered during the execution of this operation.
    bool replanned{false};

    long long nMatched{-1};   // number of records that match the query
    long long nModified{-1};  // number of records written (no no-ops)
    long long ninserted{-1};
    long long ndeleted{-1};
    bool fastmodinsert{false};  // upsert of an $operation. builds a default object
    bool upsert{false};         // true if the update actually did an insert
    bool cursorExhausted{
        false};  // true if the cursor has been closed at end a find/getMore operation

    // The following metrics are initialized with 0 rather than -1 in order to simplify use by the
    // CRUD path.
    long long nmoved{0};        // updates resulted in a move (moves are expensive)
    long long keysInserted{0};  // Number of index keys inserted.
    long long keysDeleted{0};   // Number of index keys removed.
    long long writeConflicts{0};

    BSONObj execStats;  // Owned here.

    // error handling
    ExceptionInfo exceptionInfo;

    // response info
    int executionTime{0};
    long long nreturned{-1};
    int responseLength{-1};
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
    MONGO_DISALLOW_COPYING(CurOp);

public:
    static CurOp* get(const OperationContext* opCtx);
    static CurOp* get(const OperationContext& opCtx);

    /**
     * Constructs a nested CurOp at the top of the given "opCtx"'s CurOp stack.
     */
    explicit CurOp(OperationContext* opCtx);
    ~CurOp();

    bool haveQuery() const {
        return !_query.isEmpty();
    }

    /**
     * The BSONObj returned may not be owned by CurOp. Callers should call getOwned() if they plan
     * to reference beyond the lifetime of this CurOp instance.
     */
    BSONObj query() const {
        return _query;
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

    bool shouldDBProfile(int ms) const {
        if (_dbprofile <= 0)
            return false;

        return _dbprofile >= 2 || ms >= serverGlobalParams.slowMS;
    }

    /**
     * Raises the profiling level for this operation to "dbProfileLevel" if it was previously
     * less than "dbProfileLevel".
     *
     * This belongs on OpDebug, and so does not have the _inlock suffix.
     */
    void raiseDbProfileLevel(int dbProfileLevel);

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

    void ensureStarted();
    bool isStarted() const {
        return _start > 0;
    }
    long long startTime() {  // micros
        ensureStarted();
        return _start;
    }
    void done() {
        _end = curTimeMicros64();
    }

    long long totalTimeMicros() {
        massert(12601, "CurOp not marked done yet", _end);
        return _end - startTime();
    }
    int totalTimeMillis() {
        return (int)(totalTimeMicros() / 1000);
    }
    long long elapsedMicros() {
        return curTimeMicros64() - startTime();
    }
    int elapsedMillis() {
        return (int)(elapsedMicros() / 1000);
    }
    int elapsedSeconds() {
        return elapsedMillis() / 1000;
    }

    /**
     * 'query' must be either an owned BSONObj or guaranteed to outlive the OperationContext it is
     * associated with.
     */
    void setQuery_inlock(const BSONObj& query) {
        _query = query;
    }

    /**
     * Sets the original command object. Used only by the getMore command.
     */
    void setOriginatingCommand_inlock(const BSONObj& commandObj) {
        _originatingCommand = commandObj.getOwned();
    }

    Command* getCommand() const {
        return _command;
    }
    void setCommand_inlock(Command* command) {
        _command = command;
    }

    /**
     * Returns whether the current operation is a read, write, or command.
     */
    Command::ReadWriteType getReadWriteType() const;

    /**
     * Appends information about this CurOp to "builder".
     *
     * If called from a thread other than the one executing the operation associated with this
     * CurOp, it is necessary to lock the associated Client object before executing this method.
     */
    void reportState(BSONObjBuilder* builder);

    /**
     * Sets the message and the progress meter for this CurOp.
     *
     * While it is necessary to hold the lock while this method executes, the
     * "hit" and "finished" methods of ProgressMeter may be called safely from
     * the thread executing the operation without locking the Client.
     */
    ProgressMeter& setMessage_inlock(const char* msg,
                                     std::string name = "Progress",
                                     unsigned long long progressMeterTotal = 0,
                                     int secondsBetween = 3);

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
    void yielded() {
        _numYields++;
    }  // Should be _inlock()?

    /**
     * Returns the number of times yielded() was called.  Callers on threads other
     * than the one executing the operation must lock the client.
     */
    int numYields() const {
        return _numYields;
    }

    /**
     * Access to _expectedLatencyMs is not synchronized, so it is illegal for threads other than the
     * one executing the operation to call getExpectedLatencyMs() and setExpectedLatencyMs().
     */
    long long getExpectedLatencyMs() const {
        return _expectedLatencyMs;
    }
    void setExpectedLatencyMs(long long latency) {
        _expectedLatencyMs = latency;
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

private:
    class CurOpStack;

    static const OperationContext::Decoration<CurOpStack> _curopStack;

    CurOp(OperationContext*, CurOpStack*);

    CurOpStack* _stack;
    CurOp* _parent{nullptr};
    Command* _command{nullptr};
    long long _start{0};
    long long _end{0};

    // _networkOp represents the network-level op code: OP_QUERY, OP_GET_MORE, OP_COMMAND, etc.
    NetworkOp _networkOp{opInvalid};  // only set this through setNetworkOp_inlock() to keep synced
    // _logicalOp is the logical operation type, ie 'dbQuery' regardless of whether this is an
    // OP_QUERY find, a find command using OP_QUERY, or a find command using OP_COMMAND.
    // Similarly, the return value will be dbGetMore for both OP_GET_MORE and getMore command.
    LogicalOp _logicalOp{LogicalOp::opInvalid};  // only set this through setNetworkOp_inlock()

    bool _isCommand{false};
    int _dbprofile{0};  // 0=off, 1=slow, 2=all
    std::string _ns;
    BSONObj _query;
    BSONObj _originatingCommand;  // Used by getMore to display original command.
    OpDebug _debug;
    std::string _message;
    ProgressMeter _progressMeter;
    int _numYields{0};

    // this is how much "extra" time a query might take
    // a writebacklisten for example will block for 30s
    // so this should be 30000 in that case
    long long _expectedLatencyMs{0};

    std::string _planSummary;
};
}  // namespace mongo
