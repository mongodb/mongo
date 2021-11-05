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
#include "mongo/db/storage/snapshot_helper.h"
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

std::shared_ptr<CollectionCatalog> batchedCatalogWriteInstance;

/**
 * Decoration on OperationContext to store cloned Collections until they are committed or rolled
 * back TODO SERVER-51236: This should be merged with UncommittedCollections
 */
class UncommittedCatalogUpdates {
public:
    struct Entry {
        enum class Action {
            // Writable clone
            kWritable,
            // Marker to indicate that the namespace has been renamed
            kRenamed,
            // Dropped collection instance
            kDropped,
            // Recreated collection after drop
            kRecreated
        };

        CollectionUUID uuid() const {
            if (action == Action::kDropped || action == Entry::Action::kRecreated)
                return *externalUUID;
            return collection->uuid();
        }

        // Type of action this entry has stored. Members below may or may not be set depending on
        // this member.
        Action action;

        // Storage for the actual collection.
        // Set for actions kWritable, kRecreated. nullptr otherwise.
        std::shared_ptr<Collection> collection;

        // Store namespace separately to handle rename and drop without making writable first
        // Set for all actions
        NamespaceString nss;

        // External uuid when not accessible via collection
        // Set for actions kDropped, kRecreated. boost::none otherwise.
        boost::optional<UUID> externalUUID;

        // New namespace this collection has been renamed to
        // Set for action kRenamed. Default constructed otherwise.
        NamespaceString renameTo;
    };

    /**
     * Lookup of Collection by UUID. The boolean indicates if this namespace is managed.
     * A managed Collection pointer may be returned as nullptr, which indicates a drop.
     * If the returned boolean is false then the Collection will always be nullptr.
     */
    std::pair<bool, Collection*> lookup(CollectionUUID uuid) const {
        // Doing reverse search so we find most recent entry affecting this uuid
        auto it = std::find_if(_entries.rbegin(), _entries.rend(), [uuid](auto&& entry) {
            // Rename actions don't have UUID
            if (entry.action == Entry::Action::kRenamed)
                return false;

            return entry.uuid() == uuid;
        });
        if (it == _entries.rend())
            return {false, nullptr};
        return {true, it->collection.get()};
    }

    /**
     * Lookup of Collection by NamespaceString. The boolean indicates if this namespace is managed.
     * A managed Collection pointer may be returned as nullptr, which indicates drop or rename.
     * If the returned boolean is false then the Collection will always be nullptr.
     */
    std::pair<bool, Collection*> lookup(const NamespaceString& nss) const {
        // Doing reverse search so we find most recent entry affecting this namespace
        auto it = std::find_if(
            _entries.rbegin(), _entries.rend(), [&nss](auto&& entry) { return entry.nss == nss; });
        if (it == _entries.rend())
            return {false, nullptr};
        return {true, it->collection.get()};
    }

    /**
     * Manage the lifetime of uncommitted writable collection
     */
    void writable(std::shared_ptr<Collection> collection) {
        const auto& ns = collection->ns();
        _entries.push_back({Entry::Action::kWritable, std::move(collection), ns});
    }

    /**
     * Manage an uncommitted rename, pointer must have made writable first and should exist in entry
     * list
     */
    void rename(const Collection* collection, const NamespaceString& from) {
        auto it = std::find_if(_entries.rbegin(), _entries.rend(), [collection](auto&& entry) {
            return entry.collection.get() == collection;
        });
        invariant(it != _entries.rend());
        it->nss = collection->ns();
        _entries.push_back({Entry::Action::kRenamed, nullptr, from, boost::none, it->nss});
    }

    /**
     * Manage an uncommitted collection drop
     */
    void drop(const Collection* collection) {
        auto it = std::find_if(
            _entries.rbegin(), _entries.rend(), [uuid = collection->uuid()](auto&& entry) {
                return entry.uuid() == uuid;
            });
        if (it == _entries.rend()) {
            // Entry with this uuid was not found, add new
            _entries.push_back(
                {Entry::Action::kDropped, nullptr, collection->ns(), collection->uuid()});
            return;
        }

        // If we have been recreated after drop we can simply just erase this entry so lookup will
        // then find previous drop
        if (it->action == Entry::Action::kRecreated) {
            _entries.erase(it.base());
            return;
        }

        // Entry is already without Collection pointer, nothing to do
        if (!it->collection)
            return;

        // Transform found entry into dropped.
        invariant(it->collection.get() == collection);
        it->action = Entry::Action::kDropped;
        it->externalUUID = it->collection->uuid();
        it->collection = nullptr;
    }

