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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "collection_catalog.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {
struct LatestCollectionCatalog {
    std::shared_ptr<CollectionCatalog> catalog = std::make_shared<CollectionCatalog>();
};
const ServiceContext::Decoration<LatestCollectionCatalog> getCatalog =
    ServiceContext::declareDecoration<LatestCollectionCatalog>();

/**
 * Decoration on OperationContext to store cloned Collections until they are committed or rolled
 * back TODO SERVER-51236: This should be merged with UncommittedCollections
 */
class UncommittedWritableCollections {
public:
    using CommitFn = std::function<void(CollectionCatalog&, boost::optional<Timestamp>)>;

    /**
     * Lookup of Collection by UUID
     */
    Collection* lookup(CollectionUUID uuid) const {
        auto it = std::find_if(_collections.begin(), _collections.end(), [uuid](auto&& entry) {
            if (!entry.collection)
                return false;
            return entry.collection->uuid() == uuid;
        });
        if (it == _collections.end())
            return nullptr;
        return it->collection.get();
    }

    /**
     * Lookup of Collection by NamespaceString. The boolean indicates if something was found, if
     * this is a drop the Collection pointer may be nullptr
     */
    std::pair<bool, Collection*> lookup(const NamespaceString& nss) const {
        auto it = std::find_if(_collections.begin(), _collections.end(), [&nss](auto&& entry) {
            return entry.nss == nss;
        });
        if (it == _collections.end())
            return {false, nullptr};
        return {true, it->collection.get()};
    }

    /**
     * Manage the lifetime of uncommitted writable collection
     */
    void insert(std::shared_ptr<Collection> collection) {
        auto nss = collection->ns();
        _collections.push_back(Entry{std::move(collection), std::move(nss)});
    }

    /**
     * Manage an uncommitted rename, the provided commit handler will execute under the same
     * critical section under which the rename is committed into the catalog
     */
    void rename(const Collection* collection, const NamespaceString& from, CommitFn commitHandler) {
        auto it =
            std::find_if(_collections.begin(), _collections.end(), [collection](auto&& entry) {
                return entry.collection.get() == collection;
            });
        if (it == _collections.end())
            return;
        it->nss = collection->ns();
        it->commitHandlers.push_back(std::move(commitHandler));
        _collections.push_back(Entry{nullptr, from});
    }

    /**
     * Remove a managed collection. Return the shared_ptr and all installed commit handlers
     */
    std::pair<std::shared_ptr<Collection>, std::vector<CommitFn>> remove(Collection* collection) {
        auto it =
            std::find_if(_collections.begin(), _collections.end(), [collection](auto&& entry) {
                return entry.collection.get() == collection;
            });
        if (it == _collections.end())
            return {nullptr, {}};
        auto coll = std::move(it->collection);
        auto commitHandlers = std::move(it->commitHandlers);
        _collections.erase(it);
        return {std::move(coll), std::move(commitHandlers)};
    }

    /**
     * Remove managed collection by namespace
     */
    void remove(const NamespaceString& nss) {
        auto it = std::find_if(_collections.begin(), _collections.end(), [&nss](auto&& entry) {
            return entry.nss == nss;
        });
        if (it != _collections.end())
            _collections.erase(it);
    }

private:
    struct Entry {
        // Storage for the actual collection
        std::shared_ptr<Collection> collection;

        // Store namespace separately in case this is a pending drop of the collection
        NamespaceString nss;

        // Extra commit handlers to run under the same catalog lock where we install the collection
        // into the catalog
        std::vector<CommitFn> commitHandlers;
    };

    // Store entries in vector, we will do linear search to find what we're looking for but it will
    // be very few entries so it should be fine.
    std::vector<Entry> _collections;
};

const OperationContext::Decoration<UncommittedWritableCollections>
    getUncommittedWritableCollections =
        OperationContext::declareDecoration<UncommittedWritableCollections>();

const OperationContext::Decoration<std::shared_ptr<const CollectionCatalog>> stashedCatalog =
    OperationContext::declareDecoration<std::shared_ptr<const CollectionCatalog>>();

class FinishDropCollectionChange : public RecoveryUnit::Change {
public:
    FinishDropCollectionChange(OperationContext* opCtx,
                               std::shared_ptr<Collection> coll,
                               CollectionUUID uuid)
        : _opCtx(opCtx), _coll(std::move(coll)), _uuid(uuid) {}

    void commit(boost::optional<Timestamp>) override {
        _coll.reset();
    }

