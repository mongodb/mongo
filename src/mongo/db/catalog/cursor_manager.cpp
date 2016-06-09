// cursor_manager.cpp

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

#include "mongo/db/catalog/cursor_manager.h"

#include "mongo/base/data_cursor.h"
#include "mongo/base/init.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/random.h"
#include "mongo/util/exit.h"
#include "mongo/util/startup_test.h"

namespace mongo {

using std::string;
using std::vector;

namespace {
unsigned idFromCursorId(CursorId id) {
    uint64_t x = static_cast<uint64_t>(id);
    x = x >> 32;
    return static_cast<unsigned>(x);
}

CursorId cursorIdFromParts(unsigned collection, unsigned cursor) {
    CursorId x = static_cast<CursorId>(collection) << 32;
    x |= cursor;
    return x;
}

class IdWorkTest : public StartupTest {
public:
    void _run(unsigned a, unsigned b) {
        CursorId x = cursorIdFromParts(a, b);
        invariant(a == idFromCursorId(x));
        CursorId y = cursorIdFromParts(a, b + 1);
        invariant(x != y);
    }

    void run() {
        _run(123, 456);
        _run(0xdeadbeef, 0xcafecafe);
        _run(0, 0);
        _run(99999999, 999);
        _run(0xFFFFFFFF, 1);
        _run(0xFFFFFFFF, 0);
        _run(0xFFFFFFFF, 0xFFFFFFFF);
    }
} idWorkTest;
}

class GlobalCursorIdCache {
public:
    GlobalCursorIdCache();
    ~GlobalCursorIdCache();

    /**
     * this gets called when a CursorManager gets created
     * @return the id the CursorManager should use when generating
     * cursor ids
     */
    unsigned created(const std::string& ns);

    /**
     * called by CursorManager when its going away
     */
    void destroyed(unsigned id, const std::string& ns);

    /**
     * works globally
     */
    bool eraseCursor(OperationContext* txn, CursorId id, bool checkAuth);

    void appendStats(BSONObjBuilder& builder);

    std::size_t timeoutCursors(OperationContext* txn, int millisSinceLastCall);

    int64_t nextSeed();

private:
    SimpleMutex _mutex;

    typedef unordered_map<unsigned, string> Map;
    Map _idToNS;
    unsigned _nextId;

    std::unique_ptr<SecureRandom> _secureRandom;
};

// Note that "globalCursorIdCache" must be declared before "globalCursorManager", as the latter
// calls into the former during destruction.
std::unique_ptr<GlobalCursorIdCache> globalCursorIdCache;
std::unique_ptr<CursorManager> globalCursorManager;

MONGO_INITIALIZER(GlobalCursorIdCache)(InitializerContext* context) {
    globalCursorIdCache.reset(new GlobalCursorIdCache());
    return Status::OK();
}

MONGO_INITIALIZER_WITH_PREREQUISITES(GlobalCursorManager, ("GlobalCursorIdCache"))
(InitializerContext* context) {
    globalCursorManager.reset(new CursorManager(""));
    return Status::OK();
}

GlobalCursorIdCache::GlobalCursorIdCache() : _nextId(0), _secureRandom() {}

GlobalCursorIdCache::~GlobalCursorIdCache() {}

int64_t GlobalCursorIdCache::nextSeed() {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    if (!_secureRandom)
        _secureRandom.reset(SecureRandom::create());
    return _secureRandom->nextInt64();
}

unsigned GlobalCursorIdCache::created(const std::string& ns) {
    static const unsigned MAX_IDS = 1000 * 1000 * 1000;

    stdx::lock_guard<SimpleMutex> lk(_mutex);

    fassert(17359, _idToNS.size() < MAX_IDS);

    for (unsigned i = 0; i <= MAX_IDS; i++) {
        unsigned id = ++_nextId;
        if (id == 0)
            continue;
        if (_idToNS.count(id) > 0)
            continue;
        _idToNS[id] = ns;
        return id;
    }

    invariant(false);
}

void GlobalCursorIdCache::destroyed(unsigned id, const std::string& ns) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    invariant(ns == _idToNS[id]);
    _idToNS.erase(id);
}

