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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/cursor_manager.h"

#include "mongo/base/data_cursor.h"
#include "mongo/base/init.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/cursor_server_params.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/kill_sessions_common.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_catalog.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/startup_test.h"

namespace mongo {

using std::vector;

constexpr int CursorManager::kNumPartitions;

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
    bool killCursor(OperationContext* opCtx, CursorId id, bool checkAuth);

    void appendStats(BSONObjBuilder& builder);

    std::size_t timeoutCursors(OperationContext* opCtx, Date_t now);

    template <typename Visitor>
    void visitAllCursorManagers(OperationContext* opCtx, Visitor* visitor);

    int64_t nextSeed();

private:
    // '_mutex' must not be held when acquiring a CursorManager mutex to avoid deadlock.
    SimpleMutex _mutex;

    using CursorIdToNssMap = stdx::unordered_map<CursorId, NamespaceString>;
    using IdToNssMap = stdx::unordered_map<unsigned, NamespaceString>;

    IdToNssMap _idToNss;
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
        _secureRandom = SecureRandom::create();
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

bool GlobalCursorIdCache::killCursor(OperationContext* opCtx, CursorId id, bool checkAuth) {
    // Figure out what the namespace of this cursor is.
    NamespaceString nss;
    if (CursorManager::isGloballyManagedCursor(id)) {
        auto pin = globalCursorManager->pinCursor(opCtx, id, CursorManager::kNoCheckSession);
        if (!pin.isOK()) {
            invariant(pin == ErrorCodes::CursorNotFound || pin == ErrorCodes::Unauthorized);
            // No such cursor.  TODO: Consider writing to audit log here (even though we don't
            // have a namespace).
            return false;
        }
        nss = pin.getValue().getCursor()->nss();
    } else {
        stdx::lock_guard<SimpleMutex> lk(_mutex);
        uint32_t nsid = idFromCursorId(id);
        IdToNssMap::const_iterator it = _idToNss.find(nsid);
        if (it == _idToNss.end()) {
            // No namespace corresponding to this cursor id prefix.  TODO: Consider writing to
            // audit log here (even though we don't have a namespace).
            return false;
        }
        nss = it->second;
    }
    invariant(nss.isValid());

    // Check if we are authorized to kill this cursor.
    if (checkAuth) {
        auto status = CursorManager::withCursorManager(
            opCtx, id, nss, [nss, id, opCtx](CursorManager* manager) {
                auto ccPin = manager->pinCursor(opCtx, id, CursorManager::kNoCheckSession);
                if (!ccPin.isOK()) {
                    return ccPin.getStatus();
                }
                AuthorizationSession* as = AuthorizationSession::get(opCtx->getClient());
                auto cursorOwner = ccPin.getValue().getCursor()->getAuthenticatedUsers();
                return as->checkAuthForKillCursors(nss, cursorOwner);
            });
        if (!status.isOK()) {
            audit::logKillCursorsAuthzCheck(opCtx->getClient(), nss, id, status.code());
            return false;
        }
    }

    // If this cursor is owned by the global cursor manager, ask it to kill the cursor for us.
    if (CursorManager::isGloballyManagedCursor(id)) {
        Status killStatus = globalCursorManager->killCursor(opCtx, id, checkAuth);
        massert(28697,
                killStatus.reason(),
                killStatus.code() == ErrorCodes::OK ||
                    killStatus.code() == ErrorCodes::CursorNotFound);
        return killStatus.isOK();
    }

    // If not, then the cursor must be owned by a collection. Kill the cursor under the
    // collection lock (to prevent the collection from going away during the erase).
    AutoGetCollectionForReadCommand ctx(opCtx, nss);
    Collection* collection = ctx.getCollection();
    if (!collection) {
        if (checkAuth)
            audit::logKillCursorsAuthzCheck(
                opCtx->getClient(), nss, id, ErrorCodes::CursorNotFound);
        return false;
    }

    Status eraseStatus = collection->getCursorManager()->killCursor(opCtx, id, checkAuth);
    uassert(16089,
            eraseStatus.reason(),
            eraseStatus.code() == ErrorCodes::OK ||
                eraseStatus.code() == ErrorCodes::CursorNotFound);
    return eraseStatus.isOK();
}

std::size_t GlobalCursorIdCache::timeoutCursors(OperationContext* opCtx, Date_t now) {
    size_t totalTimedOut = 0;

    // Time out the cursors from the global cursor manager.
    totalTimedOut += globalCursorManager->timeoutCursors(opCtx, now);

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
    for (const auto& nsTodo : todo) {
        // We need to be careful to not use an AutoGet* helper, since we only need the lock to
        // protect potential access to the Collection's CursorManager, and those helpers may
        // do things we don't want here, like check the shard version or throw an exception if this
        // namespace has since turned into a view. Using Database::getCollection() will simply
        // return nullptr if the collection has since turned into a view. In this case, the cursors
        // will already have been cleaned up when the collection was dropped, so there will be none
        // left to time out.
        //
        // Additionally, we need to use the UninterruptibleLockGuard to ensure the lock acquisition
        // will not throw due to an interrupt. This method can be called from a background thread so
        // we do not want to throw any exceptions.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        AutoGetDb dbLock(opCtx, nsTodo.db(), MODE_IS);
        Lock::CollectionLock collLock(opCtx->lockState(), nsTodo.ns(), MODE_IS);
        if (!dbLock.getDb()) {
            continue;
        }

        Collection* const collection = dbLock.getDb()->getCollection(opCtx, nsTodo);
        if (!collection) {
            // The 'nsTodo' collection has been dropped since we held _mutex. We can safely skip it.
            continue;
        }

        totalTimedOut += collection->getCursorManager()->timeoutCursors(opCtx, now);
    }

    return totalTimedOut;
}

}  // namespace