    void rollback() override {
        CollectionCatalog::write(_opCtx, [&](CollectionCatalog& catalog) {
            catalog.registerCollection(_uuid, std::move(_coll));
        });
    }

private:
    OperationContext* _opCtx;
    std::shared_ptr<Collection> _coll;
    CollectionUUID _uuid;
};

}  // namespace

CollectionCatalog::iterator::iterator(OperationContext* opCtx,
                                      StringData dbName,
                                      const CollectionCatalog& catalog)
    : _opCtx(opCtx), _dbName(dbName), _catalog(&catalog) {
    auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();

    _mapIter = _catalog->_orderedCollections.lower_bound(std::make_pair(_dbName, minUuid));

    // Start with the first collection that is visible outside of its transaction.
    while (!_exhausted() && !_mapIter->second->isCommitted()) {
        _mapIter++;
    }

    if (!_exhausted()) {
        _uuid = _mapIter->first.second;
    }
}

CollectionCatalog::iterator::iterator(OperationContext* opCtx,
                                      std::map<std::pair<std::string, CollectionUUID>,
                                               std::shared_ptr<Collection>>::const_iterator mapIter,
                                      const CollectionCatalog& catalog)
    : _opCtx(opCtx), _mapIter(mapIter), _catalog(&catalog) {}

CollectionCatalog::iterator::value_type CollectionCatalog::iterator::operator*() {
    if (_exhausted()) {
        return CollectionPtr();
    }

    return {_opCtx, _mapIter->second.get(), LookupCollectionForYieldRestore()};
}

Collection* CollectionCatalog::iterator::getWritableCollection(OperationContext* opCtx,
                                                               LifetimeMode mode) {
    return CollectionCatalog::get(opCtx)->lookupCollectionByUUIDForMetadataWrite(
        opCtx, mode, operator*()->uuid());
}

boost::optional<CollectionUUID> CollectionCatalog::iterator::uuid() {
    return _uuid;
}

CollectionCatalog::iterator CollectionCatalog::iterator::operator++() {
    _mapIter++;

    // Skip any collections that are not yet visible outside of their respective transactions.
    while (!_exhausted() && !_mapIter->second->isCommitted()) {
        _mapIter++;
    }

    if (_exhausted()) {
        // If the iterator is at the end of the map or now points to an entry that does not
        // correspond to the correct database.
        _mapIter = _catalog->_orderedCollections.end();
        _uuid = boost::none;
        return *this;
    }

    _uuid = _mapIter->first.second;
    return *this;
}

CollectionCatalog::iterator CollectionCatalog::iterator::operator++(int) {
    auto oldPosition = *this;
    ++(*this);
    return oldPosition;
}

bool CollectionCatalog::iterator::operator==(const iterator& other) {
    invariant(_catalog == other._catalog);
    if (other._mapIter == _catalog->_orderedCollections.end()) {
        return _uuid == boost::none;
    }

    return _uuid == other._uuid;
}

bool CollectionCatalog::iterator::operator!=(const iterator& other) {
    return !(*this == other);
}

bool CollectionCatalog::iterator::_exhausted() {
    return _mapIter == _catalog->_orderedCollections.end() || _mapIter->first.first != _dbName;
}

std::shared_ptr<const CollectionCatalog> CollectionCatalog::get(ServiceContext* svcCtx) {
    return atomic_load(&getCatalog(svcCtx).catalog);
}

std::shared_ptr<const CollectionCatalog> CollectionCatalog::get(OperationContext* opCtx) {
    const auto& stashed = stashedCatalog(opCtx);
    if (stashed)
        return stashed;
    return get(opCtx->getServiceContext());
}

void CollectionCatalog::stash(OperationContext* opCtx,
                              std::shared_ptr<const CollectionCatalog> catalog) {
    stashedCatalog(opCtx) = std::move(catalog);
}