bool GlobalCursorIdCache::eraseCursor(OperationContext* txn, CursorId id, bool checkAuth) {
    // Figure out what the namespace of this cursor is.
    std::string ns;
    if (globalCursorManager->ownsCursorId(id)) {
        ClientCursorPin pin(globalCursorManager.get(), id);
        if (!pin.c()) {
            // No such cursor.  TODO: Consider writing to audit log here (even though we don't
            // have a namespace).
            return false;
        }
        ns = pin.c()->ns();
    } else {
        stdx::lock_guard<SimpleMutex> lk(_mutex);
        unsigned nsid = idFromCursorId(id);
        Map::const_iterator it = _idToNS.find(nsid);
        if (it == _idToNS.end()) {
            // No namespace corresponding to this cursor id prefix.  TODO: Consider writing to
            // audit log here (even though we don't have a namespace).
            return false;
        }
        ns = it->second;
    }
    const NamespaceString nss(ns);
    invariant(nss.isValid());

    // Check if we are authorized to erase this cursor.
    if (checkAuth) {
        AuthorizationSession* as = AuthorizationSession::get(txn->getClient());
        Status authorizationStatus = as->checkAuthForKillCursors(nss, id);
        if (!authorizationStatus.isOK()) {
            audit::logKillCursorsAuthzCheck(txn->getClient(), nss, id, ErrorCodes::Unauthorized);
            return false;
        }
    }

    // If this cursor is owned by the global cursor manager, ask it to erase the cursor for us.
    if (globalCursorManager->ownsCursorId(id)) {
        Status eraseStatus = globalCursorManager->eraseCursor(txn, id, checkAuth);
        massert(28697,
                eraseStatus.reason(),
                eraseStatus.code() == ErrorCodes::OK ||
                    eraseStatus.code() == ErrorCodes::CursorNotFound);
        return eraseStatus.isOK();
    }

    // If not, then the cursor must be owned by a collection.  Erase the cursor under the
    // collection lock (to prevent the collection from going away during the erase).
    AutoGetCollectionForRead ctx(txn, nss);
    Collection* collection = ctx.getCollection();
    if (!collection) {
        if (checkAuth)
            audit::logKillCursorsAuthzCheck(txn->getClient(), nss, id, ErrorCodes::CursorNotFound);
        return false;
    }

    Status eraseStatus = collection->getCursorManager()->eraseCursor(txn, id, checkAuth);
    massert(16089,
            eraseStatus.reason(),
            eraseStatus.code() == ErrorCodes::OK ||
                eraseStatus.code() == ErrorCodes::CursorNotFound);
    return eraseStatus.isOK();
}

std::size_t GlobalCursorIdCache::timeoutCursors(OperationContext* txn, int millisSinceLastCall) {
    size_t totalTimedOut = 0;

    // Time out the cursors from the global cursor manager.
    totalTimedOut += globalCursorManager->timeoutCursors(millisSinceLastCall);

    // Compute the set of collection names that we have to time out cursors for.
    vector<string> todo;
    {
        stdx::lock_guard<SimpleMutex> lk(_mutex);
        for (Map::const_iterator i = _idToNS.begin(); i != _idToNS.end(); ++i) {
            if (globalCursorManager->ownsCursorId(cursorIdFromParts(i->first, 0))) {
                // Skip the global cursor manager, since we handle it above (and it's not
                // associated with a collection).
                continue;
            }
            todo.push_back(i->second);
        }
    }

    // For each collection, time out its cursors under the collection lock (to prevent the
    // collection from going away during the erase).
    for (unsigned i = 0; i < todo.size(); i++) {
        const std::string& ns = todo[i];

        AutoGetCollectionForRead ctx(txn, ns);
        if (!ctx.getDb()) {
            continue;
        }

        Collection* collection = ctx.getCollection();
        if (collection == NULL) {
            continue;
        }

        totalTimedOut += collection->getCursorManager()->timeoutCursors(millisSinceLastCall);
    }

    return totalTimedOut;
}

// ---