template <typename Visitor>
void GlobalCursorIdCache::visitAllCursorManagers(OperationContext* opCtx, Visitor* visitor) {
    (*visitor)(*globalCursorManager);

    // Compute the set of collection names that we have to get sessions for
    vector<NamespaceString> namespaces;
    {
        stdx::lock_guard<SimpleMutex> lk(_mutex);
        for (auto&& entry : _idToNss) {
            namespaces.push_back(entry.second);
        }
    }

    // For each collection, get its sessions under the collection lock (to prevent the
    // collection from going away during the erase).
    for (auto&& ns : namespaces) {
        AutoGetCollection ctx(opCtx, ns, MODE_IS);
        if (!ctx.getDb()) {
            continue;
        }

        Collection* collection = ctx.getCollection();
        if (!collection) {
            continue;
        }

        (*visitor)(*(collection->getCursorManager()));
    }
}

// ---

CursorManager* CursorManager::getGlobalCursorManager() {
    return globalCursorManager.get();
}

void CursorManager::appendAllActiveSessions(OperationContext* opCtx, LogicalSessionIdSet* lsids) {
    auto visitor = [&](CursorManager& mgr) { mgr.appendActiveSessions(lsids); };
    globalCursorIdCache->visitAllCursorManagers(opCtx, &visitor);
}

std::vector<GenericCursor> CursorManager::getAllCursors(OperationContext* opCtx) {
    std::vector<GenericCursor> cursors;
    auto visitor = [&](CursorManager& mgr) { mgr.appendActiveCursors(&cursors); };
    globalCursorIdCache->visitAllCursorManagers(opCtx, &visitor);

    return cursors;
}

std::pair<Status, int> CursorManager::killCursorsWithMatchingSessions(
    OperationContext* opCtx, const SessionKiller::Matcher& matcher) {
    auto eraser = [&](CursorManager& mgr, CursorId id) {
        uassertStatusOK(mgr.killCursor(opCtx, id, true));
    };

    auto visitor = makeKillSessionsCursorManagerVisitor(opCtx, matcher, std::move(eraser));
    globalCursorIdCache->visitAllCursorManagers(opCtx, &visitor);

    return std::make_pair(visitor.getStatus(), visitor.getCursorsKilled());
}

std::size_t CursorManager::timeoutCursorsGlobal(OperationContext* opCtx, Date_t now) {
    return globalCursorIdCache->timeoutCursors(opCtx, now);
}

