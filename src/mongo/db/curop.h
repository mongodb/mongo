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
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/thread_safe_string.h"
#include "mongo/util/time_support.h"
#include "mongo/util/net/message.h"

namespace mongo {

class Client;
class Command;
class CurOp;
class OperationContext;

/**
 * stores a copy of a bson obj in a fixed size buffer
 * if its too big for the buffer, says "too big"
 * useful for keeping a copy around indefinitely without wasting a lot of space or doing malloc
 */
class CachedBSONObjBase {
public:
    static BSONObj _tooBig;  // { $msg : "query not recording (too large)" }
};

template <size_t BUFFER_SIZE>
class CachedBSONObj : public CachedBSONObjBase {
public:
    enum { TOO_BIG_SENTINEL = 1 };

    CachedBSONObj() {
        _size = (int*)_buf;
        reset();
    }

    void reset(int sz = 0) {
        _lock.lock();
        _reset(sz);
        _lock.unlock();
    }

    void set(const BSONObj& o) {
        scoped_spinlock lk(_lock);
        size_t sz = o.objsize();
        if (sz > sizeof(_buf)) {
            _reset(TOO_BIG_SENTINEL);
        } else {
            memcpy(_buf, o.objdata(), sz);
        }
    }

    int size() const {
        return *_size;
    }
    bool have() const {
        return size() > 0;
    }
    bool tooBig() const {
        return size() == TOO_BIG_SENTINEL;
    }

    BSONObj get() const {
        scoped_spinlock lk(_lock);
        return _get();
    }

    void append(BSONObjBuilder& b, StringData name) const {
        scoped_spinlock lk(_lock);
        BSONObj temp = _get();
        b.append(name, temp);
    }

private:
    /** you have to be locked when you call this */
    BSONObj _get() const {
        int sz = size();
        if (sz == 0)
            return BSONObj();
        if (sz == TOO_BIG_SENTINEL)
            return _tooBig;
        return BSONObj(_buf).copy();
    }

    /** you have to be locked when you call this */
    void _reset(int sz) {
        _size[0] = sz;
    }

    mutable SpinLock _lock;
    int* _size;
    char _buf[BUFFER_SIZE];
};

/* lifespan is different than CurOp because of recursives with DBDirectClient */
class OpDebug {
public:
    OpDebug() : planSummary(2048) {
        reset();
    }

    void reset();

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

    // -------------------

    // basic options
    // _networkOp represents the network-level op code: OP_QUERY, OP_GET_MORE, OP_COMMAND, etc.
    Operation networkOp;  // only set this through setNetworkOp_inlock() to keep synced
    // _logicalOp is the logical operation type, ie 'dbQuery' regardless of whether this is an
    // OP_QUERY find, a find command using OP_QUERY, or a find command using OP_COMMAND.
    // Similarly, the return value will be dbGetMore for both OP_GET_MORE and getMore command.
    Operation logicalOp;  // only set this through setNetworkOp_inlock() to keep synced
    bool iscommand;
    BSONObj query;
    BSONObj updateobj;

    // detailed options
    long long cursorid;
    long long ntoreturn;
    long long ntoskip;
    bool exhaust;

    // debugging/profile info
    long long keysExamined;
    long long docsExamined;
    bool idhack;  // indicates short circuited code path on an update to make the update faster
    bool hasSortStage;    // true if the query plan involves an in-memory sort
    long long nMatched;   // number of records that match the query
    long long nModified;  // number of records written (no no-ops)
    long long nmoved;     // updates resulted in a move (moves are expensive)
    long long ninserted;
    long long ndeleted;
    bool fastmod;
    bool fastmodinsert;    // upsert of an $operation. builds a default object
    bool upsert;           // true if the update actually did an insert
    bool cursorExhausted;  // true if the cursor has been closed at end a find/getMore operation
    int keyUpdates;
    long long writeConflicts;
    ThreadSafeString planSummary;  // a brief std::string describing the query solution

    // New Query Framework debugging/profiling info
    // TODO: should this really be an opaque BSONObj?  Not sure.
    CachedBSONObj<4096> execStats;

    // error handling
    ExceptionInfo exceptionInfo;

    // response info
    int executionTime;
    long long nreturned;
    int responseLength;

private:
    /**
     * Returns true if this OpDebug instance was generated by a find command. Returns false for
     * OP_QUERY find and all other operations.
     */
    bool isFindCommand() const;

    /**
     * Returns true if this OpDebug instance was generated by a find command. Returns false for
     * OP_GET_MORE and all other operations.
     */
    bool isGetMoreCommand() const;
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
        return _query.have();
    }
    BSONObj query() const {
        return _query.get();
    }
    void appendQuery(BSONObjBuilder& b, StringData name) const {
        _query.append(b, name);
    }

    void enter_inlock(const char* ns, int dbProfileLevel);