void CollectionCatalog::write(ServiceContext* svcCtx, CatalogWriteFn job) {
    // It is potentially expensive to copy the collection catalog so we batch the operations by only
    // having one concurrent thread copying the catalog and executing all the write jobs.

    struct JobEntry {
        JobEntry(CatalogWriteFn write) : job(std::move(write)) {}

        CatalogWriteFn job;

        struct CompletionInfo {
            // Used to wait for job to complete by worker thread
            Mutex mutex;
            stdx::condition_variable cv;

            // Exception storage if we threw during job execution, so we can transfer the exception
            // back to the calling thread
            std::exception_ptr exception;

            // The job is completed when the catalog we modified has been committed back to the
            // storage or if we threw during its execution
            bool completed = false;
        };

        // Shared state for completion info as JobEntry's gets deleted when we are finished
        // executing. No shared state means that this job belongs to the same thread executing them.
        std::shared_ptr<CompletionInfo> completion;
    };

    static std::list<JobEntry> queue;
    static bool workerExists = false;
    static Mutex mutex =
        MONGO_MAKE_LATCH("CollectionCatalog::write");  // Protecting the two globals above

    invariant(job);

    // Current batch of jobs to execute
    std::list<JobEntry> pending;
    {
        stdx::unique_lock lock(mutex);
        queue.emplace_back(std::move(job));

        // If worker already exists, then wait on our condition variable until the job is completed
        if (workerExists) {
            auto completion = std::make_shared<JobEntry::CompletionInfo>();
            queue.back().completion = completion;
            lock.unlock();

            stdx::unique_lock completionLock(completion->mutex);
            const bool& completed = completion->completed;
            completion->cv.wait(completionLock, [&completed]() { return completed; });

            // Throw any exception that was caught during execution of our job
            if (completion->exception)
                std::rethrow_exception(completion->exception);
            return;
        }

        // No worker existed, then we take this responsibility
        workerExists = true;
        pending.splice(pending.end(), queue);
    }

    // Implementation for thread with worker responsibility below, only one thread at a time can be
    // in here. Keep track of completed jobs so we can notify them when we've written back the
    // catalog to storage
    std::list<JobEntry> completed;
    std::exception_ptr myException;

    auto& storage = getCatalog(svcCtx);
    // hold onto base so if we need to delete it we can do it outside of the lock
    auto base = atomic_load(&storage.catalog);
    // copy the collection catalog, this could be expensive, but we will only have one pending
    // collection in flight at a given time
    auto clone = std::make_shared<CollectionCatalog>(*base);

    // Execute jobs until we drain the queue
    while (true) {
        for (auto&& current : pending) {
            // Store any exception thrown during job execution so we can notify the calling thread
            try {
                current.job(*clone);
            } catch (...) {
                if (current.completion)
                    current.completion->exception = std::current_exception();
                else
                    myException = std::current_exception();
            }
        }
        // Transfer the jobs we just executed to the completed list
        completed.splice(completed.end(), pending);

        stdx::lock_guard lock(mutex);
        if (queue.empty()) {
            // Queue is empty, store catalog and relinquish responsibility of being worker thread
            atomic_store(&storage.catalog, std::move(clone));
            workerExists = false;
            break;
        }

        // Transfer jobs in queue to the pending list
        pending.splice(pending.end(), queue);
    }

    for (auto&& entry : completed) {
        if (!entry.completion) {
            continue;
        }

        stdx::lock_guard completionLock(entry.completion->mutex);
        entry.completion->completed = true;
        entry.completion->cv.notify_one();
    }
    LOGV2_DEBUG(
        5255601, 1, "Finished writing to the CollectionCatalog", "jobs"_attr = completed.size());
    if (myException)
        std::rethrow_exception(myException);
}

void CollectionCatalog::write(OperationContext* opCtx,
                              std::function<void(CollectionCatalog&)> job) {
    write(opCtx->getServiceContext(), std::move(job));
}


void CollectionCatalog::setCollectionNamespace(OperationContext* opCtx,
                                               Collection* coll,
                                               const NamespaceString& fromCollection,
                                               const NamespaceString& toCollection) const {
    // Rather than maintain, in addition to the UUID -> Collection* mapping, an auxiliary
    // data structure with the UUID -> namespace mapping, the CollectionCatalog relies on
    // Collection::ns() to provide UUID to namespace lookup. In addition, the CollectionCatalog
    // does not require callers to hold locks.
    invariant(coll);
    coll->setNs(toCollection);

    auto& uncommittedWritableCollections = getUncommittedWritableCollections(opCtx);
    uncommittedWritableCollections.rename(
        coll,
        fromCollection,
        [opCtx, coll, fromCollection, toCollection](CollectionCatalog& writableCatalog,
                                                    boost::optional<Timestamp> commitTime) {
            writableCatalog._collections.erase(fromCollection);

            ResourceId oldRid = ResourceId(RESOURCE_COLLECTION, fromCollection.ns());
            ResourceId newRid = ResourceId(RESOURCE_COLLECTION, toCollection.ns());

            writableCatalog.removeResource(oldRid, fromCollection.ns());
            writableCatalog.addResource(newRid, toCollection.ns());

            // Ban reading from this collection on committed reads on snapshots before now.
            if (commitTime) {
                coll->setMinimumVisibleSnapshot(commitTime.get());
            }
        });

    class RenameChange : public RecoveryUnit::Change {
    public:
        RenameChange(UncommittedWritableCollections& uncommittedWritableCollections,
                     const NamespaceString& from)
            : _uncommittedWritableCollections(uncommittedWritableCollections), _from(from) {}

        void commit(boost::optional<Timestamp> timestamp) override {
            _uncommittedWritableCollections.remove(_from);
        }
        void rollback() override {
            _uncommittedWritableCollections.remove(_from);
        }

    private:
        UncommittedWritableCollections& _uncommittedWritableCollections;
        NamespaceString _from;
    };

    opCtx->recoveryUnit()->registerChange(
        std::make_unique<RenameChange>(uncommittedWritableCollections, fromCollection));
}