int CursorManager::killCursorGlobalIfAuthorized(OperationContext* opCtx, int n, const char* _ids) {
    ConstDataCursor ids(_ids);
    int numDeleted = 0;
    for (int i = 0; i < n; i++) {
        if (killCursorGlobalIfAuthorized(opCtx, ids.readAndAdvance<LittleEndian<int64_t>>()))
            numDeleted++;
        if (globalInShutdownDeprecated())
            break;
    }
    return numDeleted;
}
bool CursorManager::killCursorGlobalIfAuthorized(OperationContext* opCtx, CursorId id) {
    return globalCursorIdCache->killCursor(opCtx, id, true);
}
bool CursorManager::killCursorGlobal(OperationContext* opCtx, CursorId id) {
    return globalCursorIdCache->killCursor(opCtx, id, false);
}

Status CursorManager::withCursorManager(OperationContext* opCtx,
                                        CursorId id,
                                        const NamespaceString& nss,
                                        stdx::function<Status(CursorManager*)> callback) {
    boost::optional<AutoGetCollectionForReadCommand> readLock;
    CursorManager* cursorManager = nullptr;

    if (CursorManager::isGloballyManagedCursor(id)) {
        cursorManager = CursorManager::getGlobalCursorManager();
    } else {
        readLock.emplace(opCtx, nss);
        Collection* collection = readLock->getCollection();
        if (!collection) {
            return {ErrorCodes::CursorNotFound,
                    str::stream() << "collection does not exist: " << nss.ns()};
        }
        cursorManager = collection->getCursorManager();
    }
    invariant(cursorManager);

    return callback(cursorManager);
}

// --------------------------

std::size_t CursorManager::PlanExecutorPartitioner::operator()(const PlanExecutor* exec,
                                                               const std::size_t nPartitions) {
    auto token = exec->getRegistrationToken();
    invariant(token);
    return (*token) % nPartitions;
}

CursorManager::CursorManager(NamespaceString nss)
    : _nss(std::move(nss)),
      _collectionCacheRuntimeId(_nss.isEmpty() ? 0
                                               : globalCursorIdCache->registerCursorManager(_nss)),
      _random(stdx::make_unique<PseudoRandom>(globalCursorIdCache->nextSeed())),
      _registeredPlanExecutors(),
      _cursorMap(stdx::make_unique<Partitioned<stdx::unordered_map<CursorId, ClientCursor*>>>()) {}

CursorManager::~CursorManager() {
    // All cursors and PlanExecutors should have been deleted already.
    invariant(_registeredPlanExecutors.empty());
    invariant(_cursorMap->empty());

    if (!isGlobalManager()) {
        globalCursorIdCache->deregisterCursorManager(_collectionCacheRuntimeId, _nss);
    }
}

void CursorManager::invalidateAll(OperationContext* opCtx,
                                  bool collectionGoingAway,
                                  const std::string& reason) {
    invariant(!isGlobalManager());  // The global cursor manager should never need to kill cursors.
    dassert(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_X));
    fassert(28819, !BackgroundOperation::inProgForNs(_nss));
    auto allExecPartitions = _registeredPlanExecutors.lockAllPartitions();
    for (auto&& partition : allExecPartitions) {
        for (auto&& exec : partition) {
            // The PlanExecutor is owned elsewhere, so we just mark it as killed and let it be
            // cleaned up later.
            exec->markAsKilled({ErrorCodes::QueryPlanKilled, reason});
        }
    }
    allExecPartitions.clear();

    // Mark all cursors as killed, but keep around those we can in order to provide a useful error
    // message to the user when they attempt to use it next time.
    std::vector<std::unique_ptr<ClientCursor, ClientCursor::Deleter>> toDisposeWithoutMutex;
    {
        auto allCurrentPartitions = _cursorMap->lockAllPartitions();
        for (auto&& partition : allCurrentPartitions) {
            for (auto it = partition.begin(); it != partition.end();) {
                auto* cursor = it->second;
                cursor->markAsKilled({ErrorCodes::QueryPlanKilled, reason});

                // If there's an operation actively using the cursor, then that operation is now
                // responsible for cleaning it up.  Otherwise we can immediately dispose of it.
                if (cursor->_operationUsingCursor) {
                    it = partition.erase(it);
                    continue;
                }

                if (!collectionGoingAway) {
                    // We keep around unpinned cursors so that future attempts to use the cursor
                    // will result in a useful error message.
                    ++it;
                } else {
                    toDisposeWithoutMutex.emplace_back(cursor);
                    it = partition.erase(it);
                }
            }
        }
    }

    // Dispose of the cursors we can now delete. This might involve lock acquisitions for safe
    // cleanup, so avoid doing it while holding mutexes.
    for (auto&& cursor : toDisposeWithoutMutex) {
        cursor->dispose(opCtx);
    }
}

