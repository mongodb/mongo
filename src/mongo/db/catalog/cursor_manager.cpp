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
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/random.h"
#include "mongo/util/exit.h"
#include "mongo/util/startup_test.h"

namespace mongo {

using std::vector;

namespace {
uint32_t idFromCursorId(CursorId id) {
    uint64_t x = static_cast<uint64_t>(id);
    x = x >> 32;
    return static_cast<uint32_t>(x);
}

CursorId cursorIdFromParts(uint32_t collectionIdentifier, uint32_t cursor) {
    // The leading two bits of a non-global CursorId should be 0.
    invariant((collectionIdentifier & (0b11 << 30)) == 0);
    CursorId x = static_cast<CursorId>(collectionIdentifier) << 32;
    x |= cursor;
    return x;
}
}  // namespace

class GlobalCursorIdCache {
public:
    GlobalCursorIdCache();
    ~GlobalCursorIdCache();

    /**
     * Returns a unique 32-bit identifier to be used as the first 32 bits of all cursor ids for a
     * new CursorManager.
     */
    uint32_t registerCursorManager(const NamespaceString& nss);

    /**
     * Must be called when a CursorManager is deleted. 'id' must be the identifier returned by
     * registerCursorManager().
     */
    void deregisterCursorManager(uint32_t id, const NamespaceString& nss);

    /**
     * works globally
     */
    bool eraseCursor(OperationContext* opCtx, CursorId id, bool checkAuth);

    void appendStats(BSONObjBuilder& builder);

    std::size_t timeoutCursors(OperationContext* opCtx, int millisSinceLastCall);

    int64_t nextSeed();

private:
    SimpleMutex _mutex;

