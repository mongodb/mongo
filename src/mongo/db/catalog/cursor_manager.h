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

class CursorManager {
public:
    CursorManager(StringData ns);

    /**
     * will kill() all PlanExecutor instances it has
     */
    ~CursorManager();

    // -----------------

    /**
     * @param collectionGoingAway Pass as true if the Collection instance is going away.
     *                            This could be because the db is being closed, or the
     *                            collection/db is being dropped.
     * @param reason              The motivation for invalidating all cursors. Will be used
     *                            for error reporting and logging when an operation finds that
     *                            the cursor it was operating on has been killed.
     */
    void invalidateAll(bool collectionGoingAway, const std::string& reason);

    /**
     * Broadcast a document invalidation to all relevant PlanExecutor(s).  invalidateDocument
     * must called *before* the provided RecordId is about to be deleted or mutated.
     */
    void invalidateDocument(OperationContext* txn, const RecordId& dl, InvalidationType type);

    /*
     * timesout cursors that have been idle for too long
     * note: must have a readlock on the collection
     * @return number timed out
     */
    std::size_t timeoutCursors(int millisSinceLastCall);

    // -----------------

    /**
     * Register an executor so that it can be notified of deletion/invalidation during yields.
     * Must be called before an executor yields.  If an executor is cached (inside a
     * ClientCursor) it MUST NOT be registered; the two are mutually exclusive.
     */
    void registerExecutor(PlanExecutor* exec);

    /**
     * Remove an executor from the registry.
     */
    void deregisterExecutor(PlanExecutor* exec);

    // -----------------

    CursorId registerCursor(ClientCursor* cc);
    void deregisterCursor(ClientCursor* cc);

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
     * cursor actually exists with the given cursor id.  Use the find() method for that purpose.
     */
    bool ownsCursorId(CursorId cursorId) const;

    void getCursorIds(std::set<CursorId>* openCursors) const;
    std::size_t numCursors() const;

    /**
     * @param pin - if true, will try to pin cursor
     *                  if pinned already, will assert
     *                  otherwise will pin
     */
    ClientCursor* find(CursorId id, bool pin);

    void unpin(ClientCursor* cursor);

    // ----------------------

    static CursorManager* getGlobalCursorManager();

    static int eraseCursorGlobalIfAuthorized(OperationContext* txn, int n, const char* ids);
    static bool eraseCursorGlobalIfAuthorized(OperationContext* txn, CursorId id);

    static bool eraseCursorGlobal(OperationContext* txn, CursorId id);

    /**
     * @return number timed out
     */
    static std::size_t timeoutCursorsGlobal(OperationContext* txn, int millisSinceLastCall);

private:
    CursorId _allocateCursorId_inlock();
    void _deregisterCursor_inlock(ClientCursor* cc);

    NamespaceString _nss;
    unsigned _collectionCacheRuntimeId;
    std::unique_ptr<PseudoRandom> _random;

    mutable SimpleMutex _mutex;

    typedef unordered_set<PlanExecutor*> ExecSet;
    ExecSet _nonCachedExecutors;

    typedef std::map<CursorId, ClientCursor*> CursorMap;
    CursorMap _cursors;
};
}