void CursorManager::invalidateDocument(OperationContext* opCtx,
                                       const RecordId& dl,
                                       InvalidationType type) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(_nss.ns(), MODE_IX));
    invariant(!isGlobalManager());  // The global cursor manager should never receive invalidations.
    if (supportsDocLocking()) {
        // If a storage engine supports doc locking, then we do not need to invalidate.
        // The transactional boundaries of the operation protect us.
        return;
    }

    auto allExecPartitions = _registeredPlanExecutors.lockAllPartitions();
    for (auto&& partition : allExecPartitions) {
        for (auto&& exec : partition) {
            exec->invalidate(opCtx, dl, type);
        }
    }

    auto allPartitions = _cursorMap->lockAllPartitions();
    for (auto&& partition : allPartitions) {
        for (auto&& entry : partition) {
            auto exec = entry.second->getExecutor();
            exec->invalidate(opCtx, dl, type);
        }
    }
}

bool CursorManager::cursorShouldTimeout_inlock(const ClientCursor* cursor, Date_t now) {
    if (cursor->isNoTimeout() || cursor->_operationUsingCursor) {
        return false;
    }
    return (now - cursor->_lastUseDate) >= Milliseconds(getCursorTimeoutMillis());
}

std::size_t CursorManager::timeoutCursors(OperationContext* opCtx, Date_t now) {
    std::vector<std::unique_ptr<ClientCursor, ClientCursor::Deleter>> toDisposeWithoutMutex;

    for (size_t partitionId = 0; partitionId < kNumPartitions; ++partitionId) {
        auto lockedPartition = _cursorMap->lockOnePartitionById(partitionId);
        for (auto it = lockedPartition->begin(); it != lockedPartition->end();) {
            auto* cursor = it->second;
            if (cursorShouldTimeout_inlock(cursor, now)) {
                toDisposeWithoutMutex.emplace_back(cursor);
                it = lockedPartition->erase(it);
            } else {
                ++it;
            }
        }
    }

    // Be careful not to dispose of cursors while holding the partition lock.
    for (auto&& cursor : toDisposeWithoutMutex) {
        cursor->dispose(opCtx);
    }
    return toDisposeWithoutMutex.size();
}

namespace {
static AtomicUInt32 registeredPlanExecutorId;
}  // namespace

Partitioned<stdx::unordered_set<PlanExecutor*>>::PartitionId CursorManager::registerExecutor(
    PlanExecutor* exec) {
    auto partitionId = registeredPlanExecutorId.fetchAndAdd(1);
    exec->setRegistrationToken(partitionId);
    _registeredPlanExecutors.insert(exec);
    return partitionId;
}

void CursorManager::deregisterExecutor(PlanExecutor* exec) {
    if (auto partitionId = exec->getRegistrationToken()) {
        _registeredPlanExecutors.erase(exec);
    }
}

StatusWith<ClientCursorPin> CursorManager::pinCursor(OperationContext* opCtx,
                                                     CursorId id,
                                                     AuthCheck checkSessionAuth) {
    auto lockedPartition = _cursorMap->lockOnePartition(id);
    auto it = lockedPartition->find(id);
    if (it == lockedPartition->end()) {
        return {ErrorCodes::CursorNotFound, str::stream() << "cursor id " << id << " not found"};
    }

    ClientCursor* cursor = it->second;
    uassert(ErrorCodes::CursorInUse,
            str::stream() << "cursor id " << id << " is already in use",
            !cursor->_operationUsingCursor);
    if (cursor->getExecutor()->isMarkedAsKilled()) {
        // This cursor was killed while it was idle.
        Status error = cursor->getExecutor()->getKillStatus();
        deregisterAndDestroyCursor(std::move(lockedPartition),
                                   opCtx,
                                   std::unique_ptr<ClientCursor, ClientCursor::Deleter>(cursor));
        return error;
    }

    if (checkSessionAuth == kCheckSession) {
        auto cursorPrivilegeStatus = checkCursorSessionPrivilege(opCtx, cursor->getSessionId());
        if (!cursorPrivilegeStatus.isOK()) {
            return cursorPrivilegeStatus;
        }
    }

    cursor->_operationUsingCursor = opCtx;

    // We use pinning of a cursor as a proxy for active, user-initiated use of a cursor.  Therefor,
    // we pass down to the logical session cache and vivify the record (updating last use).
    if (cursor->getSessionId()) {
        LogicalSessionCache::get(opCtx)->vivify(opCtx, cursor->getSessionId().get());
    }

    return ClientCursorPin(opCtx, cursor);
}