    typedef unordered_map<unsigned, NamespaceString> Map;
    Map _idToNss;
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
    globalCursorManager.reset(new CursorManager({}));
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

uint32_t GlobalCursorIdCache::registerCursorManager(const NamespaceString& nss) {
    static const uint32_t kMaxIds = 1000 * 1000 * 1000;
    static_assert((kMaxIds & (0b11 << 30)) == 0,
                  "the first two bits of a collection identifier must always be zeroes");

    stdx::lock_guard<SimpleMutex> lk(_mutex);

    fassert(17359, _idToNss.size() < kMaxIds);

    for (uint32_t i = 0; i <= kMaxIds; i++) {
        uint32_t id = ++_nextId;
        if (id == 0)
            continue;
        if (_idToNss.count(id) > 0)
            continue;
        _idToNss[id] = nss;
        return id;
    }

    MONGO_UNREACHABLE;
}

void GlobalCursorIdCache::deregisterCursorManager(uint32_t id, const NamespaceString& nss) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    invariant(nss == _idToNss[id]);
    _idToNss.erase(id);
}

bool GlobalCursorIdCache::eraseCursor(OperationContext* opCtx, CursorId id, bool checkAuth) {
    // Figure out what the namespace of this cursor is.
    NamespaceString nss;
    if (CursorManager::isGloballyManagedCursor(id)) {
        auto pin = globalCursorManager->pinCursor(opCtx, id);
        if (!pin.isOK()) {
            invariant(pin == ErrorCodes::CursorNotFound);
            // No such cursor.  TODO: Consider writing to audit log here (even though we don't
            // have a namespace).
            return false;
        }
        nss = pin.getValue().getCursor()->nss();
    } else {
        stdx::lock_guard<SimpleMutex> lk(_mutex);
        uint32_t nsid = idFromCursorId(id);
        Map::const_iterator it = _idToNss.find(nsid);
        if (it == _idToNss.end()) {
            // No namespace corresponding to this cursor id prefix.  TODO: Consider writing to
            // audit log here (even though we don't have a namespace).
            return false;
        }
        nss = it->second;
    }
    invariant(nss.isValid());

    // Check if we are authorized to erase this cursor.
    if (checkAuth) {
        AuthorizationSession* as = AuthorizationSession::get(opCtx->getClient());
        Status authorizationStatus = as->checkAuthForKillCursors(nss, id);
        if (!authorizationStatus.isOK()) {
            audit::logKillCursorsAuthzCheck(opCtx->getClient(), nss, id, ErrorCodes::Unauthorized);
            return false;
        }
    }

    // If this cursor is owned by the global cursor manager, ask it to erase the cursor for us.
    if (CursorManager::isGloballyManagedCursor(id)) {
        Status eraseStatus = globalCursorManager->eraseCursor(opCtx, id, checkAuth);
        massert(28697,
                eraseStatus.reason(),
                eraseStatus.code() == ErrorCodes::OK ||
                    eraseStatus.code() == ErrorCodes::CursorNotFound);
        return eraseStatus.isOK();
    }

    // If not, then the cursor must be owned by a collection.  Erase the cursor under the
    // collection lock (to prevent the collection from going away during the erase).
    AutoGetCollectionForReadCommand ctx(opCtx, nss);
    Collection* collection = ctx.getCollection();
    if (!collection) {
        if (checkAuth)
            audit::logKillCursorsAuthzCheck(
                opCtx->getClient(), nss, id, ErrorCodes::CursorNotFound);
        return false;
    }

    Status eraseStatus = collection->getCursorManager()->eraseCursor(opCtx, id, checkAuth);
    massert(16089,
            eraseStatus.reason(),
            eraseStatus.code() == ErrorCodes::OK ||
                eraseStatus.code() == ErrorCodes::CursorNotFound);
    return eraseStatus.isOK();
}

std::size_t GlobalCursorIdCache::timeoutCursors(OperationContext* opCtx, int millisSinceLastCall) {
    size_t totalTimedOut = 0;

    // Time out the cursors from the global cursor manager.
    totalTimedOut += globalCursorManager->timeoutCursors(opCtx, millisSinceLastCall);

    // Compute the set of collection names that we have to time out cursors for.
    vector<NamespaceString> todo;
    {
        stdx::lock_guard<SimpleMutex> lk(_mutex);
        for (auto&& entry : _idToNss) {
            todo.push_back(entry.second);
        }
    }

    // For each collection, time out its cursors under the collection lock (to prevent the
    // collection from going away during the erase).
    for (unsigned i = 0; i < todo.size(); i++) {
        AutoGetCollectionOrViewForReadCommand ctx(opCtx, NamespaceString(todo[i]));
        if (!ctx.getDb()) {
            continue;
        }

        Collection* collection = ctx.getCollection();
        if (collection == NULL) {
            continue;
        }

        totalTimedOut += collection->getCursorManager()->timeoutCursors(opCtx, millisSinceLastCall);
    }

    return totalTimedOut;
}

// ---

CursorManager* CursorManager::getGlobalCursorManager() {
    return globalCursorManager.get();
}

std::size_t CursorManager::timeoutCursorsGlobal(OperationContext* opCtx, int millisSinceLastCall) {
    return globalCursorIdCache->timeoutCursors(opCtx, millisSinceLastCall);
}

int CursorManager::eraseCursorGlobalIfAuthorized(OperationContext* opCtx, int n, const char* _ids) {
    ConstDataCursor ids(_ids);
    int numDeleted = 0;
    for (int i = 0; i < n; i++) {
        if (eraseCursorGlobalIfAuthorized(opCtx, ids.readAndAdvance<LittleEndian<int64_t>>()))
            numDeleted++;
        if (globalInShutdownDeprecated())
            break;
    }
    return numDeleted;
}
bool CursorManager::eraseCursorGlobalIfAuthorized(OperationContext* opCtx, CursorId id) {
    return globalCursorIdCache->eraseCursor(opCtx, id, true);
}
bool CursorManager::eraseCursorGlobal(OperationContext* opCtx, CursorId id) {
    return globalCursorIdCache->eraseCursor(opCtx, id, false);
}


// --------------------------


CursorManager::CursorManager(NamespaceString nss) : _nss(std::move(nss)) {
    if (!isGlobalManager()) {
        // Generate a unique id for this collection.
        _collectionCacheRuntimeId = globalCursorIdCache->registerCursorManager(_nss);
    }
    _random.reset(new PseudoRandom(globalCursorIdCache->nextSeed()));
}

CursorManager::~CursorManager() {
    if (!isGlobalManager()) {
        globalCursorIdCache->deregisterCursorManager(_collectionCacheRuntimeId, _nss);
    }
    invariant(_cursors.empty());
    invariant(_nonCachedExecutors.empty());
}

void CursorManager::invalidateAll(OperationContext* opCtx,
                                  bool collectionGoingAway,
                                  const std::string& reason) {
    invariant(!isGlobalManager());  // The global cursor manager should never need to kill cursors.
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    fassert(28819, !BackgroundOperation::inProgForNs(_nss));

    for (auto&& exec : _nonCachedExecutors) {
        // We kill the executor, but it deletes itself.
        exec->markAsKilled(reason);
    }
    _nonCachedExecutors.clear();

    CursorMap newMap;
    for (auto&& entry : _cursors) {
        auto* cursor = entry.second;
        cursor->markAsKilled(reason);

        if (cursor->_isPinned) {
            // There is an active user of this cursor, who is now responsible for cleaning it up.
            // This CursorManager will no longer track this cursor.
            continue;
        }

        if (!collectionGoingAway) {
            // We keep around unpinned cursors so that future attempts to use the cursor will result
            // in a useful error message.
            newMap.insert(entry);
        } else {
            // The collection is going away, so there's no point in keeping any state.
            cursor->dispose(opCtx);
            delete cursor;
        }
    }
    _cursors = newMap;
}

void CursorManager::invalidateDocument(OperationContext* opCtx,
                                       const RecordId& dl,
                                       InvalidationType type) {
    invariant(!isGlobalManager());  // The global cursor manager should never receive invalidations.
    if (supportsDocLocking()) {
        // If a storage engine supports doc locking, then we do not need to invalidate.
        // The transactional boundaries of the operation protect us.
        return;
    }

    stdx::lock_guard<SimpleMutex> lk(_mutex);

    for (ExecSet::iterator it = _nonCachedExecutors.begin(); it != _nonCachedExecutors.end();
         ++it) {
        PlanExecutor* exec = *it;
        exec->invalidate(opCtx, dl, type);
    }

    for (CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i) {
        PlanExecutor* exec = i->second->getExecutor();
        if (exec) {
            exec->invalidate(opCtx, dl, type);
        }
    }
}

std::size_t CursorManager::timeoutCursors(OperationContext* opCtx, int millisSinceLastCall) {
    vector<ClientCursor*> toDelete;

    stdx::lock_guard<SimpleMutex> lk(_mutex);

    for (CursorMap::const_iterator i = _cursors.begin(); i != _cursors.end(); ++i) {
        ClientCursor* cc = i->second;
        // shouldTimeout() ensures that we skip pinned cursors.
        if (cc->shouldTimeout(millisSinceLastCall))
            toDelete.push_back(cc);
    }

    // Properly dispose of each cursor that was timed out.
    for (vector<ClientCursor*>::const_iterator i = toDelete.begin(); i != toDelete.end(); ++i) {
        ClientCursor* cc = *i;
        _deregisterCursor_inlock(cc);
        cc->dispose(opCtx);
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

StatusWith<ClientCursorPin> CursorManager::pinCursor(OperationContext* opCtx, CursorId id) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    CursorMap::const_iterator it = _cursors.find(id);
    if (it == _cursors.end()) {
        return {ErrorCodes::CursorNotFound, str::stream() << "cursor id " << id << " not found"};
    }

    ClientCursor* cursor = it->second;
    uassert(12051, str::stream() << "cursor id " << id << " is already in use", !cursor->_isPinned);
    if (cursor->_killed) {
        // This cursor was killed while it was idle.
        invariant(cursor->getExecutor());  // We should never unpin RangePreserver cursors.
        Status error{ErrorCodes::QueryPlanKilled,
                     str::stream() << "cursor killed because: "
                                   << cursor->getExecutor()->getKillReason()};
        _deregisterCursor_inlock(cursor);
        cursor->dispose(opCtx);
        delete cursor;
        return error;
    }
    cursor->_isPinned = true;
    return ClientCursorPin(opCtx, cursor);
}

void CursorManager::unpin(ClientCursor* cursor) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);