    /**
     * Re-creates a collection that has previously been dropped
     */
    void createAfterDrop(CollectionUUID uuid, std::shared_ptr<Collection> collection) {
        const auto& ns = collection->ns();
        _entries.push_back({Entry::Action::kRecreated, std::move(collection), ns, uuid});
    }

    /**
     * Releases all entries, needs to be done when WriteUnitOfWork commits or rolls back.
     */
    std::vector<Entry> releaseEntries() {
        std::vector<Entry> ret;
        std::swap(ret, _entries);
        return ret;
    }

private:
    // Store entries in vector, we will do linear search to find what we're looking for but it will
    // be very few entries so it should be fine.
    std::vector<Entry> _entries;
};

const OperationContext::Decoration<UncommittedCatalogUpdates> getUncommittedCatalogUpdates =
    OperationContext::declareDecoration<UncommittedCatalogUpdates>();

const OperationContext::Decoration<std::shared_ptr<const CollectionCatalog>> stashedCatalog =
    OperationContext::declareDecoration<std::shared_ptr<const CollectionCatalog>>();

}  // namespace

/**
 * Publishes all uncommitted Collection actions registered on UncommittedCatalogUpdates to the
 * catalog. All catalog updates are performed under the same write to ensure no external observer
 * can see a partial update. Cleans up UncommittedCatalogUpdates on both commit and rollback to
 * make it behave like a decoration on a WriteUnitOfWork.
 *
 * It needs to be registered with registerChangeForCatalogVisibility so other commit handlers can
 * still write to this Collection.
 */
class CollectionCatalog::PublishCatalogUpdates final : public RecoveryUnit::Change {
public:
    static constexpr size_t kNumStaticActions = 2;

    PublishCatalogUpdates(OperationContext* opCtx,
                          UncommittedCatalogUpdates& uncommittedCatalogUpdates)
        : _opCtx(opCtx), _uncommittedCatalogUpdates(uncommittedCatalogUpdates) {}

    static void ensureRegisteredWithRecoveryUnit(
        OperationContext* opCtx, UncommittedCatalogUpdates& UncommittedCatalogUpdates) {
        if (opCtx->recoveryUnit()->hasRegisteredChangeForCatalogVisibility())
            return;

        opCtx->recoveryUnit()->registerChangeForCatalogVisibility(
            std::make_unique<PublishCatalogUpdates>(opCtx, UncommittedCatalogUpdates));
    }

    void commit(boost::optional<Timestamp> commitTime) override {
        boost::container::small_vector<CollectionCatalog::CatalogWriteFn, kNumStaticActions>
            writeJobs;

        // Create catalog write jobs for all updates registered in this WriteUnitOfWork
        auto entries = _uncommittedCatalogUpdates.releaseEntries();
        for (auto&& entry : entries) {
            switch (entry.action) {
                case UncommittedCatalogUpdates::Entry::Action::kWritable:
                    writeJobs.push_back(
                        [collection = std::move(entry.collection)](CollectionCatalog& catalog) {
                            catalog._collections[collection->ns()] = collection;
                            catalog._catalog[collection->uuid()] = collection;
                            auto dbIdPair = std::make_pair(collection->ns().db().toString(),
                                                           collection->uuid());
                            catalog._orderedCollections[dbIdPair] = collection;
                        });
                    break;
                case UncommittedCatalogUpdates::Entry::Action::kRenamed:
                    writeJobs.push_back(
                        [& from = entry.nss, &to = entry.renameTo](CollectionCatalog& catalog) {
                            catalog._collections.erase(from);

                            auto fromStr = from.ns();
                            auto toStr = to.ns();

                            ResourceId oldRid = ResourceId(RESOURCE_COLLECTION, fromStr);
                            ResourceId newRid = ResourceId(RESOURCE_COLLECTION, toStr);

                            catalog.removeResource(oldRid, fromStr);
                            catalog.addResource(newRid, toStr);
                        });
                    break;
                case UncommittedCatalogUpdates::Entry::Action::kDropped:
                    writeJobs.push_back(
                        [opCtx = _opCtx, uuid = entry.uuid()](CollectionCatalog& catalog) {
                            catalog.deregisterCollection(opCtx, uuid);
                        });
                    break;
                case UncommittedCatalogUpdates::Entry::Action::kRecreated:
                    writeJobs.push_back([opCtx = _opCtx,
                                         collection = std::move(entry.collection),
                                         uuid = *entry.externalUUID](CollectionCatalog& catalog) {
                        catalog.registerCollection(opCtx, uuid, std::move(collection));
                    });
                    break;
            };
        }

        // Write all catalog updates to the catalog in the same write to ensure atomicity.
        if (!writeJobs.empty()) {
            CollectionCatalog::write(_opCtx, [&writeJobs](CollectionCatalog& catalog) {
                for (auto&& job : writeJobs) {
                    job(catalog);
                }
            });
        }
    }