    /**
     * Sets the type of the current network operation.
     */
    void setNetworkOp_inlock(Operation op) {
        _networkOp = op;
        _debug.networkOp = op;
    }

    /**
     * Sets the type of the current logical operation.
     */
    void setLogicalOp_inlock(Operation op) {
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
    Operation getNetworkOp() const {
        return _networkOp;
    }

    /**
     * Gets the logical operation type. No lock is required if called by the thread executing
     * the operation, but the lock must be held if called from another thread.
     */
    Operation getLogicalOp() const {
        return _logicalOp;
    }

    /**
     * Returns true if the current operation is known to be a command.
     */
    bool isCommand() const {
        return _isCommand;
    }

    //
    // Methods for controlling CurOp "max time".
    //

    /**
     * Sets the amount of time operation this should be allowed to run, units of microseconds.
     * The special value 0 is "allow to run indefinitely".
     */
    void setMaxTimeMicros(uint64_t maxTimeMicros);

    /**
     * Returns true if a time limit has been set on this operation, and false otherwise.
     */
    bool isMaxTimeSet() const;

    /**
     * Checks whether this operation has been running longer than its time limit.  Returns
     * false if not, or if the operation has no time limit.
     */
    bool maxTimeHasExpired();

    /**
     * Returns the number of microseconds remaining for this operation's time limit, or the
     * special value 0 if the operation has no time limit.
     *
     * Calling this method is more expensive than calling its sibling "maxTimeHasExpired()",
     * since an accurate measure of remaining time needs to be calculated.
     */
    uint64_t getRemainingMaxTimeMicros() const;

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

    void setQuery_inlock(const BSONObj& query) {
        _query.set(query);
    }

    Command* getCommand() const {
        return _command;
    }
    void setCommand_inlock(Command* command) {
        _command = command;
    }

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

private:
    class CurOpStack;

    static const OperationContext::Decoration<CurOpStack> _curopStack;

    CurOp(OperationContext*, CurOpStack*);

    CurOpStack* _stack;
    CurOp* _parent = nullptr;
    Command* _command;
    long long _start;
    long long _end;

    // _networkOp represents the network-level op code: OP_QUERY, OP_GET_MORE, OP_COMMAND, etc.
    Operation _networkOp;  // only set this through setNetworkOp_inlock() to keep synced
    // _logicalOp is the logical operation type, ie 'dbQuery' regardless of whether this is an
    // OP_QUERY find, a find command using OP_QUERY, or a find command using OP_COMMAND.
    // Similarly, the return value will be dbGetMore for both OP_GET_MORE and getMore command.
    Operation _logicalOp;  // only set this through setNetworkOp_inlock() to keep synced

    bool _isCommand;
    int _dbprofile;  // 0=off, 1=slow, 2=all
    std::string _ns;
    CachedBSONObj<512> _query;  // CachedBSONObj is thread safe
    OpDebug _debug;
    std::string _message;
    ProgressMeter _progressMeter;
    int _numYields;

    // this is how much "extra" time a query might take
    // a writebacklisten for example will block for 30s
    // so this should be 30000 in that case
    long long _expectedLatencyMs;

    // Time limit for this operation.  0 if the operation has no time limit.
    uint64_t _maxTimeMicros;

    /** Nested class that implements tracking of a time limit for a CurOp object. */
    class MaxTimeTracker {
        MONGO_DISALLOW_COPYING(MaxTimeTracker);

    public:
        /** Newly-constructed MaxTimeTracker objects have the time limit disabled. */
        MaxTimeTracker();

        /** Disables the time tracker. */
        void reset();

        /** Returns whether or not time tracking is enabled. */
        bool isEnabled() const {
            return _enabled;
        }

        /**
         * Enables time tracking.  The time limit is set to be "durationMicros" microseconds
         * from "startEpochMicros" (units of microseconds since the epoch).
         *
         * "durationMicros" must be nonzero.
         */
        void setTimeLimit(uint64_t startEpochMicros, uint64_t durationMicros);

        /**
         * Checks whether the time limit has been hit.  Returns false if not, or if time
         * tracking is disabled.
         */
        bool checkTimeLimit();

        /**
         * Returns the number of microseconds remaining for the time limit, or the special
         * value 0 if time tracking is disabled.
         *
         * Calling this method is more expensive than calling its sibling "checkInterval()",
         * since an accurate measure of remaining time needs to be calculated.
         */
        uint64_t getRemainingMicros() const;

    private:
        // Whether or not time tracking is enabled for this operation.
        bool _enabled;

        // Point in time at which the time limit is hit.  Units of microseconds since the
        // epoch.
        uint64_t _targetEpochMicros;

        // Approximate point in time at which the time limit is hit.   Units of milliseconds
        // since the server process was started.
        int64_t _approxTargetServerMillis;
    } _maxTimeTracker;
};
}