    invariant(cursor->_isPinned);
    cursor->_isPinned = false;
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
        // The leading two bits of a CursorId are used to determine if the cursor is registered on
        // the global cursor manager.
        CursorId id;
        if (isGlobalManager()) {
            // This is the global cursor manager, so generate a random number and make sure the
            // first two bits are 01.
            uint64_t mask = 0x3FFFFFFFFFFFFFFF;
            uint64_t bitToSet = 1ULL << 62;
            id = ((_random->nextInt64() & mask) | bitToSet);
        } else {
            // The first 2 bits are 0, the next 30 bits are the collection identifier, the next 32
            // bits are random.
            uint32_t myPart = static_cast<uint32_t>(_random->nextInt32());
            id = cursorIdFromParts(_collectionCacheRuntimeId, myPart);
        }
        if (_cursors.count(id) == 0)
            return id;
    }
    fassertFailed(17360);
}

ClientCursorPin CursorManager::registerCursor(OperationContext* opCtx,
                                              ClientCursorParams&& cursorParams) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    // Make sure the PlanExecutor isn't registered, since we will register the ClientCursor wrapping
    // it.
    invariant(cursorParams.exec);
    _nonCachedExecutors.erase(cursorParams.exec.get());
    cursorParams.exec.get_deleter().dismissDisposal();
    cursorParams.exec->unsetRegistered();

    CursorId cursorId = _allocateCursorId_inlock();
    std::unique_ptr<ClientCursor, ClientCursor::Deleter> clientCursor(
        new ClientCursor(std::move(cursorParams), this, cursorId));
    return _registerCursor_inlock(opCtx, std::move(clientCursor));
}