void CursorManager::unpin(OperationContext* opCtx,
                          std::unique_ptr<ClientCursor, ClientCursor::Deleter> cursor) {
    // Avoid computing the current time within the critical section.
    auto now = opCtx->getServiceContext()->getPreciseClockSource()->now();

    auto partition = _cursorMap->lockOnePartition(cursor->cursorid());
    invariant(cursor->_operationUsingCursor);

    // We must verify that no interrupts have occurred since we finished building the current
    // batch. Otherwise, the cursor will be checked back in, the interrupted opCtx will be
    // destroyed, and subsequent getMores with a fresh opCtx will succeed.
    auto interruptStatus = cursor->_operationUsingCursor->checkForInterruptNoAssert();
    cursor->_operationUsingCursor = nullptr;
    cursor->_lastUseDate = now;

    // If someone was trying to kill this cursor with a killOp or a killCursors, they are likely
    // interesting in proactively cleaning up that cursor's resources. In these cases, we
    // proactively delete the cursor. In other cases we preserve the error code so that the client
    // will see the reason the cursor was killed when asking for the next batch.
    if (interruptStatus == ErrorCodes::Interrupted || interruptStatus == ErrorCodes::CursorKilled) {
        LOG(0) << "removing cursor " << cursor->cursorid()
               << " after completing batch: " << interruptStatus;
        return deregisterAndDestroyCursor(std::move(partition), opCtx, std::move(cursor));
    } else if (!interruptStatus.isOK()) {
        cursor->markAsKilled(interruptStatus);
    }

    // The cursor will stay around in '_cursorMap', so release the unique pointer to avoid deleting
    // it.
    cursor.release();
}

void CursorManager::appendActiveSessions(LogicalSessionIdSet* lsids) const {
    auto allPartitions = _cursorMap->lockAllPartitions();
    for (auto&& partition : allPartitions) {
        for (auto&& entry : partition) {
            auto cursor = entry.second;
            if (auto id = cursor->getSessionId()) {
                lsids->insert(id.value());
            }
        }
    }
}

void CursorManager::appendActiveCursors(std::vector<GenericCursor>* cursors) const {
    auto allPartitions = _cursorMap->lockAllPartitions();
    for (auto&& partition : allPartitions) {
        for (auto&& entry : partition) {
            auto cursor = entry.second;
            cursors->emplace_back();
            auto& gc = cursors->back();
            gc.setId(cursor->_cursorid);
            gc.setNs(cursor->nss());
            gc.setLsid(cursor->getSessionId());
        }
    }
}

stdx::unordered_set<CursorId> CursorManager::getCursorsForSession(LogicalSessionId lsid) const {
    stdx::unordered_set<CursorId> cursors;

    auto allPartitions = _cursorMap->lockAllPartitions();
    for (auto&& partition : allPartitions) {
        for (auto&& entry : partition) {
            auto cursor = entry.second;
            if (cursor->getSessionId() == lsid) {
                cursors.insert(cursor->cursorid());
            }
        }
    }

    return cursors;
}

size_t CursorManager::numCursors() const {
    return _cursorMap->size();
}

CursorId CursorManager::allocateCursorId_inlock() {
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
        auto partition = _cursorMap->lockOnePartition(id);
        if (partition->count(id) == 0)
            return id;
    }
    fassertFailed(17360);
}

