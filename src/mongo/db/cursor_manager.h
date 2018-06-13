/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include <utility>

#include "mongo/db/catalog/util/partitioned.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/generic_cursor.h"
#include "mongo/db/invalidation_type.h"
#include "mongo/db/kill_sessions.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/session_killer.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/duration.h"

namespace mongo {

class OperationContext;
class PseudoRandom;
class PlanExecutor;

/**
 * A container which owns ClientCursor objects. This class is used to create, access, and delete
 * ClientCursors. It is also responsible for allocating the cursor ids that are passed back to
 * clients.
 *
 * In addition to managing the lifetime of ClientCursors, the CursorManager is responsible for
 * notifying yielded queries of write operations and collection drops. For this reason, query
 * PlanExecutor objects which are not contained within a ClientCursor are also registered with the
 * CursorManager. Query executors must be registered with the CursorManager, either as a bare
 * PlanExecutor or inside a ClientCursor (but cannot be registered in both ways).
 *
 * There is a CursorManager per-collection and a global CursorManager. The global CursorManager owns
 * cursors whose lifetime is not tied to that of the collection and which do not need to receive
 * notifications about writes for a particular collection. In contrast, cursors owned by a
 * collection's CursorManager, unless pinned, are destroyed when the collection is destroyed. Such
 * cursors receive notifications about writes to the collection.
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
    using RegistrationToken = Partitioned<stdx::unordered_set<PlanExecutor*>>::PartitionId;

    /**
     * Appends the sessions that have open cursors on the global cursor manager and across
     * all collection-level cursor managers to the given set of lsids.
     */
    static void appendAllActiveSessions(OperationContext* opCtx, LogicalSessionIdSet* lsids);

    /**
     * Returns a list of GenericCursors for all cursors on the global cursor manager and across all
     * collection-level cursor maangers.
     */
    static std::vector<GenericCursor> getAllCursors(OperationContext* opCtx);

    /**
     * Kills cursors with matching logical sessions. Returns a pair with the overall
     * Status of the operation and the number of cursors successfully killed.
     */
    static std::pair<Status, int> killCursorsWithMatchingSessions(
        OperationContext* opCtx, const SessionKiller::Matcher& matcher);

    CursorManager(NamespaceString nss);

    /**
     * Destroys the CursorManager. All cursors and PlanExecutors must be cleaned up via
     * invalidateAll() before destruction.
     */
    ~CursorManager();

    /**
     * Kills all managed query executors and ClientCursors. Callers must have exclusive access to
     * the collection (i.e. must have the collection, database, or global resource locked in
     * MODE_X).
     *
     * 'collectionGoingAway' indicates whether the Collection instance is being deleted.  This could
     * be because the db is being closed, or the collection/db is being dropped.
     *
     * The 'reason' is the motivation for invalidating all cursors. This will be used for error
     * reporting and logging when an operation finds that the cursor it was operating on has been
     * killed.
     */
    void invalidateAll(OperationContext* opCtx,
                       bool collectionGoingAway,
                       const std::string& reason);

    /**
     * Broadcast a document invalidation to all relevant PlanExecutor(s).  invalidateDocument
     * must called *before* the provided RecordId is about to be deleted or mutated.
     */
    void invalidateDocument(OperationContext* opCtx, const RecordId& dl, InvalidationType type);

    /**
     * Destroys cursors that have been inactive for too long.
     *
     * Returns the number of cursors that were timed out.
     */
    std::size_t timeoutCursors(OperationContext* opCtx, Date_t now);

    /**
     * Register an executor so that it can be notified of deletions, invalidations, collection
     * drops, or the like during yields. Must be called before an executor yields. Registration
     * happens automatically for yielding PlanExecutors, so this should only be called by a
     * PlanExecutor itself. Returns a token that must be stored for use during deregistration.
     */
    Partitioned<stdx::unordered_set<PlanExecutor*>>::PartitionId registerExecutor(
        PlanExecutor* exec);

    /**
     * Remove an executor from the registry. It is legal to call this even if 'exec' is not
     * registered.
     */
    void deregisterExecutor(PlanExecutor* exec);

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
     */
    void appendActiveSessions(LogicalSessionIdSet* lsids) const;

    /**
     * Appends all active cursors in this cursor manager to the output vector.
     */
    void appendActiveCursors(std::vector<GenericCursor>* cursors) const;

    /*
     * Returns a list of all open cursors for the given session.
     */
    stdx::unordered_set<CursorId> getCursorsForSession(LogicalSessionId lsid) const;

    /**
     * Returns the number of ClientCursors currently registered. Excludes any registered bare
     * PlanExecutors.
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

    struct PlanExecutorPartitioner {
        std::size_t operator()(const PlanExecutor* exec, std::size_t nPartitions);
    };

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

    // A CursorManager holds a pointer to all open PlanExecutors and all open ClientCursors. All
    // pointers to PlanExecutors are unowned, and a PlanExecutor will notify the CursorManager when
    // it is being destroyed. ClientCursors are owned by the CursorManager, except when they are in
    // use by a ClientCursorPin. When in use by a pin, an unowned pointer remains to ensure they
    // still receive invalidations while in use.
    //
    // There are several mutexes at work to protect concurrent access to data structures managed by
    // this cursor manager. The two registration data structures '_registeredPlanExecutors' and
    // '_cursorMap' are partitioned to decrease contention, and each partition of the structure is
    // protected by its own mutex. Separately, there is a '_registrationLock' which protects
    // concurrent access to '_random' for cursor id generation, and must be held from cursor id
    // generation until insertion into '_cursorMap'. If you ever need to acquire more than one of
    // these mutexes at once, you must follow the following rules:
    // - '_registrationLock' must be acquired first, if at all.
    // - Mutex(es) for '_registeredPlanExecutors' must be acquired next.
    // - Mutex(es) for '_cursorMap' must be acquired next.
    // - If you need to access multiple partitions within '_registeredPlanExecutors' or '_cursorMap'
    //   at once, you must acquire the mutexes for those partitions in ascending order, or use the
    //   partition helpers to acquire mutexes for all partitions.
    mutable SimpleMutex _registrationLock;
    std::unique_ptr<PseudoRandom> _random;
    Partitioned<stdx::unordered_set<PlanExecutor*>, kNumPartitions, PlanExecutorPartitioner>
        _registeredPlanExecutors;
    std::unique_ptr<Partitioned<stdx::unordered_map<CursorId, ClientCursor*>, kNumPartitions>>
        _cursorMap;
};
}  // namespace mongo
