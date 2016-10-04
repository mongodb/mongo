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
#include "mongo/util/net/message.h"

namespace mongo {

class Collection;
class CursorManager;
class RecoveryUnit;

/**
 * ClientCursor is a wrapper that represents a cursorid from our database application's
 * perspective.
 */
class ClientCursor {
    MONGO_DISALLOW_COPYING(ClientCursor);

public:
    /**
     * This ClientCursor constructor creates a cursorid that can be used with getMore and
     * killCursors.  "cursorManager" is the object that will manage the lifetime of this
     * cursor, and "ns" is the namespace string that should be associated with this cursor (e.g.
     * "test.foo", "test.$cmd.listCollections", etc).
     */
    ClientCursor(CursorManager* cursorManager,
                 PlanExecutor* exec,
                 const std::string& ns,
                 bool isReadCommitted,
                 int qopts = 0,
                 const BSONObj query = BSONObj(),
                 bool isAggCursor = false);

    /**
     * This ClientCursor is used to track sharding state for the given collection.
     *
     * Do not use outside of RangePreserver!
     */
    explicit ClientCursor(const Collection* collection);

    //
    // Basic accessors
    //

    CursorId cursorid() const {
        return _cursorid;
    }
    std::string ns() const {
        return _ns;
    }
    CursorManager* cursorManager() const {
        return _cursorManager;
    }
    bool isReadCommitted() const {
        return _isReadCommitted;
    }
    bool isAggCursor() const {
        return _isAggCursor;
    }

    //
    // Pinning functionality.
    //

    /**
     * Marks this ClientCursor as in use.  unsetPinned() must be called before the destructor of
     * this ClientCursor is invoked.
     */
    void setPinned() {
        _isPinned = true;
    }

    /**
     * Marks this ClientCursor as no longer in use.
     */
    void unsetPinned() {
        _isPinned = false;
    }

    bool isPinned() const {
        return _isPinned;
    }

    /**
     * This is called when someone is dropping a collection or something else that
     * goes through killing cursors.
     * It removes the responsiilibty of de-registering from ClientCursor.
     * Responsibility for deleting the ClientCursor doesn't change from this call
     * see PlanExecutor::kill.
     */
    void kill();

    //
    // Timing and timeouts
    //

    /**
     * @param millis amount of idle passed time since last call
     * note called outside of locks (other than ccmutex) so care must be exercised
     */
    bool shouldTimeout(int millis);
    void setIdleTime(int millis);
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
    // Replication-related stuff.  TODO: Document and clean.
    //

    // Used to report replication position only in master-slave,
    // so we keep them as TimeStamp rather than OpTime.
    void updateSlaveLocation(OperationContext* txn);
    void slaveReadTill(const Timestamp& t) {
        _slaveReadTill = t;
    }
    /** Just for testing. */
    Timestamp getSlaveReadTill() const {
        return _slaveReadTill;
    }

    //
    // Query-specific functionality that may be adapted for the PlanExecutor.
    //

    PlanExecutor* getExecutor() const {
        return _exec.get();
    }
    int queryOptions() const {
        return _queryOptions;
    }
    const BSONObj& getQuery() const {
        return _query;
    }

    // Used by ops/query.cpp to stash how many results have been returned by a query.
    long long pos() const {
        return _pos;
    }
    void incPos(long long n) {
        _pos += n;
    }
    void setPos(long long n) {
        _pos = n;
    }

    static long long totalOpen();

private:
    friend class CursorManager;
    friend class ClientCursorPin;

    /**
     * Only friends are allowed to destroy ClientCursor objects.
     */
    ~ClientCursor();

    /**
     * Initialization common between both constructors for the ClientCursor. The database must
     * be stable when this is called, because cursors hang off the collection.
     */
    void init();

    //
    // ClientCursor-specific data, independent of the underlying execution type.
    //

    // The ID of the ClientCursor.
    CursorId _cursorid;

    // The namespace we're operating on.
    std::string _ns;

    const bool _isReadCommitted;

    CursorManager* _cursorManager;

    // if we've added it to the total open counter yet
    bool _countedYet;

    // How many objects have been returned by the find() so far?
    long long _pos;

    // If this cursor was created by a find operation, '_query' holds the query predicate for
    // the find. If this cursor was created by a command (e.g. the aggregate command), then
    // '_query' holds the command specification received from the client.
    BSONObj _query;

    // See the QueryOptions enum in dbclient.h
    int _queryOptions;

    // Is this ClientCursor backed by an aggregation pipeline?  Defaults to false.
    //
    // Agg executors differ from others in that they manage their own locking internally and
    // should not be killed or destroyed when the underlying collection is deleted.
    //
    // Note: This should *not* be set for the internal cursor used as input to an aggregation.
    const bool _isAggCursor;

    // Is this cursor in use?  Defaults to false.
    bool _isPinned;

    // Is the "no timeout" flag set on this cursor?  If false, this cursor may be targeted for
    // deletion after an interval of inactivity.  Defaults to false.
    bool _isNoTimeout;

    // The replication position only used in master-slave.
    Timestamp _slaveReadTill;

    // How long has the cursor been idle?
    int _idleAgeMillis;

    // Unused maxTime budget for this cursor.
    Microseconds _leftoverMaxTimeMicros = Microseconds::max();

    //
    // The underlying execution machinery.
    //
    std::unique_ptr<PlanExecutor> _exec;
};

/**
 * ClientCursorPin is an RAII class that manages the pinned state of a ClientCursor.
 * ClientCursorPin objects pin the given cursor upon construction, and release the pin upon
 * destruction.
 *
 * A pin extends the lifetime of a ClientCursor object until the pin's release.  Pinned
 * ClientCursor objects cannot not be killed due to inactivity, and cannot be killed by user
 * kill requests.  When a CursorManager is destroyed (e.g. by a collection drop), ownership of
 * any still-pinned ClientCursor objects is transferred to their managing ClientCursorPin
 * objects.
 *
 * Example usage:
 * {
 *     ClientCursorPin pin(cursorManager, cursorid);
 *     ClientCursor* cursor = pin.c();
 *     if (cursor) {
 *         // Use cursor.
 *     }
 *     // Pin automatically released on block exit.
 * }
 *
 * Clients that wish to access ClientCursor objects owned by collection cursor managers must
 * hold the collection lock during pin acquisition and pin release.  This guards from a
 * collection drop (which requires an exclusive lock on the collection) occurring concurrently
 * with the pin request or unpin request.
 *
 * Clients that wish to access ClientCursor objects owned by the global cursor manager need not
 * hold any locks; the global cursor manager can only be destroyed by a process exit.
 */
class ClientCursorPin {
    MONGO_DISALLOW_COPYING(ClientCursorPin);

public:
    /**
     * Asks "cursorManager" to set a pin on the ClientCursor associated with "cursorid".  If no
     * such cursor exists, does nothing.  If the cursor is already pinned, throws a
     * UserException.
     */
    ClientCursorPin(CursorManager* cursorManager, long long cursorid);

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

    ClientCursor* c() const;

private:
    ClientCursor* _cursor;
};

void startClientCursorMonitor();

}  // namespace mongo