ClientCursorPin CursorManager::registerCursor(OperationContext* opCtx,
                                              ClientCursorParams&& cursorParams) {
    // Avoid computing the current time within the critical section.
    auto now = opCtx->getServiceContext()->getPreciseClockSource()->now();

    // Make sure the PlanExecutor isn't registered, since we will register the ClientCursor wrapping
    // it.
    invariant(cursorParams.exec);
    deregisterExecutor(cursorParams.exec.get());
    cursorParams.exec.get_deleter().dismissDisposal();
    cursorParams.exec->unsetRegistered();

    // Note we must hold the registration lock from now until insertion into '_cursorMap' to ensure
    // we don't insert two cursors with the same cursor id.
    stdx::lock_guard<SimpleMutex> lock(_registrationLock);
    CursorId cursorId = allocateCursorId_inlock();
    std::unique_ptr<ClientCursor, ClientCursor::Deleter> clientCursor(
        new ClientCursor(std::move(cursorParams), this, cursorId, opCtx, now));

    // Register this cursor for lookup by transaction.
    if (opCtx->getLogicalSessionId() && opCtx->getTxnNumber()) {
        invariant(opCtx->getLogicalSessionId());
    }

    // Transfer ownership of the cursor to '_cursorMap'.
    auto partition = _cursorMap->lockOnePartition(cursorId);
    ClientCursor* unownedCursor = clientCursor.release();
    partition->emplace(cursorId, unownedCursor);
    return ClientCursorPin(opCtx, unownedCursor);
}

void CursorManager::deregisterCursor(ClientCursor* cursor) {
    _cursorMap->erase(cursor->cursorid());
}

void CursorManager::deregisterAndDestroyCursor(
    Partitioned<stdx::unordered_map<CursorId, ClientCursor*>, kNumPartitions>::OnePartition&& lk,
    OperationContext* opCtx,
    std::unique_ptr<ClientCursor, ClientCursor::Deleter> cursor) {
    {
        auto lockWithRestrictedScope = std::move(lk);
        lockWithRestrictedScope->erase(cursor->cursorid());
    }
    // Dispose of the cursor without holding any cursor manager mutexes. Disposal of a cursor can
    // require taking lock manager locks, which we want to avoid while holding a mutex. If we did
    // so, any caller of a CursorManager method which already held a lock manager lock could induce
    // a deadlock when trying to acquire a CursorManager lock.
    cursor->dispose(opCtx);
}

Status CursorManager::killCursor(OperationContext* opCtx, CursorId id, bool shouldAudit) {
    auto lockedPartition = _cursorMap->lockOnePartition(id);
    auto it = lockedPartition->find(id);
    if (it == lockedPartition->end()) {
        if (shouldAudit) {
            audit::logKillCursorsAuthzCheck(
                opCtx->getClient(), _nss, id, ErrorCodes::CursorNotFound);
        }
        return {ErrorCodes::CursorNotFound, str::stream() << "Cursor id not found: " << id};
    }
    auto cursor = it->second;

    if (cursor->_operationUsingCursor) {
        // Rather than removing the cursor directly, kill the operation that's currently using the
        // cursor. It will stop on its own (and remove the cursor) when it sees that it's been
        // interrupted.
        {
            stdx::unique_lock<Client> lk(*cursor->_operationUsingCursor->getClient());
            cursor->_operationUsingCursor->getServiceContext()->killOperation(
                cursor->_operationUsingCursor, ErrorCodes::CursorKilled);
        }

        if (shouldAudit) {
            audit::logKillCursorsAuthzCheck(opCtx->getClient(), _nss, id, ErrorCodes::OK);
        }
        return Status::OK();
    }
    std::unique_ptr<ClientCursor, ClientCursor::Deleter> ownedCursor(cursor);

    if (shouldAudit) {
        audit::logKillCursorsAuthzCheck(opCtx->getClient(), _nss, id, ErrorCodes::OK);
    }

    deregisterAndDestroyCursor(std::move(lockedPartition), opCtx, std::move(ownedCursor));
    return Status::OK();
}

Status CursorManager::checkAuthForKillCursors(OperationContext* opCtx, CursorId id) {
    auto lockedPartition = _cursorMap->lockOnePartition(id);
    auto it = lockedPartition->find(id);
    if (it == lockedPartition->end()) {
        return {ErrorCodes::CursorNotFound, str::stream() << "cursor id " << id << " not found"};
    }

    ClientCursor* cursor = it->second;
    // Note that we're accessing the cursor without having pinned it! This is okay since we're only
    // accessing nss() and getAuthenticatedUsers() both of which return values that don't change
    // after the cursor's creation. We're guaranteed that the cursor won't get destroyed while we're
    // reading from it because we hold the partition's lock.
    AuthorizationSession* as = AuthorizationSession::get(opCtx->getClient());
    return as->checkAuthForKillCursors(cursor->nss(), cursor->getAuthenticatedUsers());
}

}  // namespace mongo