void CollectionCatalog::onCloseDatabase(OperationContext* opCtx, std::string dbName) {
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_X));
    auto rid = ResourceId(RESOURCE_DATABASE, dbName);
    removeResource(rid, dbName);
}

void CollectionCatalog::onCloseCatalog(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());
    invariant(!_shadowCatalog);
    _shadowCatalog.emplace();
    for (auto& entry : _catalog)
        _shadowCatalog->insert({entry.first, entry.second->ns()});
}

void CollectionCatalog::onOpenCatalog(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());
    invariant(_shadowCatalog);
    _shadowCatalog.reset();
    ++_epoch;
}

uint64_t CollectionCatalog::getEpoch() const {
    return _epoch;
}

std::shared_ptr<const Collection> CollectionCatalog::lookupCollectionByUUIDForRead(
    OperationContext* opCtx, CollectionUUID uuid) const {
    if (auto coll = UncommittedCollections::getForTxn(opCtx, uuid)) {
        return coll;
    }

    auto coll = _lookupCollectionByUUID(uuid);
    return (coll && coll->isCommitted()) ? coll : nullptr;
}

Collection* CollectionCatalog::lookupCollectionByUUIDForMetadataWrite(OperationContext* opCtx,
                                                                      LifetimeMode mode,
                                                                      CollectionUUID uuid) const {
    if (mode == LifetimeMode::kInplace) {
        return const_cast<Collection*>(lookupCollectionByUUID(opCtx, uuid).get());
    }

    auto& uncommittedWritableCollections = getUncommittedWritableCollections(opCtx);
    if (auto coll = uncommittedWritableCollections.lookup(uuid)) {
        return coll;
    }

    if (auto coll = UncommittedCollections::getForTxn(opCtx, uuid)) {
        invariant(opCtx->lockState()->isCollectionLockedForMode(coll->ns(), MODE_IX));
        return coll.get();
    }

    std::shared_ptr<Collection> coll = _lookupCollectionByUUID(uuid);

    if (!coll || !coll->isCommitted())
        return nullptr;

    if (coll->ns().isOplog())
        return coll.get();

    invariant(opCtx->lockState()->isCollectionLockedForMode(coll->ns(), MODE_X));
    auto cloned = coll->clone();
    uncommittedWritableCollections.insert(cloned);

    if (mode == LifetimeMode::kManagedInWriteUnitOfWork) {
        opCtx->recoveryUnit()->onCommit(
            [opCtx, &uncommittedWritableCollections, clonedPtr = cloned.get()](
                boost::optional<Timestamp> commitTime) {
                auto [collection, commitHandlers] =
                    uncommittedWritableCollections.remove(clonedPtr);
                if (collection) {
                    CollectionCatalog::write(
                        opCtx,
                        [collection = std::move(collection),
                         &commitTime,
                         commitHandlers = &commitHandlers](CollectionCatalog& catalog) {
                            catalog._commitWritableClone(
                                std::move(collection), commitTime, *commitHandlers);
                        });
                }
            });
        opCtx->recoveryUnit()->onRollback([&uncommittedWritableCollections, cloned]() {
            uncommittedWritableCollections.remove(cloned.get());
        });
    }

    return cloned.get();
}