    void rollback() override {
        _uncommittedCatalogUpdates.releaseEntries();
    }

private:
    OperationContext* _opCtx;
    UncommittedCatalogUpdates& _uncommittedCatalogUpdates;
};

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

bool CollectionCatalog::iterator::operator==(const iterator& other) const {
    invariant(_catalog == other._catalog);
    if (other._mapIter == _catalog->_orderedCollections.end()) {
        return _uuid == boost::none;
    }

    return _uuid == other._uuid;
}

bool CollectionCatalog::iterator::operator!=(const iterator& other) const {
    return !(*this == other);
}

bool CollectionCatalog::iterator::_exhausted() {
    return _mapIter == _catalog->_orderedCollections.end() || _mapIter->first.first != _dbName;
}

std::shared_ptr<const CollectionCatalog> CollectionCatalog::get(ServiceContext* svcCtx) {
    return atomic_load(&getCatalog(svcCtx).catalog);
}

std::shared_ptr<const CollectionCatalog> CollectionCatalog::get(OperationContext* opCtx) {
    // If there is a batched catalog write ongoing and we are the one doing it return this instance
    // so we can observe our own writes. There may be other callers that reads the CollectionCatalog
    // without any locks, they must see the immutable regular instance.
    if (batchedCatalogWriteInstance && opCtx->lockState()->isW()) {
        return batchedCatalogWriteInstance;
    }

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
    // We should never have ongoing batching here. When batching is in progress the caller should
    // use the overload with OperationContext so we can verify that the global exlusive lock is
    // being held.
    invariant(!batchedCatalogWriteInstance);

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

            // Throw any exception that was caught during execution of our job. Make sure we destroy
            // the exception_ptr on the same thread that throws the exception to avoid a data race
            // between destroying the exception_ptr and reading the exception.
            auto ex = std::move(completion->exception);
            if (ex)
                std::rethrow_exception(ex);
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
    // If global MODE_X lock are held we can re-use a cloned CollectionCatalog instance when
    // 'batchedCatalogWriteInstance' is set. Make sure we are the one holding the write lock.
    if (batchedCatalogWriteInstance) {
        invariant(opCtx->lockState()->isW());
        job(*batchedCatalogWriteInstance);
        return;
    }

    write(opCtx->getServiceContext(), std::move(job));
}


void CollectionCatalog::onCollectionRename(OperationContext* opCtx,
                                           Collection* coll,
                                           const NamespaceString& fromCollection) const {
    invariant(coll);

    auto& uncommittedCatalogUpdates = getUncommittedCatalogUpdates(opCtx);
    uncommittedCatalogUpdates.rename(coll, fromCollection);
}

void CollectionCatalog::dropCollection(OperationContext* opCtx, Collection* coll) const {
    invariant(coll);

    auto& uncommittedCatalogUpdates = getUncommittedCatalogUpdates(opCtx);
    uncommittedCatalogUpdates.drop(coll);

    // Requesting a writable collection normally ensures we have registered PublishCatalogUpdates
    // with the recovery unit. However, when the writable Collection was requested in Inplace mode
    // (or is the oplog) this is not the case. So make sure we are registered in all cases.
    PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);
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

    auto& uncommittedCatalogUpdates = getUncommittedCatalogUpdates(opCtx);
    auto [found, uncommittedPtr] = uncommittedCatalogUpdates.lookup(uuid);
    // If UUID is managed by uncommittedCatalogUpdates return the pointer which will be nullptr in
    // case of a drop. We don't need to check UncommittedCollections as we will never share UUID for
    // a new Collection.
    if (found) {
        return uncommittedPtr;
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
    auto ptr = cloned.get();
    uncommittedCatalogUpdates.writable(std::move(cloned));

    PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);

    return ptr;
}

