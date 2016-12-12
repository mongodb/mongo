/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/cursor_id.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/record_id.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/message.h"

namespace mongo {

class Collection;
class CursorManager;
class RecoveryUnit;

/**
 * Parameters used for constructing a ClientCursor. ClientCursors cannot be constructed in
 * isolation, but rather must be constructed and managed using a CursorManager. See cursor_manager.h
 * for more details.
 */
struct ClientCursorParams {
    ClientCursorParams(PlanExecutor* exec,
                       std::string ns,
                       bool isReadCommitted,
                       int qopts = 0,
                       const BSONObj query = BSONObj(),
                       bool isAggCursor = false)
        : exec(exec),
          ns(std::move(ns)),
          isReadCommitted(isReadCommitted),
          qopts(qopts),
          query(query),
          isAggCursor(isAggCursor) {}

    PlanExecutor* exec = nullptr;
    const std::string ns;
    bool isReadCommitted = false;
    int qopts = 0;
    const BSONObj query = BSONObj();
    bool isAggCursor = false;
};

/**
 * A ClientCursor is the server-side state associated with a particular cursor id. A cursor id is a
 * handle that we return to the client for queries which require results to be returned in multiple
 * batches. The client can manage the server-side cursor state by passing the cursor id back to the
 * server for certain supported operations.
 *
 * For instance, a client can retrieve the next batch of results from the cursor by issuing a
 * getMore on this cursor id. It can also request that server-side resources be freed by issuing a
 * killCursors on a particular cursor id. This is useful if the client wishes to abandon the cursor
 * without retrieving all results.
 *
 * ClientCursors cannot exist in isolation and must be created, accessed, and destroyed via a
 * CursorManager. See cursor_manager.h for more details. Unless the ClientCursor is marked by the
 * caller as "no timeout", it will be automatically destroyed by its cursor manager after a period
 * of inactivity.
 */
class ClientCursor {
    MONGO_DISALLOW_COPYING(ClientCursor);

public:
    CursorId cursorid() const {
        return _cursorid;
    }

    std::string ns() const {
        return _ns;
    }

    bool isReadCommitted() const {
        return _isReadCommitted;
    }

    bool isAggCursor() const {
        return _isAggCursor;
    }

    PlanExecutor* getExecutor() const {
        return _exec.get();
    }

    int queryOptions() const {
        return _queryOptions;
    }

    const BSONObj& getQuery() const {
        return _query;
    }

    /**
     * Returns the total number of query results returned by the cursor so far.
     */
    long long pos() const {
        return _pos;
    }

    /**
     * Increments the cursor's tracked number of query results returned so far by 'n'.
     */
    void incPos(long long n) {
        _pos += n;
    }

    /**
     * Sets the cursor's tracked number of query results returned so far to 'n'.
     */
    void setPos(long long n) {
        _pos = n;
    }

    //
    // Timing and timeouts.
    //

    /**
     * Increments the amount of time for which this cursor believes it has been idle by 'millis'.
     *
     * After factoring in this additional idle time, returns whether or not this cursor now exceeds
     * its idleness timeout and should be deleted.
     */
    bool shouldTimeout(int millis);

    /**
     * Resets this cursor's idle time to zero. Only cursors whose idle time is sufficiently high
     * will be deleted by the cursor manager's reaper thread.
     */
    void resetIdleTime();

    /**
     * Returns the number of milliseconds for which this cursor believes it has been idle.
     */
    int idleTime() const {
        return _idleAgeMillis;
    }

    /**
     * Returns the amount of time execution time available to this cursor. Only valid at the
     * beginning of a getMore request, and only really for use by the maxTime tracking code.
     *
     * Microseconds::max() == infinity, values less than 1 mean no time left.
     */
    Microseconds getLeftoverMaxTimeMicros() const {
        return _leftoverMaxTimeMicros;
    }

    /**
     * Sets the amount of execution time available to this cursor. This is only called when an
     * operation that uses a cursor is finishing, to update its remaining time.
     *
     * Microseconds::max() == infinity, values less than 1 mean no time left.
     */
    void setLeftoverMaxTimeMicros(Microseconds leftoverMaxTimeMicros) {
        _leftoverMaxTimeMicros = leftoverMaxTimeMicros;
    }

    //
    // Replication-related methods.
    //

    // Used to report replication position only in master-slave, so we keep them as TimeStamp rather
    // than OpTime.
    void updateSlaveLocation(OperationContext* txn);

    void slaveReadTill(const Timestamp& t) {
        _slaveReadTill = t;
    }

    /** Just for testing. */
    Timestamp getSlaveReadTill() const {
        return _slaveReadTill;
    }

    /**
     * Returns the server-wide the count of living cursors. Such a cursor is called an "open
     * cursor".
     */
    static long long totalOpen();

private:
    friend class CursorManager;
    friend class ClientCursorPin;

    /**
     * Since the client cursor destructor is private, this is needed for using client cursors with
     * smart pointers.
     */
    struct Deleter {
        void operator()(ClientCursor* cursor) {
            delete cursor;
        }
    };

    /**
     * Constructs a ClientCursor. Since cursors must come into being registered and pinned, this is
     * private. See cursor_manager.h for more details.
     */
    ClientCursor(const ClientCursorParams& params, CursorManager* cursorManager, CursorId cursorId);

    /**
     * Constructs a special ClientCursor used to track sharding state for the given collection.
     */
    ClientCursor(const Collection* collection, CursorManager* cursorManager, CursorId cursorId);