CollectionPtr CollectionCatalog::lookupCollectionByUUID(OperationContext* opCtx,
                                                        CollectionUUID uuid) const {
    auto& uncommittedWritableCollections = getUncommittedWritableCollections(opCtx);
    if (auto coll = uncommittedWritableCollections.lookup(uuid)) {
        return coll;
    }

    if (auto coll = UncommittedCollections::getForTxn(opCtx, uuid)) {
        return {opCtx, coll.get(), LookupCollectionForYieldRestore()};
    }

    auto coll = _lookupCollectionByUUID(uuid);
    return (coll && coll->isCommitted())
        ? CollectionPtr(opCtx, coll.get(), LookupCollectionForYieldRestore())
        : CollectionPtr();
}

bool CollectionCatalog::isCollectionAwaitingVisibility(CollectionUUID uuid) const {
    auto coll = _lookupCollectionByUUID(uuid);
    return coll && !coll->isCommitted();
}

std::shared_ptr<Collection> CollectionCatalog::_lookupCollectionByUUID(CollectionUUID uuid) const {
    auto foundIt = _catalog.find(uuid);
    return foundIt == _catalog.end() ? nullptr : foundIt->second;
}

std::shared_ptr<const Collection> CollectionCatalog::lookupCollectionByNamespaceForRead(
    OperationContext* opCtx, const NamespaceString& nss) const {
    if (auto coll = UncommittedCollections::getForTxn(opCtx, nss)) {
        return coll;
    }

    auto it = _collections.find(nss);
    auto coll = (it == _collections.end() ? nullptr : it->second);
    return (coll && coll->isCommitted()) ? coll : nullptr;
}

Collection* CollectionCatalog::lookupCollectionByNamespaceForMetadataWrite(
    OperationContext* opCtx, LifetimeMode mode, const NamespaceString& nss) const {
    if (mode == LifetimeMode::kInplace || nss.isOplog()) {
        return const_cast<Collection*>(lookupCollectionByNamespace(opCtx, nss).get());
    }

    auto& uncommittedWritableCollections = getUncommittedWritableCollections(opCtx);
    auto [found, uncommittedPtr] = uncommittedWritableCollections.lookup(nss);
    if (found) {
        return uncommittedPtr;
    }

    if (auto coll = UncommittedCollections::getForTxn(opCtx, nss)) {
        invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));
        return coll.get();
    }

    auto it = _collections.find(nss);
    auto coll = (it == _collections.end() ? nullptr : it->second);

    if (!coll || !coll->isCommitted())
        return nullptr;

    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));
    auto cloned = coll->clone();
    uncommittedWritableCollections.insert(cloned);

    if (mode == LifetimeMode::kManagedInWriteUnitOfWork) {
        opCtx->recoveryUnit()->onCommit(
            [opCtx, &uncommittedWritableCollections, clonedPtr = cloned.get()](
                boost::optional<Timestamp> commitTime) {
                auto [collection, commitHandlers] =
                    uncommittedWritableCollections.remove(clonedPtr);
                if (collection) {
                    CollectionCatalog::write(
                        opCtx,
                        [collection = std::move(collection),
                         &commitTime,
                         commitHandlers = &commitHandlers](CollectionCatalog& catalog) {
                            catalog._commitWritableClone(
                                std::move(collection), commitTime, *commitHandlers);
                        });
                }
            });
        opCtx->recoveryUnit()->onRollback([&uncommittedWritableCollections, cloned]() {
            uncommittedWritableCollections.remove(cloned.get());
        });
    }

    return cloned.get();
}

CollectionPtr CollectionCatalog::lookupCollectionByNamespace(OperationContext* opCtx,
                                                             const NamespaceString& nss) const {
    auto& uncommittedWritableCollections = getUncommittedWritableCollections(opCtx);
    auto [found, uncommittedPtr] = uncommittedWritableCollections.lookup(nss);
    if (found) {
        return uncommittedPtr;
    }

    if (auto coll = UncommittedCollections::getForTxn(opCtx, nss)) {
        return {opCtx, coll.get(), LookupCollectionForYieldRestore()};
    }

    auto it = _collections.find(nss);
    auto coll = (it == _collections.end() ? nullptr : it->second);
    return (coll && coll->isCommitted())
        ? CollectionPtr(opCtx, coll.get(), LookupCollectionForYieldRestore())
        : nullptr;
}