CursorManager* CursorManager::getGlobalCursorManager() {
    return globalCursorManager.get();
}

std::size_t CursorManager::timeoutCursorsGlobal(OperationContext* txn, int millisSinceLastCall) {
    return globalCursorIdCache->timeoutCursors(txn, millisSinceLastCall);
}

int CursorManager::eraseCursorGlobalIfAuthorized(OperationContext* txn, int n, const char* _ids) {
    ConstDataCursor ids(_ids);
    int numDeleted = 0;
    for (int i = 0; i < n; i++) {
        if (eraseCursorGlobalIfAuthorized(txn, ids.readAndAdvance<LittleEndian<int64_t>>()))
            numDeleted++;
        if (inShutdown())
            break;
    }
    return numDeleted;
}
bool CursorManager::eraseCursorGlobalIfAuthorized(OperationContext* txn, CursorId id) {
    return globalCursorIdCache->eraseCursor(txn, id, true);
}
bool CursorManager::eraseCursorGlobal(OperationContext* txn, CursorId id) {
    return globalCursorIdCache->eraseCursor(txn, id, false);
}


// --------------------------


CursorManager::CursorManager(StringData ns) : _nss(ns) {
    _collectionCacheRuntimeId = globalCursorIdCache->created(_nss.ns());
    _random.reset(new PseudoRandom(globalCursorIdCache->nextSeed()));
}

CursorManager::~CursorManager() {
    invalidateAll(true, "collection going away");
    globalCursorIdCache->destroyed(_collectionCacheRuntimeId, _nss.ns());
}

void CursorManager::invalidateAll(bool collectionGoingAway, const std::string& reason) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    fassert(28819, !BackgroundOperation::inProgForNs(_nss));

    for (ExecSet::iterator it = _nonCachedExecutors.begin(); it != _nonCachedExecutors.end();
         ++it) {
        // we kill the executor, but it deletes itself
        PlanExecutor* exec = *it;
        exec->kill(reason);
    }
    _nonCachedExecutors.clear();

    if (collectionGoingAway) {
        // we're going to wipe out the world
        for (CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i) {
            ClientCursor* cc = i->second;

            cc->kill();

            // If the CC is pinned, somebody is actively using it and we do not delete it.
            // Instead we notify the holder that we killed it.  The holder will then delete the
            // CC.
            //
            // If the CC is not pinned, there is nobody actively holding it.  We can safely
            // delete it.
            if (!cc->isPinned()) {
                delete cc;
            }
        }
    } else {
        CursorMap newMap;

        // collection will still be around, just all PlanExecutors are invalid
        for (CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i) {
            ClientCursor* cc = i->second;

            // Note that a valid ClientCursor state is "no cursor no executor."  This is because
            // the set of active cursor IDs in ClientCursor is used as representation of query
            // state.  See sharding_block.h.  TODO(greg,hk): Move this out.
            if (NULL == cc->getExecutor()) {
                newMap.insert(*i);
                continue;
            }

            if (cc->isPinned() || cc->isAggCursor()) {
                // Pinned cursors need to stay alive, so we leave them around.  Aggregation
                // cursors also can stay alive (since they don't have their lifetime bound to
                // the underlying collection).  However, if they have an associated executor, we
                // need to kill it, because it's now invalid.
                if (cc->getExecutor())
                    cc->getExecutor()->kill(reason);
                newMap.insert(*i);
            } else {
                cc->kill();
                delete cc;
            }
        }

        _cursors = newMap;
    }
}

void CursorManager::invalidateDocument(OperationContext* txn,
                                       const RecordId& dl,
                                       InvalidationType type) {
    if (supportsDocLocking()) {
        // If a storage engine supports doc locking, then we do not need to invalidate.
        // The transactional boundaries of the operation protect us.
        return;
    }

    stdx::lock_guard<SimpleMutex> lk(_mutex);

    for (ExecSet::iterator it = _nonCachedExecutors.begin(); it != _nonCachedExecutors.end();
         ++it) {
        PlanExecutor* exec = *it;
        exec->invalidate(txn, dl, type);
    }

    for (CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i) {
        PlanExecutor* exec = i->second->getExecutor();
        if (exec) {
            exec->invalidate(txn, dl, type);
        }
    }
}

