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

#include <utility>

#include "mongo/db/catalog/util/partitioned.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/generic_cursor.h"
#include "mongo/db/kill_sessions.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/session_killer.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

namespace mongo {

class AuthorizationSession;
class OperationContext;
class PseudoRandom;
class PlanExecutor;

/**
 * A container which owns ClientCursor objects. This class is used to create, access, and delete
 * ClientCursors. It is also responsible for allocating the cursor ids that are passed back to
 * clients.
 *
 * There is a process-global CursorManager on every mongod which is responsible for managing all
 * open cursors on the node. No lock manager locks are required to access this global cursor
 * manager. The CursorManager is internally synchronized, and unless otherwise noted its public
 * methods are thread-safe. For scalability in circumstances where many threads may be concurrently
 * accessing the CursorManager (i.e. a workload which runs many concurrent queries), the cursor
 * manager's underlying data structure is partitioned. Each partition is protected by its own latch.
 *
 * See clientcursor.h for more information.
 */
class CursorManager {
public:
    /**
     * Returns a pointer to the cursor manager defined within the specified ServiceContext.
     */
    static CursorManager* get(ServiceContext* svcCtx);

    /**
     * Returns a pointer to the cursor manager defined within the specified OperationContext.
     */
    static CursorManager* get(OperationContext* opCtx);

    /**
     * Registers the new cursor manager within the specified ServiceContext.
     */
    static void set(ServiceContext* svcCtx, std::unique_ptr<CursorManager> newCursorManager);


    CursorManager();

    /**
     * Destroys the cursor manager, deleting all managed cursors. Illegal to call if any managed
     * cursor is pinned.
     */
    ~CursorManager();

    /**
     * Destroys cursors that have been inactive for too long.
     *
     * Returns the number of cursors that were timed out.
     */
    std::size_t timeoutCursors(OperationContext* opCtx, Date_t now);

    /**
     * Constructs a new ClientCursor according to the given 'cursorParams'. The cursor is atomically
     * registered with the manager and returned in pinned state.
     */
    ClientCursorPin registerCursor(OperationContext* opCtx, ClientCursorParams&& cursorParams);

    /**
     * Pins and returns the cursor with the given id.
     *
     * Returns ErrorCodes::CursorNotFound if the cursor does not exist or
     * ErrorCodes::QueryPlanKilled if the cursor was killed in between uses.
     *
     * If 'checkSessionAuth' is 'kCheckSession' or left unspecified, this function also checks if
     * the current session in the specified 'opCtx' has privilege to access the cursor specified by
     * 'id.' In this case, this function returns a 'mongo::Status' with information regarding the
     * nature of the inaccessability when the cursor is not accessible. If 'kNoCheckSession' is
     * passed for 'checkSessionAuth,' this function does not check if the current session is
     * authorized to access the cursor with the given id.
     *
     * Throws a AssertionException if the cursor is already pinned. Callers need not specially
     * handle this error, as it should only happen if a misbehaving client attempts to
     * simultaneously issue two operations against the same cursor id.
     */
    enum AuthCheck { kCheckSession = true, kNoCheckSession = false };
    StatusWith<ClientCursorPin> pinCursor(OperationContext* opCtx,
                                          CursorId id,
                                          AuthCheck checkSessionAuth = kCheckSession);

    /**
     * Returns an OK status if the cursor was successfully killed, meaning either:
     * (1) The cursor was erased from the cursor registry
     * (2) The cursor's operation was interrupted, and the cursor will be cleaned up when the
     * operation next checks for interruption.
     * Case (2) will only occur if the cursor is pinned.
     *
     * Returns ErrorCodes::CursorNotFound if the cursor id is not owned by this manager. Returns
     * ErrorCodes::OperationFailed if attempting to erase a pinned cursor.
     *
     * If 'shouldAudit' is true, will perform audit logging.
     */
    Status killCursor(OperationContext* opCtx, CursorId id, bool shouldAudit);

    /**
     * Returns an OK status if we're authorized to erase the cursor. Otherwise, returns
     * ErrorCodes::Unauthorized.
     */
    Status checkAuthForKillCursors(OperationContext* opCtx, CursorId id);

    /**
     * Appends sessions that have open cursors in this cursor manager to the given set of lsids.
     * 'userMode': If auth is on, calling with userMode as kExcludeOthers will cause this function
     * to only return cursors owned by the caller. If auth is off, this argument does not matter.
     */
    void appendActiveSessions(LogicalSessionIdSet* lsids) const;