CollectionPtr CollectionCatalog::lookupCollectionByUUID(OperationContext* opCtx,
                                                        CollectionUUID uuid) const {
    auto& uncommittedCatalogUpdates = getUncommittedCatalogUpdates(opCtx);
    auto [found, uncommittedPtr] = uncommittedCatalogUpdates.lookup(uuid);
    // If UUID is managed by uncommittedCatalogUpdates return the pointer which will be nullptr in
    // case of a drop. We don't need to check UncommittedCollections as we will never share UUID for
    // a new Collection.
    if (found) {
        return uncommittedPtr;
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

    auto& uncommittedCatalogUpdates = getUncommittedCatalogUpdates(opCtx);
    auto [found, uncommittedPtr] = uncommittedCatalogUpdates.lookup(nss);
    // If uncommittedPtr is valid, found is always true. Return the pointer as the collection still
    // exists.
    if (uncommittedPtr) {
        return uncommittedPtr;
    }

    // If found=true above but we don't have a Collection pointer it is a drop or rename. But first
    // check UncommittedCollections in case we find a new collection there.
    if (auto coll = UncommittedCollections::getForTxn(opCtx, nss)) {
        invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));
        return coll.get();
    }

    // Report the drop or rename as nothing new was created.
    if (found) {
        return nullptr;
    }

    auto it = _collections.find(nss);
    auto coll = (it == _collections.end() ? nullptr : it->second);

    if (!coll || !coll->isCommitted())
        return nullptr;

    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));
    auto cloned = coll->clone();
    auto ptr = cloned.get();
    uncommittedCatalogUpdates.writable(std::move(cloned));

    PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);

    return ptr;
}

CollectionPtr CollectionCatalog::lookupCollectionByNamespace(OperationContext* opCtx,
                                                             const NamespaceString& nss) const {
    auto& uncommittedCatalogUpdates = getUncommittedCatalogUpdates(opCtx);
    auto [found, uncommittedPtr] = uncommittedCatalogUpdates.lookup(nss);
    // If uncommittedPtr is valid, found is always true. Return the pointer as the collection still
    // exists.
    if (uncommittedPtr) {
        return uncommittedPtr;
    }

    // If found=true above but we don't have a Collection pointer it is a drop or rename. But first
    // check UncommittedCollections in case we find a new collection there.
    if (auto coll = UncommittedCollections::getForTxn(opCtx, nss)) {
        return {opCtx, coll.get(), LookupCollectionForYieldRestore()};
    }

    // Report the drop or rename as nothing new was created.
    if (found) {
        return nullptr;
    }

    auto it = _collections.find(nss);
    auto coll = (it == _collections.end() ? nullptr : it->second);
    return (coll && coll->isCommitted())
        ? CollectionPtr(opCtx, coll.get(), LookupCollectionForYieldRestore())
        : nullptr;
}