std::size_t CursorManager::timeoutCursors(int millisSinceLastCall) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);

    vector<ClientCursor*> toDelete;

    for (CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i) {
        ClientCursor* cc = i->second;
        if (cc->shouldTimeout(millisSinceLastCall))
            toDelete.push_back(cc);
    }

    for (vector<ClientCursor*>::const_iterator i = toDelete.begin(); i != toDelete.end(); ++i) {
        ClientCursor* cc = *i;
        _deregisterCursor_inlock(cc);
        cc->kill();
        delete cc;
    }

    return toDelete.size();
}

void CursorManager::registerExecutor(PlanExecutor* exec) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    const std::pair<ExecSet::iterator, bool> result = _nonCachedExecutors.insert(exec);
    invariant(result.second);  // make sure this was inserted
}

void CursorManager::deregisterExecutor(PlanExecutor* exec) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    _nonCachedExecutors.erase(exec);
}

ClientCursor* CursorManager::find(CursorId id, bool pin) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    CursorMap::const_iterator it = _cursors.find(id);
    if (it == _cursors.end())
        return NULL;

    ClientCursor* cursor = it->second;
    if (pin) {
        uassert(12051, "clientcursor already in use? driver problem?", !cursor->isPinned());
        cursor->setPinned();
    }

    return cursor;
}

void CursorManager::unpin(ClientCursor* cursor) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);

    invariant(cursor->isPinned());
    cursor->unsetPinned();
}

bool CursorManager::ownsCursorId(CursorId cursorId) const {
    return _collectionCacheRuntimeId == idFromCursorId(cursorId);
}

void CursorManager::getCursorIds(std::set<CursorId>* openCursors) const {
    stdx::lock_guard<SimpleMutex> lk(_mutex);

    for (CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i) {
        ClientCursor* cc = i->second;
        openCursors->insert(cc->cursorid());
    }
}

size_t CursorManager::numCursors() const {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    return _cursors.size();
}

CursorId CursorManager::_allocateCursorId_inlock() {
    for (int i = 0; i < 10000; i++) {
        unsigned mypart = static_cast<unsigned>(_random->nextInt32());
        CursorId id = cursorIdFromParts(_collectionCacheRuntimeId, mypart);
        if (_cursors.count(id) == 0)
            return id;
    }
    fassertFailed(17360);
}

CursorId CursorManager::registerCursor(ClientCursor* cc) {
    invariant(cc);
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    CursorId id = _allocateCursorId_inlock();
    _cursors[id] = cc;
    return id;
}

void CursorManager::deregisterCursor(ClientCursor* cc) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    _deregisterCursor_inlock(cc);
}

Status CursorManager::eraseCursor(OperationContext* txn, CursorId id, bool shouldAudit) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);

    CursorMap::iterator it = _cursors.find(id);
    if (it == _cursors.end()) {
        if (shouldAudit) {
            audit::logKillCursorsAuthzCheck(txn->getClient(), _nss, id, ErrorCodes::CursorNotFound);
        }
        return {ErrorCodes::CursorNotFound, str::stream() << "Cursor id not found: " << id};
    }

    ClientCursor* cursor = it->second;

    if (cursor->isPinned()) {
        if (shouldAudit) {
            audit::logKillCursorsAuthzCheck(
                txn->getClient(), _nss, id, ErrorCodes::OperationFailed);
        }
        return {ErrorCodes::OperationFailed, str::stream() << "Cannot kill pinned cursor: " << id};
    }

    if (shouldAudit) {
        audit::logKillCursorsAuthzCheck(txn->getClient(), _nss, id, ErrorCodes::OK);
    }

    cursor->kill();
    _deregisterCursor_inlock(cursor);
    delete cursor;
    return Status::OK();
}

void CursorManager::_deregisterCursor_inlock(ClientCursor* cc) {
    invariant(cc);
    CursorId id = cc->cursorid();
    _cursors.erase(id);
}
}