boost::optional<NamespaceString> CollectionCatalog::lookupNSSByUUID(OperationContext* opCtx,
                                                                    CollectionUUID uuid) const {
    auto& uncommittedWritableCollections = getUncommittedWritableCollections(opCtx);
    if (auto coll = uncommittedWritableCollections.lookup(uuid)) {
        return coll->ns();
    }
    if (auto coll = UncommittedCollections::getForTxn(opCtx, uuid)) {
        return coll->ns();
    }

    auto foundIt = _catalog.find(uuid);
    if (foundIt != _catalog.end()) {
        boost::optional<NamespaceString> ns = foundIt->second->ns();
        invariant(!ns.get().isEmpty());
        return _collections.find(ns.get())->second->isCommitted() ? ns : boost::none;
    }

    // Only in the case that the catalog is closed and a UUID is currently unknown, resolve it
    // using the pre-close state. This ensures that any tasks reloading the catalog can see their
    // own updates.
    if (_shadowCatalog) {
        auto shadowIt = _shadowCatalog->find(uuid);
        if (shadowIt != _shadowCatalog->end())
            return shadowIt->second;
    }
    return boost::none;
}

boost::optional<CollectionUUID> CollectionCatalog::lookupUUIDByNSS(
    OperationContext* opCtx, const NamespaceString& nss) const {
    auto& uncommittedWritableCollections = getUncommittedWritableCollections(opCtx);
    auto [found, uncommittedPtr] = uncommittedWritableCollections.lookup(nss);
    if (found) {
        invariant(uncommittedPtr);
        return uncommittedPtr->uuid();
    }

    if (auto coll = UncommittedCollections::getForTxn(opCtx, nss)) {
        return coll->uuid();
    }

    auto it = _collections.find(nss);
    if (it != _collections.end()) {
        boost::optional<CollectionUUID> uuid = it->second->uuid();
        return it->second->isCommitted() ? uuid : boost::none;
    }
    return boost::none;
}

NamespaceString CollectionCatalog::resolveNamespaceStringOrUUID(
    OperationContext* opCtx, NamespaceStringOrUUID nsOrUUID) const {
    if (auto& nss = nsOrUUID.nss()) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace " << *nss << " is not a valid collection name",
                nss->isValid());
        return std::move(*nss);
    }

    auto resolvedNss = lookupNSSByUUID(opCtx, *nsOrUUID.uuid());

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Unable to resolve " << nsOrUUID.toString(),
            resolvedNss && resolvedNss->isValid());

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "UUID " << nsOrUUID.toString() << " specified in " << nsOrUUID.dbname()
                          << " resolved to a collection in a different database: " << *resolvedNss,
            resolvedNss->db() == nsOrUUID.dbname());

    return std::move(*resolvedNss);
}

bool CollectionCatalog::checkIfCollectionSatisfiable(CollectionUUID uuid,
                                                     CollectionInfoFn predicate) const {
    invariant(predicate);

    auto collection = _lookupCollectionByUUID(uuid);

    if (!collection) {
        return false;
    }

    return predicate(collection.get());
}

std::vector<CollectionUUID> CollectionCatalog::getAllCollectionUUIDsFromDb(
    StringData dbName) const {
    auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto it = _orderedCollections.lower_bound(std::make_pair(dbName.toString(), minUuid));

    std::vector<CollectionUUID> ret;
    while (it != _orderedCollections.end() && it->first.first == dbName) {
        if (it->second->isCommitted()) {
            ret.push_back(it->first.second);
        }
        ++it;
    }
    return ret;
}

std::vector<NamespaceString> CollectionCatalog::getAllCollectionNamesFromDb(
    OperationContext* opCtx, StringData dbName) const {
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_S));

    auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();

    std::vector<NamespaceString> ret;
    for (auto it = _orderedCollections.lower_bound(std::make_pair(dbName.toString(), minUuid));
         it != _orderedCollections.end() && it->first.first == dbName;
         ++it) {
        if (it->second->isCommitted()) {
            ret.push_back(it->second->ns());
        }
    }
    return ret;
}

std::vector<std::string> CollectionCatalog::getAllDbNames() const {
    std::vector<std::string> ret;
    auto maxUuid = UUID::parse("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF").getValue();
    auto iter = _orderedCollections.upper_bound(std::make_pair("", maxUuid));
    while (iter != _orderedCollections.end()) {
        auto dbName = iter->first.first;
        if (iter->second->isCommitted()) {
            ret.push_back(dbName);
        } else {
            // If the first collection found for `dbName` is not yet committed, increment the
            // iterator to find the next visible collection (possibly under a different `dbName`).
            iter++;
            continue;
        }
        // Move on to the next database after `dbName`.
        iter = _orderedCollections.upper_bound(std::make_pair(dbName, maxUuid));
    }
    return ret;
}

void CollectionCatalog::setDatabaseProfileSettings(
    StringData dbName, CollectionCatalog::ProfileSettings newProfileSettings) {
    _databaseProfileSettings[dbName] = newProfileSettings;
}