boost::optional<NamespaceString> CollectionCatalog::lookupNSSByUUID(OperationContext* opCtx,
                                                                    CollectionUUID uuid) const {
    auto& uncommittedCatalogUpdates = getUncommittedCatalogUpdates(opCtx);
    auto [found, uncommittedPtr] = uncommittedCatalogUpdates.lookup(uuid);
    // If UUID is managed by uncommittedCatalogUpdates return its corresponding namespace if the
    // Collection exists, boost::none otherwise.
    if (found) {
        if (uncommittedPtr)
            return uncommittedPtr->ns();
        else
            return boost::none;
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
    auto& uncommittedCatalogUpdates = getUncommittedCatalogUpdates(opCtx);
    auto [found, uncommittedPtr] = uncommittedCatalogUpdates.lookup(nss);
    if (uncommittedPtr) {
        return uncommittedPtr->uuid();
    }

    if (auto coll = UncommittedCollections::getForTxn(opCtx, nss)) {
        return coll->uuid();
    }

    if (found) {
        return boost::none;
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

CollectionCatalog::Stats CollectionCatalog::getStats() const {
    return _stats;
}

CollectionCatalog::ViewCatalogSet CollectionCatalog::getViewCatalogDbNames() const {
    ViewCatalogSet results;
    for (const auto& dbNameViewSetPair : _views) {
        results.insert(dbNameViewSetPair.first);
    }
    return results;
}

void CollectionCatalog::registerCollection(OperationContext* opCtx,
                                           CollectionUUID uuid,
                                           std::shared_ptr<Collection> coll) {
    auto ns = coll->ns();
    if (auto it = _views.find(ns.db()); it != _views.end()) {
        uassert(ErrorCodes::NamespaceExists,
                str::stream() << "View already exists. NS: " << ns,
                !it->second.contains(ns));
    }
    if (_collections.find(ns) != _collections.end()) {
        auto& uncommittedCatalogUpdates = getUncommittedCatalogUpdates(opCtx);
        auto [found, uncommittedPtr] = uncommittedCatalogUpdates.lookup(ns);
        // If we have an uncommitted drop of this collection we can defer the creation, the register
        // will happen in the same catalog write as the drop.
        if (found && !uncommittedPtr) {
            uncommittedCatalogUpdates.createAfterDrop(uuid, std::move(coll));
            return;
        }

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

    if (!ns.isOnInternalDb() && !ns.isSystem()) {
        _stats.userCollections += 1;
        if (coll->isCapped()) {
            _stats.userCapped += 1;
        }
    } else {
        _stats.internal += 1;
    }

    invariant(static_cast<size_t>(_stats.internal + _stats.userCollections) == _collections.size());

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

    if (!ns.isOnInternalDb() && !ns.isSystem()) {
        _stats.userCollections -= 1;
        if (coll->isCapped()) {
            _stats.userCapped -= 1;
        }
    } else {
        _stats.internal -= 1;
    }

    invariant(static_cast<size_t>(_stats.internal + _stats.userCollections) == _collections.size());

    coll->onDeregisterFromCatalog(opCtx);

    auto collRid = ResourceId(RESOURCE_COLLECTION, ns.ns());
    removeResource(collRid, ns.ns());

    return coll;
}

void CollectionCatalog::deregisterAllCollectionsAndViews() {
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
    _views.clear();
    _stats = {};

    _resourceInformation.clear();
}

void CollectionCatalog::registerView(const NamespaceString& ns) {
    if (_collections.contains(ns)) {
        LOGV2(5706100, "Conflicted creating a view", "ns"_attr = ns);
        throw WriteConflictException();
    }

    _views[ns.db()].insert(ns);
}
void CollectionCatalog::deregisterView(const NamespaceString& ns) {
    auto it = _views.find(ns.db());
    if (it == _views.end()) {
        return;
    }

    auto& viewsForDb = it->second;
    viewsForDb.erase(ns);
    if (viewsForDb.empty()) {
        _views.erase(it);
    }
}

void CollectionCatalog::replaceViewsForDatabase(StringData dbName,
                                                absl::flat_hash_set<NamespaceString> views) {
    if (views.empty())
        _views.erase(dbName);
    else {
        _views[dbName] = std::move(views);
    }
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

CollectionCatalogStasher::CollectionCatalogStasher(OperationContext* opCtx)
    : _opCtx(opCtx), _stashed(false) {}

CollectionCatalogStasher::CollectionCatalogStasher(OperationContext* opCtx,
                                                   std::shared_ptr<const CollectionCatalog> catalog)
    : _opCtx(opCtx), _stashed(true) {
    invariant(catalog);
    CollectionCatalog::stash(_opCtx, std::move(catalog));
}

CollectionCatalogStasher::CollectionCatalogStasher(CollectionCatalogStasher&& other)
    : _opCtx(other._opCtx), _stashed(other._stashed) {
    other._stashed = false;
}

CollectionCatalogStasher::~CollectionCatalogStasher() {
    if (_opCtx->isLockFreeReadsOp()) {
        // Leave the catalog stashed on the opCtx because there is another Stasher instance still
        // using it.
        return;
    }

    reset();
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
    auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, uuid).get();
    if (!collection)
        return nullptr;

    // After yielding and reacquiring locks, the preconditions that were used to select our
    // ReadSource initially need to be checked again. We select a ReadSource based on replication
    // state. After a query yields its locks, the replication state may have changed, invalidating
    // our current choice of ReadSource. Using the same preconditions, change our ReadSource if
    // necessary.
    auto [newReadSource, _] = SnapshotHelper::shouldChangeReadSource(opCtx, collection->ns());
    if (newReadSource) {
        opCtx->recoveryUnit()->setTimestampReadSource(*newReadSource);
    }

    return collection;
}

BatchedCollectionCatalogWriter::BatchedCollectionCatalogWriter(OperationContext* opCtx)
    : _opCtx(opCtx) {
    invariant(_opCtx->lockState()->isW());
    invariant(!batchedCatalogWriteInstance);

    auto& storage = getCatalog(_opCtx->getServiceContext());
    // hold onto base so if we need to delete it we can do it outside of the lock
    _base = atomic_load(&storage.catalog);
    // copy the collection catalog, this could be expensive, store it for future writes during this
    // batcher
    batchedCatalogWriteInstance = std::make_shared<CollectionCatalog>(*_base);
}
BatchedCollectionCatalogWriter::~BatchedCollectionCatalogWriter() {
    invariant(_opCtx->lockState()->isW());

    // Publish out batched instance, validate that no other writers have been able to write during
    // the batcher.
    auto& storage = getCatalog(_opCtx->getServiceContext());
    invariant(
        atomic_compare_exchange_strong(&storage.catalog, &_base, batchedCatalogWriteInstance));

    // Clear out batched pointer so no more attempts of batching are made
    batchedCatalogWriteInstance = nullptr;
}

}  // namespace mongo