ClientCursorPin CursorManager::registerRangePreserverCursor(OperationContext* opCtx,
                                                            const Collection* collection) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    CursorId cursorId = _allocateCursorId_inlock();
    std::unique_ptr<ClientCursor, ClientCursor::Deleter> clientCursor(
        new ClientCursor(collection, this, cursorId));
    return _registerCursor_inlock(opCtx, std::move(clientCursor));
}

ClientCursorPin CursorManager::_registerCursor_inlock(
    OperationContext* opCtx, std::unique_ptr<ClientCursor, ClientCursor::Deleter> clientCursor) {
    CursorId cursorId = clientCursor->cursorid();
    invariant(cursorId);

    // Transfer ownership of the cursor to '_cursors'.
    ClientCursor* unownedCursor = clientCursor.release();
    _cursors[cursorId] = unownedCursor;
    return ClientCursorPin(opCtx, unownedCursor);
}

void CursorManager::deregisterCursor(ClientCursor* cc) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);
    _deregisterCursor_inlock(cc);
}

Status CursorManager::eraseCursor(OperationContext* opCtx, CursorId id, bool shouldAudit) {
    stdx::lock_guard<SimpleMutex> lk(_mutex);

    CursorMap::iterator it = _cursors.find(id);
    if (it == _cursors.end()) {
        if (shouldAudit) {
            audit::logKillCursorsAuthzCheck(
                opCtx->getClient(), _nss, id, ErrorCodes::CursorNotFound);
        }
        return {ErrorCodes::CursorNotFound, str::stream() << "Cursor id not found: " << id};
    }

    auto cursor = it->second;

    if (cursor->_isPinned) {
        if (shouldAudit) {
            audit::logKillCursorsAuthzCheck(
                opCtx->getClient(), _nss, id, ErrorCodes::OperationFailed);
        }
        return {ErrorCodes::OperationFailed, str::stream() << "Cannot kill pinned cursor: " << id};
    }

    if (shouldAudit) {
        audit::logKillCursorsAuthzCheck(opCtx->getClient(), _nss, id, ErrorCodes::OK);
    }

    _deregisterCursor_inlock(cursor);
    cursor->dispose(opCtx);
    delete cursor;
    return Status::OK();
}

void CursorManager::_deregisterCursor_inlock(ClientCursor* cc) {
    invariant(cc);
    CursorId id = cc->cursorid();
    _cursors.erase(id);
}
}  // namespace mongo
