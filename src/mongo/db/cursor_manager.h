
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
 * There is a CursorManager per-collection and a global CursorManager. The global CursorManager owns
 * cursors whose lifetime is not tied to that of the collection and which do not need to receive
 * notifications about writes for a particular collection. In contrast, cursors owned by a
 * collection's CursorManager, unless pinned, are destroyed when the collection is destroyed.
 *
 * Callers must hold the collection lock in at least MODE_IS in order to access a collection's
 * CursorManager, which guards against the CursorManager being concurrently deleted due to a
 * catalog-level operation such as a collection drop. No locks are required to access the global
 * cursor manager.
 *
 * The CursorManager is internally synchronized; operations on a given collection may call methods
 * concurrently on that collection's CursorManager.
 *
 * See clientcursor.h for more information.
 */
class CursorManager {
public:
    /**
     * Appends the sessions that have open cursors on the global cursor manager and across
     * all collection-level cursor managers to the given set of lsids.
     */
    static void appendAllActiveSessions(OperationContext* opCtx, LogicalSessionIdSet* lsids);

    /**
     * Returns a list of GenericCursors for all idle cursors on the global cursor manager and across
     * all collection-level cursor managers. Does not include currently pinned cursors.
     * 'userMode': If auth is on, calling with userMode as kExcludeOthers will cause this function
     * to only return cursors owned by the caller. If auth is off, this argument does not matter.
     */
    static std::vector<GenericCursor> getIdleCursors(
        OperationContext* opCtx, MongoProcessInterface::CurrentOpUserMode userMode);

    /**
     * Kills cursors with matching logical sessions. Returns a pair with the overall
     * Status of the operation and the number of cursors successfully killed.
     */
    static std::pair<Status, int> killCursorsWithMatchingSessions(
        OperationContext* opCtx, const SessionKiller::Matcher& matcher);

    CursorManager(NamespaceString nss);

    /**
     * Destroys the CursorManager. All cursors must be cleaned up via invalidateAll() before
     * destruction.
     */
    ~CursorManager();

    /**
     * Kills all managed ClientCursors. Callers must have exclusive access to the collection (i.e.
     * must have the collection, database, or global resource locked in MODE_X).
     *
     * Must be called before the CursorManger is destroyed.
     *
     * 'collectionGoingAway' indicates whether the Collection instance is being deleted.  This could
     * be because the db is being closed, or the collection/db is being dropped.
     *
     * For any cursors that are in use by an active operation, ownership is transferred to the
     * respective cursor pin objects.
     *
     * The 'reason' is the motivation for invalidating all cursors. This will be used for error
     * reporting and logging when an operation finds that the cursor it was operating on has been
     * killed.
     */
    void invalidateAll(OperationContext* opCtx,
                       bool collectionGoingAway,
                       const std::string& reason);

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
     * Appends all idle (non-pinned) cursors in this cursor manager to the output vector.
     */
    void appendIdleCursors(AuthorizationSession* ctxAuth,
                           MongoProcessInterface::CurrentOpUserMode userMode,
                           std::vector<GenericCursor>* cursors) const;

    /*
     * Returns a list of all open cursors for the given session.
     */
    stdx::unordered_set<CursorId> getCursorsForSession(LogicalSessionId lsid) const;

    /**
     * Returns the number of ClientCursors currently registered.
     */
    std::size_t numCursors() const;

    static CursorManager* getGlobalCursorManager();

    /**
     * Returns true if this CursorId would be registered with the global CursorManager. Note that if
     * this method returns true it does not imply the cursor exists.
     */
    static bool isGloballyManagedCursor(CursorId cursorId) {
        // The first two bits are 01 for globally managed cursors, and 00 for cursors owned by a
        // collection. The leading bit is always 0 so that CursorIds do not appear as negative.
        const long long mask = static_cast<long long>(0b11) << 62;
        return (cursorId & mask) == (static_cast<long long>(0b01) << 62);
    }

    static int killCursorGlobalIfAuthorized(OperationContext* opCtx, int n, const char* ids);

    static bool killCursorGlobalIfAuthorized(OperationContext* opCtx, CursorId id);

    static bool killCursorGlobal(OperationContext* opCtx, CursorId id);

    /**
     * Deletes inactive cursors from the global cursor manager and from all per-collection cursor
     * managers. Returns the number of cursors that were timed out.
     */
    static std::size_t timeoutCursorsGlobal(OperationContext* opCtx, Date_t now);

    /**
     * Locate the correct cursor manager for a given cursorId and execute the provided callback.
     * Returns ErrorCodes::CursorNotFound if cursorId does not exist.
     */
    static Status withCursorManager(OperationContext* opCtx,
                                    CursorId id,
                                    const NamespaceString& nss,
                                    stdx::function<Status(CursorManager*)> callback);

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

    bool isGlobalManager() const {
        return _nss.isEmpty();
    }

    // No locks are needed to consult these data members.
    const NamespaceString _nss;
    const uint32_t _collectionCacheRuntimeId;

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
};
}  // namespace mongo