    /**
     * Returns a vector of all idle (non-pinned) cursors in this cursor manager.
     *
     * If auth is on, calling with 'userMode' as 'kExcludeOthers' will cause this function to only
     * return cursors owned by the caller. If auth is off, this argument does not matter.
     */
    std::vector<GenericCursor> getIdleCursors(
        OperationContext* opCtx, MongoProcessInterface::CurrentOpUserMode userMode) const;

    /*
     * Returns a list of all open cursors for the given session.
     */
    stdx::unordered_set<CursorId> getCursorsForSession(LogicalSessionId lsid) const;

    /*
     * Returns a list of all open cursors for the given set of OperationKeys.
     */
    stdx::unordered_set<CursorId> getCursorsForOpKeys(std::vector<OperationKey>) const;

    /**
     * Returns the number of ClientCursors currently registered.
     */
    std::size_t numCursors() const;

    /**
     * Kills cursors in this cursor manager with matching logical sessions. Returns a pair with the
     * overall Status of the operation and the number of cursors successfully killed.
     */
    std::pair<Status, int> killCursorsWithMatchingSessions(OperationContext* opCtx,
                                                           const SessionKiller::Matcher& matcher);

private:
    static constexpr int kNumPartitions = 16;
    friend class ClientCursorPin;

    CursorId allocateCursorId_inlock();

    ClientCursorPin _registerCursor(
        OperationContext* opCtx, std::unique_ptr<ClientCursor, ClientCursor::Deleter> clientCursor);

    void deregisterCursor(ClientCursor* cursor);
    void deregisterAndDestroyCursor(
        Partitioned<stdx::unordered_map<CursorId, ClientCursor*>, kNumPartitions>::OnePartition&&,
        OperationContext* opCtx,
        std::unique_ptr<ClientCursor, ClientCursor::Deleter> cursor);

    void unpin(OperationContext* opCtx,
               std::unique_ptr<ClientCursor, ClientCursor::Deleter> cursor);

    bool cursorShouldTimeout_inlock(const ClientCursor* cursor, Date_t now);

    template <class T>
    void removeCursorFromMap(T& map, ClientCursor* cursor) {
        // Remove from the opKey map first since erasing from the map may free the pointer for
        // 'cursor'.
        if (auto opKey = cursor->getOperationKey()) {
            stdx::lock_guard<Latch> lk(_opKeyMutex);
            _opKeyMap.erase(*opKey);
        }
        map->erase(cursor->cursorid());
    }

    // A CursorManager holds a pointer to all open ClientCursors. ClientCursors are owned by the
    // CursorManager, except when they are in use by a ClientCursorPin. When in use by a pin, an
    // unowned pointer remains to ensure they still receive kill notifications while in use.
    //
    // There are several mutexes at work to protect concurrent access to data structures managed by
    // this cursor manager. The '_cursorMap' is partitioned to decrease contention, and each
    // partition of the structure is protected by its own mutex. Separately, there is a
    // '_registrationLock' which protects concurrent access to '_random' for cursor id generation,
    // and must be held from cursor id generation until insertion into '_cursorMap'. If you ever
    // need to acquire more than one of these mutexes at once, you must follow the following rules:
    // - '_registrationLock' must be acquired first, if at all.
    // - Mutex(es) for '_cursorMap' must be acquired next.
    // - If you need to access multiple partitions within '_cursorMap' at once, you must acquire the
    // mutexes for those partitions in ascending order, or use the partition helpers to acquire
    // mutexes for all partitions.
    mutable SimpleMutex _registrationLock;
    std::unique_ptr<PseudoRandom> _random;
    std::unique_ptr<Partitioned<stdx::unordered_map<CursorId, ClientCursor*>, kNumPartitions>>
        _cursorMap;

    // A mapping from client OperationKey to corresponding CursorID. Note that it's possible that
    // cursors in the map above are not present in this map, since OperationKey is not required when
    // registering a cursor.
    mutable Mutex _opKeyMutex = MONGO_MAKE_LATCH("CursorManager::_opKeyMutex");
    stdx::unordered_map<OperationKey, CursorId, UUID::Hash> _opKeyMap;
};
}  // namespace mongo