    /**
     * Destroys a ClientCursor. This is private, since only the CursorManager or the ClientCursorPin
     * is allowed to destroy a cursor.
     *
     * Cursors must be unpinned and deregistered from the CursorManager before they can be
     * destroyed.
     */
    ~ClientCursor();

    void init();

    /**
     * Marks the cursor, and its underlying query plan, as killed.
     */
    void kill();

    // The ID of the ClientCursor. A value of 0 is used to mean that no cursor id has been assigned.
    CursorId _cursorid = 0;

    // The namespace we're operating on.
    std::string _ns;

    const bool _isReadCommitted = false;

    // A pointer to the CursorManager which owns this cursor. This must be filled out when the
    // cursor is constructed via the CursorManager.
    //
    // If '_cursorManager' is destroyed while this cursor is pinned, then ownership of the cursor is
    // transferred to the ClientCursorPin. In this case, '_cursorManager' set back to null in order
    // to indicate the ownership transfer.
    CursorManager* _cursorManager = nullptr;

    // Tracks the number of results returned by this cursor so far.
    long long _pos = 0;

    // If this cursor was created by a find operation, '_query' holds the query predicate for
    // the find. If this cursor was created by a command (e.g. the aggregate command), then
    // '_query' holds the command specification received from the client.
    BSONObj _query;

    // See the QueryOptions enum in dbclientinterface.h.
    int _queryOptions = 0;

    // Is this ClientCursor backed by an aggregation pipeline?
    //
    // Agg executors differ from others in that they manage their own locking internally and
    // should not be killed or destroyed when the underlying collection is deleted.
    //
    // Note: This should *not* be set for the internal cursor used as input to an aggregation.
    const bool _isAggCursor = false;

    // While a cursor is being used by a client, it is marked as "pinned". See ClientCursorPin
    // below.
    //
    // Cursors always come into existence in a pinned state.
    bool _isPinned = true;

    // Is the "no timeout" flag set on this cursor?  If false, this cursor may be automatically
    // deleted after an interval of inactivity.
    bool _isNoTimeout = false;

    // The replication position only used in master-slave.
    Timestamp _slaveReadTill;

    // How long has the cursor been idle?
    int _idleAgeMillis = 0;

    // Unused maxTime budget for this cursor.
    Microseconds _leftoverMaxTimeMicros = Microseconds::max();

    // The underlying query execution machinery.
    std::unique_ptr<PlanExecutor> _exec;
};

/**
 * ClientCursorPin is an RAII class which must be used in order to access a cursor. On construction,
 * the ClientCursorPin marks its cursor as in use, which is called "pinning" the cursor. On
 * destructrution, the ClientCursorPin marks its cursor as no longer in use, which is called
 * "unpinning" the cursor. Pinning is used to prevent multiple concurrent uses of the same cursor---
 * pinned cursors cannot be killed or timed out and cannot be used concurrently by other operations
 * such as getMore or killCursors. A pin is obtained using the CursorManager. See cursor_manager.h
 * for more details.
 *
 * A pin extends the lifetime of a ClientCursor object until the pin's release.  Pinned
 * ClientCursor objects cannot not be killed due to inactivity, and cannot be killed by user
 * kill requests.  When a CursorManager is destroyed (e.g. by a collection drop), ownership of
 * any still-pinned ClientCursor objects is transferred to their managing ClientCursorPin
 * objects.
 *
 * Example usage:
 * {
 *     StatusWith<ClientCursorPin> pin = cursorManager->pinCursor(cursorid);
 *     if (!pin.isOK()) {
 *         // No cursor with id 'cursorid' exists. Handle the error here. Pin automatically released
 *         // on block exit.
 *         return pin.getStatus();
 *     }
 *
 *     ClientCursor* cursor = pin.getValue().getCursor();
 *     // Use cursor. Pin automatically released on block exit.
 * }
 *
 * Clients that wish to access ClientCursor objects owned by collection cursor managers must hold
 * the collection lock while calling any pin method, including pin acquisition by the RAII
 * constructor and pin release by the RAII destructor.  This guards from a collection drop (which
 * requires an exclusive lock on the collection) occurring concurrently with the pin request or
 * unpin request.
 *
 * Clients that wish to access ClientCursor objects owned by the global cursor manager need not
 * hold any locks; the global cursor manager can only be destroyed by a process exit.
 */
class ClientCursorPin {
    MONGO_DISALLOW_COPYING(ClientCursorPin);

public:
    /**
     * Moves 'other' into 'this'. The 'other' pin must have a pinned cursor. Moving an empty pin
     * into 'this' is illegal.
     */
    ClientCursorPin(ClientCursorPin&& other);

    /**
     * Moves 'other' into 'this'. 'other' must have a pinned cursor and 'this' must have no pinned
     * cursor.
     */
    ClientCursorPin& operator=(ClientCursorPin&& other);

    /**
     * Calls release().
     */
    ~ClientCursorPin();

    /**
     * Releases the pin.  It does not delete the underlying cursor unless ownership has passed
     * to us after kill.  Turns into a no-op if release() or deleteUnderlying() have already
     * been called on this pin.
     */
    void release();

    /**
     * Deletes the underlying cursor.  Cannot be called if release() or deleteUnderlying() have
     * already been called on this pin.
     */
    void deleteUnderlying();

    /**
     * Returns a pointer to the pinned cursor.
     */
    ClientCursor* getCursor() const;

private:
    friend class CursorManager;

    ClientCursorPin(ClientCursor* cursor);

    ClientCursor* _cursor = nullptr;
};

void startClientCursorMonitor();

}  // namespace mongo
