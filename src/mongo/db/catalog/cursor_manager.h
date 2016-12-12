// cursor_manager.h

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


#include "mongo/db/clientcursor.h"
#include "mongo/db/invalidation_type.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/concurrency/mutex.h"

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
    CursorManager(StringData ns);

    /**
     * Destroys the CursorManager. Managed cursors which are not pinned are destroyed. Ownership of
     * pinned cursors is transferred to the corresponding ClientCursorPin.
     */
    ~CursorManager();

    /**
     * Kills all managed query executors and ClientCursors.
     *
     * 'collectionGoingAway' indicates whether the Collection instance is being deleted.  This could
     * be because the db is being closed, or the collection/db is being dropped. When passing
     * a 'collectionGoingAway' value of true, callers must have exclusive access to the collection
     * (i.e. must have the collection, database, or global resource locked in MODE_X).
     *
     * The 'reason' is the motivation for invalidating all cursors. This will be used for error
     * reporting and logging when an operation finds that the cursor it was operating on has been
     * killed.
     */
    void invalidateAll(bool collectionGoingAway, const std::string& reason);

    /**
     * Broadcast a document invalidation to all relevant PlanExecutor(s).  invalidateDocument
     * must called *before* the provided RecordId is about to be deleted or mutated.
     */
    void invalidateDocument(OperationContext* txn, const RecordId& dl, InvalidationType type);

    /**
     * Destroys cursors that have been inactive for too long.
     *
     * Returns the number of cursors that were timed out.
     */
    std::size_t timeoutCursors(int millisSinceLastCall);

    /**
     * Register an executor so that it can be notified of deletion/invalidation during yields.
     * Must be called before an executor yields.  If an executor is registered inside a
     * ClientCursor it must not be itself registered; the two are mutually exclusive.
     */
    void registerExecutor(PlanExecutor* exec);

    /**
     * Remove an executor from the registry.
     */
    void deregisterExecutor(PlanExecutor* exec);

    /**
     * Constructs a new ClientCursor according to the given 'cursorParams'. The cursor is atomically
     * registered with the manager and returned in pinned state.
     */
    ClientCursorPin registerCursor(const ClientCursorParams& cursorParams);

    /**
     * Constructs and pins a special ClientCursor used to track sharding state for the given
     * collection. See range_preserver.h for more details.
     */
    ClientCursorPin registerRangePreserverCursor(const Collection* collection);

    /**
     * Pins and returns the cursor with the given id.
     *
     * Returns ErrorCodes::CursorNotFound if the cursor does not exist.
     *
     * Throws a UserException if the cursor is already pinned. Callers need not specially handle
     * this error, as it should only happen if a misbehaving client attempts to simultaneously issue
     * two operations against the same cursor id.
     */
    StatusWith<ClientCursorPin> pinCursor(CursorId id);

    /**
     * Returns an OK status if the cursor was successfully erased.
     *
     * Returns error code CursorNotFound if the cursor id is not owned by this manager. Returns
     * error code OperationFailed if attempting to erase a pinned cursor.
     *
     * If 'shouldAudit' is true, will perform audit logging.
     */
    Status eraseCursor(OperationContext* txn, CursorId id, bool shouldAudit);

    /**
     * Returns true if the space of cursor ids that cursor manager is responsible for includes
     * the given cursor id.  Otherwise, returns false.
     *
     * The return value of this method does not indicate any information about whether or not a
     * cursor actually exists with the given cursor id.
     */
    bool ownsCursorId(CursorId cursorId) const;

    void getCursorIds(std::set<CursorId>* openCursors) const;

    /**
     * Returns the number of ClientCursors currently registered. Excludes any registered bare
     * PlanExecutors.
     */
    std::size_t numCursors() const;

    static CursorManager* getGlobalCursorManager();

    static int eraseCursorGlobalIfAuthorized(OperationContext* txn, int n, const char* ids);

    static bool eraseCursorGlobalIfAuthorized(OperationContext* txn, CursorId id);

    static bool eraseCursorGlobal(OperationContext* txn, CursorId id);

    /**
     * Deletes inactive cursors from the global cursor manager and from all per-collection cursor
     * managers. Returns the number of cursors that were timed out.
     */
    static std::size_t timeoutCursorsGlobal(OperationContext* txn, int millisSinceLastCall);

private:
    friend class ClientCursorPin;

    CursorId _allocateCursorId_inlock();
    void _deregisterCursor_inlock(ClientCursor* cc);
    ClientCursorPin _registerCursor_inlock(
        std::unique_ptr<ClientCursor, ClientCursor::Deleter> clientCursor);

    void deregisterCursor(ClientCursor* cc);

    void unpin(ClientCursor* cursor);

    NamespaceString _nss;
    unsigned _collectionCacheRuntimeId;
    std::unique_ptr<PseudoRandom> _random;

    mutable SimpleMutex _mutex;

    typedef unordered_set<PlanExecutor*> ExecSet;
    ExecSet _nonCachedExecutors;

    typedef std::map<CursorId, ClientCursor*> CursorMap;
    CursorMap _cursors;
};
}  // namespace mongo