CollectionCatalog::ProfileSettings CollectionCatalog::getDatabaseProfileSettings(
    StringData dbName) const {
    auto it = _databaseProfileSettings.find(dbName);
    if (it != _databaseProfileSettings.end()) {
        return it->second;
    }

    return {serverGlobalParams.defaultProfile, ProfileFilter::getDefault()};
}

void CollectionCatalog::clearDatabaseProfileSettings(StringData dbName) {
    _databaseProfileSettings.erase(dbName);
}

void CollectionCatalog::registerCollection(CollectionUUID uuid, std::shared_ptr<Collection> coll) {
    auto ns = coll->ns();
    if (_collections.find(ns) != _collections.end()) {
        LOGV2(20279,
              "Conflicted creating a collection. ns: {coll_ns} ({coll_uuid}).",
              "Conflicted creating a collection",
              logAttrs(*coll));
        throw WriteConflictException();
    }

    LOGV2_DEBUG(20280,
                1,
                "Registering collection {ns} with UUID {uuid}",
                "Registering collection",
                "namespace"_attr = ns,
                "uuid"_attr = uuid);

    auto dbName = ns.db().toString();
    auto dbIdPair = std::make_pair(dbName, uuid);

    // Make sure no entry related to this uuid.
    invariant(_catalog.find(uuid) == _catalog.end());
    invariant(_orderedCollections.find(dbIdPair) == _orderedCollections.end());

    _catalog[uuid] = coll;
    _collections[ns] = coll;
    _orderedCollections[dbIdPair] = coll;

    auto dbRid = ResourceId(RESOURCE_DATABASE, dbName);
    addResource(dbRid, dbName);

    auto collRid = ResourceId(RESOURCE_COLLECTION, ns.ns());
    addResource(collRid, ns.ns());
}

std::shared_ptr<Collection> CollectionCatalog::deregisterCollection(OperationContext* opCtx,
                                                                    CollectionUUID uuid) {
    invariant(_catalog.find(uuid) != _catalog.end());

    auto coll = std::move(_catalog[uuid]);
    auto ns = coll->ns();
    auto dbName = ns.db().toString();
    auto dbIdPair = std::make_pair(dbName, uuid);

    LOGV2_DEBUG(20281, 1, "Deregistering collection", "namespace"_attr = ns, "uuid"_attr = uuid);

    // Make sure collection object exists.
    invariant(_collections.find(ns) != _collections.end());
    invariant(_orderedCollections.find(dbIdPair) != _orderedCollections.end());

    _orderedCollections.erase(dbIdPair);
    _collections.erase(ns);
    _catalog.erase(uuid);
    auto& uncommittedWritableCollections = getUncommittedWritableCollections(opCtx);
    if (auto writableColl = uncommittedWritableCollections.lookup(uuid)) {
        uncommittedWritableCollections.remove(writableColl);
    }

    coll->onDeregisterFromCatalog();

    auto collRid = ResourceId(RESOURCE_COLLECTION, ns.ns());
    removeResource(collRid, ns.ns());

    return coll;
}

std::unique_ptr<RecoveryUnit::Change> CollectionCatalog::makeFinishDropCollectionChange(
    OperationContext* opCtx, std::shared_ptr<Collection> coll, CollectionUUID uuid) {
    return std::make_unique<FinishDropCollectionChange>(opCtx, std::move(coll), uuid);
}

void CollectionCatalog::deregisterAllCollections() {
    LOGV2(20282, "Deregistering all the collections");
    for (auto& entry : _catalog) {
        auto uuid = entry.first;
        auto ns = entry.second->ns();
        auto dbName = ns.db().toString();
        auto dbIdPair = std::make_pair(dbName, uuid);

        LOGV2_DEBUG(
            20283, 1, "Deregistering collection", "namespace"_attr = ns, "uuid"_attr = uuid);

        entry.second.reset();
    }

    _collections.clear();
    _orderedCollections.clear();
    _catalog.clear();

    _resourceInformation.clear();
}

CollectionCatalog::iterator CollectionCatalog::begin(OperationContext* opCtx, StringData db) const {
    return iterator(opCtx, db, *this);
}

CollectionCatalog::iterator CollectionCatalog::end(OperationContext* opCtx) const {
    return iterator(opCtx, _orderedCollections.end(), *this);
}

boost::optional<std::string> CollectionCatalog::lookupResourceName(const ResourceId& rid) const {
    invariant(rid.getType() == RESOURCE_DATABASE || rid.getType() == RESOURCE_COLLECTION);

    auto search = _resourceInformation.find(rid);
    if (search == _resourceInformation.end()) {
        return boost::none;
    }

    const std::set<std::string>& namespaces = search->second;

    // When there are multiple namespaces mapped to the same ResourceId, return boost::none as the
    // ResourceId does not identify a single namespace.
    if (namespaces.size() > 1) {
        return boost::none;
    }

    return *namespaces.begin();
}

void CollectionCatalog::removeResource(const ResourceId& rid, const std::string& entry) {
    invariant(rid.getType() == RESOURCE_DATABASE || rid.getType() == RESOURCE_COLLECTION);

    auto search = _resourceInformation.find(rid);
    if (search == _resourceInformation.end()) {
        return;
    }

    std::set<std::string>& namespaces = search->second;
    namespaces.erase(entry);

    // Remove the map entry if this is the last namespace in the set for the ResourceId.
    if (namespaces.size() == 0) {
        _resourceInformation.erase(search);
    }
}

void CollectionCatalog::addResource(const ResourceId& rid, const std::string& entry) {
    invariant(rid.getType() == RESOURCE_DATABASE || rid.getType() == RESOURCE_COLLECTION);

    auto search = _resourceInformation.find(rid);
    if (search == _resourceInformation.end()) {
        std::set<std::string> newSet = {entry};
        _resourceInformation.insert(std::make_pair(rid, newSet));
        return;
    }

    std::set<std::string>& namespaces = search->second;
    if (namespaces.count(entry) > 0) {
        return;
    }

    namespaces.insert(entry);
}

void CollectionCatalog::_commitWritableClone(
    std::shared_ptr<Collection> cloned,
    boost::optional<Timestamp> commitTime,
    const std::vector<std::function<void(CollectionCatalog&, boost::optional<Timestamp>)>>&
        commitHandlers) {

    _collections[cloned->ns()] = cloned;
    _catalog[cloned->uuid()] = cloned;
    auto dbIdPair = std::make_pair(cloned->ns().db().toString(), cloned->uuid());
    _orderedCollections[dbIdPair] = cloned;

    for (auto&& commitHandler : commitHandlers) {
        commitHandler(*this, commitTime);
    }
}

void CollectionCatalog::commitUnmanagedClone(OperationContext* opCtx, Collection* collection) {
    auto& uncommittedWritableCollections = getUncommittedWritableCollections(opCtx);
    auto [cloned, commitHandlers] = uncommittedWritableCollections.remove(collection);
    if (cloned) {
        CollectionCatalog::write(opCtx,
                                 [cloned = std::move(cloned),
                                  commitHandlers = &commitHandlers](CollectionCatalog& catalog) {
                                     catalog._commitWritableClone(
                                         std::move(cloned), boost::none, *commitHandlers);
                                 });
    }
}

void CollectionCatalog::discardUnmanagedClone(OperationContext* opCtx, Collection* collection) {
    auto& uncommittedWritableCollections = getUncommittedWritableCollections(opCtx);
    uncommittedWritableCollections.remove(collection);
}

CollectionCatalogStasher::CollectionCatalogStasher(OperationContext* opCtx)
    : _opCtx(opCtx), _stashed(false) {}
CollectionCatalogStasher::CollectionCatalogStasher(OperationContext* opCtx,
                                                   std::shared_ptr<const CollectionCatalog> catalog)
    : _opCtx(opCtx), _stashed(true) {
    invariant(catalog);
    CollectionCatalog::stash(_opCtx, std::move(catalog));
}
CollectionCatalogStasher::~CollectionCatalogStasher() {
    reset();
}

CollectionCatalogStasher::CollectionCatalogStasher(CollectionCatalogStasher&& other)
    : _opCtx(other._opCtx), _stashed(other._stashed) {
    other._stashed = false;
}

void CollectionCatalogStasher::stash(std::shared_ptr<const CollectionCatalog> catalog) {
    CollectionCatalog::stash(_opCtx, std::move(catalog));
    _stashed = true;
}

void CollectionCatalogStasher::reset() {
    if (_stashed) {
        CollectionCatalog::stash(_opCtx, nullptr);
        _stashed = false;
    }
}

const Collection* LookupCollectionForYieldRestore::operator()(OperationContext* opCtx,
                                                              CollectionUUID uuid) const {
    return CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, uuid).get();
}

}  // namespace mongo
