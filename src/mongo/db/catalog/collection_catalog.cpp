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

#include "mongo/platform/basic.h"

#include "collection_catalog.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/uncommitted_catalog_updates.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/resource_catalog.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot_helper.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {
struct LatestCollectionCatalog {
    std::shared_ptr<CollectionCatalog> catalog = std::make_shared<CollectionCatalog>();
};
const ServiceContext::Decoration<LatestCollectionCatalog> getCatalog =
    ServiceContext::declareDecoration<LatestCollectionCatalog>();

std::shared_ptr<CollectionCatalog> batchedCatalogWriteInstance;

const OperationContext::Decoration<std::shared_ptr<const CollectionCatalog>> stashedCatalog =
    OperationContext::declareDecoration<std::shared_ptr<const CollectionCatalog>>();

/**
 * Returns true if the collection is compatible with the read timestamp.
 */
bool isCollectionCompatible(std::shared_ptr<Collection> coll, Timestamp readTimestamp) {
    if (!coll) {
        return false;
    }

    boost::optional<Timestamp> minValidSnapshot = coll->getMinimumValidSnapshot();
    if (!minValidSnapshot) {
        // Collection is valid in all snapshots.
        return true;
    }
    return readTimestamp >= *minValidSnapshot;
}

void assertViewCatalogValid(const ViewsForDatabase& viewsForDb) {
    uassert(ErrorCodes::InvalidViewDefinition,
            "Invalid view definition detected in the view catalog. Remove the invalid view "
            "manually to prevent disallowing any further usage of the view catalog.",
            viewsForDb.valid());
}

}  // namespace

class IgnoreExternalViewChangesForDatabase {
public:
    IgnoreExternalViewChangesForDatabase(OperationContext* opCtx, const DatabaseName& dbName)
        : _opCtx(opCtx), _dbName(dbName) {
        auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(_opCtx);
        uncommittedCatalogUpdates.setIgnoreExternalViewChanges(_dbName, true);
    }

    ~IgnoreExternalViewChangesForDatabase() {
        auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(_opCtx);
        uncommittedCatalogUpdates.setIgnoreExternalViewChanges(_dbName, false);
    }

private:
    OperationContext* _opCtx;
    DatabaseName _dbName;
};

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

    static void setCollectionInCatalog(CollectionCatalog& catalog,
                                       std::shared_ptr<Collection> collection,
                                       boost::optional<Timestamp> commitTime) {
        if (commitTime) {
            collection->setMinimumValidSnapshot(*commitTime);
        }

        catalog._collections[collection->ns()] = collection;
        catalog._catalog[collection->uuid()] = collection;
        auto dbIdPair = std::make_pair(collection->ns().dbName(), collection->uuid());
        catalog._orderedCollections[dbIdPair] = collection;

        catalog._pendingCommitNamespaces.erase(collection->ns());
        catalog._pendingCommitUUIDs.erase(collection->uuid());
    }

    PublishCatalogUpdates(UncommittedCatalogUpdates& uncommittedCatalogUpdates)
        : _uncommittedCatalogUpdates(uncommittedCatalogUpdates) {}

    static void ensureRegisteredWithRecoveryUnit(
        OperationContext* opCtx, UncommittedCatalogUpdates& uncommittedCatalogUpdates) {
        if (opCtx->recoveryUnit()->hasRegisteredChangeForCatalogVisibility())
            return;

        if (feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCV()) {
            opCtx->recoveryUnit()->registerPreCommitHook(
                [](OperationContext* opCtx) { PublishCatalogUpdates::preCommit(opCtx); });
        }

        opCtx->recoveryUnit()->registerChangeForCatalogVisibility(
            std::make_unique<PublishCatalogUpdates>(uncommittedCatalogUpdates));
    }

    static void preCommit(OperationContext* opCtx) {
        const auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
        const auto& entries = uncommittedCatalogUpdates.entries();

        if (std::none_of(
                entries.begin(), entries.end(), UncommittedCatalogUpdates::isTwoPhaseCommitEntry)) {
            // Nothing to do, avoid calling CollectionCatalog::write.
            return;
        }
        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            for (auto&& entry : entries) {
                if (!UncommittedCatalogUpdates::isTwoPhaseCommitEntry(entry)) {
                    continue;
                }

                // Mark the namespace as pending commit even if we don't have a collection instance.
                std::shared_ptr<Collection>& collection =
                    catalog._pendingCommitNamespaces[entry.nss];

                if (!entry.collection)
                    continue;

                collection = entry.collection;
                catalog._pendingCommitUUIDs[collection->uuid()] = collection;
            }
        });
    }

    void commit(OperationContext* opCtx, boost::optional<Timestamp> commitTime) override {
        boost::container::small_vector<CollectionCatalog::CatalogWriteFn, kNumStaticActions>
            writeJobs;

        // Create catalog write jobs for all updates registered in this WriteUnitOfWork
        auto entries = _uncommittedCatalogUpdates.releaseEntries();
        for (auto&& entry : entries) {
            switch (entry.action) {
                case UncommittedCatalogUpdates::Entry::Action::kWritableCollection: {
                    writeJobs.push_back([collection = std::move(entry.collection),
                                         commitTime](CollectionCatalog& catalog) {
                        setCollectionInCatalog(catalog, std::move(collection), commitTime);
                    });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kRenamedCollection: {
                    writeJobs.push_back([opCtx,
                                         &from = entry.nss,
                                         &to = entry.renameTo,
                                         commitTime](CollectionCatalog& catalog) {
                        // We just need to do modifications on 'from' here. 'to' is taken care
                        // of by a separate kWritableCollection entry.
                        catalog._collections.erase(from);
                        catalog._pendingCommitNamespaces.erase(from);

                        auto& resourceCatalog = ResourceCatalog::get(opCtx->getServiceContext());
                        resourceCatalog.remove({RESOURCE_COLLECTION, from}, from);
                        resourceCatalog.add({RESOURCE_COLLECTION, to}, to);

                        catalog._pushCatalogIdForRename(from, to, commitTime);
                    });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kDroppedCollection: {
                    writeJobs.push_back([opCtx,
                                         uuid = *entry.uuid(),
                                         isDropPending = *entry.isDropPending,
                                         commitTime](CollectionCatalog& catalog) {
                        catalog.deregisterCollection(opCtx, uuid, isDropPending, commitTime);
                    });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kRecreatedCollection: {
                    writeJobs.push_back([opCtx,
                                         collection = entry.collection,
                                         uuid = *entry.externalUUID,
                                         commitTime](CollectionCatalog& catalog) {
                        // Override existing Collection on this namespace
                        catalog._registerCollection(opCtx,
                                                    uuid,
                                                    std::move(collection),
                                                    /*twoPhase=*/false,
                                                    /*ts=*/commitTime);
                    });
                    // Fallthrough to the createCollection case to finish committing the collection.
                    [[fallthrough]];
                }
                case UncommittedCatalogUpdates::Entry::Action::kCreatedCollection: {
                    // By this point, we may or may not have reserved an oplog slot for the
                    // collection creation.
                    // For example, multi-document transactions will only reserve the oplog slot at
                    // commit time. As a result, we may or may not have a reliable value to use to
                    // set the new collection's minimum visible snapshot until commit time.
                    // Pre-commit hooks do not presently have awareness of the commit timestamp, so
                    // we must update the minVisibleTimestamp with the appropriate value. This is
                    // fine because the collection should not be visible in the catalog until we
                    // call setCommitted(true).
                    writeJobs.push_back([coll = entry.collection.get(),
                                         commitTime](CollectionCatalog& catalog) {
                        if (commitTime) {
                            coll->setMinimumVisibleSnapshot(commitTime.value());
                            coll->setMinimumValidSnapshot(commitTime.value());
                        }
                        catalog._pushCatalogIdForNSS(coll->ns(), coll->getCatalogId(), commitTime);

                        catalog._pendingCommitNamespaces.erase(coll->ns());
                        catalog._pendingCommitUUIDs.erase(coll->uuid());
                        coll->setCommitted(true);
                    });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kReplacedViewsForDatabase: {
                    writeJobs.push_back(
                        [dbName = entry.nss.dbName(),
                         &viewsForDb = entry.viewsForDb.value()](CollectionCatalog& catalog) {
                            catalog._replaceViewsForDatabase(dbName, std::move(viewsForDb));
                        });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kAddViewResource: {
                    writeJobs.push_back([opCtx, &viewName = entry.nss](CollectionCatalog& catalog) {
                        ResourceCatalog::get(opCtx->getServiceContext())
                            .add({RESOURCE_COLLECTION, viewName}, viewName);
                        catalog.deregisterUncommittedView(viewName);
                    });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kRemoveViewResource: {
                    writeJobs.push_back([opCtx, &viewName = entry.nss](CollectionCatalog& catalog) {
                        ResourceCatalog::get(opCtx->getServiceContext())
                            .remove({RESOURCE_COLLECTION, viewName}, viewName);
                    });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kDroppedIndex: {
                    writeJobs.push_back(
                        [opCtx,
                         indexEntry = entry.indexEntry,
                         isDropPending = *entry.isDropPending](CollectionCatalog& catalog) {
                            catalog.deregisterIndex(opCtx, std::move(indexEntry), isDropPending);
                        });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kOpenedCollection: {
                    // No-op. Collections opened from earlier points in time remain local to the
                    // operation.
                    break;
                }
            };
        }

        // Write all catalog updates to the catalog in the same write to ensure atomicity.
        if (!writeJobs.empty()) {
            CollectionCatalog::write(opCtx, [&writeJobs](CollectionCatalog& catalog) {
                for (auto&& job : writeJobs) {
                    job(catalog);
                }
            });
        }
    }

    void rollback(OperationContext* opCtx) override {
        auto entries = _uncommittedCatalogUpdates.releaseEntries();
        if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCV())
            return;

        if (std::none_of(
                entries.begin(), entries.end(), UncommittedCatalogUpdates::isTwoPhaseCommitEntry)) {
            // Nothing to do, avoid calling CollectionCatalog::write.
            return;
        }

        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            for (auto&& entry : entries) {
                if (!UncommittedCatalogUpdates::isTwoPhaseCommitEntry(entry)) {
                    continue;
                }

                catalog._pendingCommitNamespaces.erase(entry.nss);

                // Entry without collection, nothing more to do
                if (!entry.collection)
                    continue;

                catalog._pendingCommitUUIDs.erase(entry.collection->uuid());
            }
        });
    }

private:
    UncommittedCatalogUpdates& _uncommittedCatalogUpdates;
};

CollectionCatalog::iterator::iterator(OperationContext* opCtx,
                                      const DatabaseName& dbName,
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

CollectionCatalog::iterator::iterator(
    OperationContext* opCtx,
    std::map<std::pair<DatabaseName, UUID>, std::shared_ptr<Collection>>::const_iterator mapIter,
    const CollectionCatalog& catalog)
    : _opCtx(opCtx), _mapIter(mapIter), _catalog(&catalog) {}

CollectionCatalog::iterator::value_type CollectionCatalog::iterator::operator*() {
    if (_exhausted()) {
        return CollectionPtr();
    }

    return {
        _opCtx, _mapIter->second.get(), LookupCollectionForYieldRestore(_mapIter->second->ns())};
}

Collection* CollectionCatalog::iterator::getWritableCollection(OperationContext* opCtx) {
    return CollectionCatalog::get(opCtx)->lookupCollectionByUUIDForMetadataWrite(
        opCtx, operator*()->uuid());
}

boost::optional<UUID> CollectionCatalog::iterator::uuid() {
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

Status CollectionCatalog::createView(OperationContext* opCtx,
                                     const NamespaceString& viewName,
                                     const NamespaceString& viewOn,
                                     const BSONArray& pipeline,
                                     const ViewsForDatabase::PipelineValidatorFn& validatePipeline,
                                     const BSONObj& collation,
                                     ViewsForDatabase::Durability durability) const {
    invariant(durability == ViewsForDatabase::Durability::kAlreadyDurable ||
              opCtx->lockState()->isCollectionLockedForMode(viewName, MODE_IX));
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(viewName.dbName(), NamespaceString::kSystemDotViewsCollectionName),
        MODE_X));

    invariant(_viewsForDatabase.contains(viewName.dbName()));
    const ViewsForDatabase& viewsForDb = *_getViewsForDatabase(opCtx, viewName.dbName());

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    if (uncommittedCatalogUpdates.shouldIgnoreExternalViewChanges(viewName.dbName())) {
        return Status::OK();
    }

    if (viewName.db() != viewOn.db())
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    if (viewsForDb.lookup(viewName) || _collections.contains(viewName))
        return Status(ErrorCodes::NamespaceExists, "Namespace already exists");

    if (!NamespaceString::validCollectionName(viewOn.coll()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid name for 'viewOn': " << viewOn.coll());

    IgnoreExternalViewChangesForDatabase ignore(opCtx, viewName.dbName());

    assertViewCatalogValid(viewsForDb);
    auto systemViews = _lookupSystemViews(opCtx, viewName.dbName());

    ViewsForDatabase writable{viewsForDb};
    auto status = writable.insert(
        opCtx, systemViews, viewName, viewOn, pipeline, validatePipeline, collation, durability);

    if (status.isOK()) {
        auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
        uncommittedCatalogUpdates.addView(opCtx, viewName);
        uncommittedCatalogUpdates.replaceViewsForDatabase(viewName.dbName(), std::move(writable));

        PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);
    }

    return status;
}

Status CollectionCatalog::modifyView(
    OperationContext* opCtx,
    const NamespaceString& viewName,
    const NamespaceString& viewOn,
    const BSONArray& pipeline,
    const ViewsForDatabase::PipelineValidatorFn& validatePipeline) const {
    invariant(opCtx->lockState()->isCollectionLockedForMode(viewName, MODE_X));
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(viewName.dbName(), NamespaceString::kSystemDotViewsCollectionName),
        MODE_X));
    invariant(_viewsForDatabase.contains(viewName.dbName()));
    const ViewsForDatabase& viewsForDb = *_getViewsForDatabase(opCtx, viewName.dbName());

    if (viewName.db() != viewOn.db())
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    auto viewPtr = viewsForDb.lookup(viewName);
    if (!viewPtr)
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "cannot modify missing view " << viewName.ns());

    if (!NamespaceString::validCollectionName(viewOn.coll()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid name for 'viewOn': " << viewOn.coll());

    IgnoreExternalViewChangesForDatabase ignore(opCtx, viewName.dbName());

    assertViewCatalogValid(viewsForDb);
    auto systemViews = _lookupSystemViews(opCtx, viewName.dbName());

    ViewsForDatabase writable{viewsForDb};
    auto status = writable.update(opCtx,
                                  systemViews,
                                  viewName,
                                  viewOn,
                                  pipeline,
                                  validatePipeline,
                                  CollatorInterface::cloneCollator(viewPtr->defaultCollator()));

    if (status.isOK()) {
        auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
        uncommittedCatalogUpdates.addView(opCtx, viewName);
        uncommittedCatalogUpdates.replaceViewsForDatabase(viewName.dbName(), std::move(writable));

        PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);
    }

    return status;
}

Status CollectionCatalog::dropView(OperationContext* opCtx, const NamespaceString& viewName) const {
    invariant(opCtx->lockState()->isCollectionLockedForMode(viewName, MODE_IX));
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(viewName.dbName(), NamespaceString::kSystemDotViewsCollectionName),
        MODE_X));
    invariant(_viewsForDatabase.contains(viewName.dbName()));
    const ViewsForDatabase& viewsForDb = *_getViewsForDatabase(opCtx, viewName.dbName());
    assertViewCatalogValid(viewsForDb);

    // Make sure the view exists before proceeding.
    if (auto viewPtr = viewsForDb.lookup(viewName); !viewPtr) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "cannot drop missing view: " << viewName.ns()};
    }

    Status result = Status::OK();
    {
        IgnoreExternalViewChangesForDatabase ignore(opCtx, viewName.dbName());

        auto systemViews = _lookupSystemViews(opCtx, viewName.dbName());

        ViewsForDatabase writable{viewsForDb};
        writable.remove(opCtx, systemViews, viewName);

        // Reload the view catalog with the changes applied.
        result = writable.reload(opCtx, systemViews);
        if (result.isOK()) {
            auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
            uncommittedCatalogUpdates.removeView(viewName);
            uncommittedCatalogUpdates.replaceViewsForDatabase(viewName.dbName(),
                                                              std::move(writable));

            PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx,
                                                                    uncommittedCatalogUpdates);
        }
    }

    return result;
}

Status CollectionCatalog::reloadViews(OperationContext* opCtx, const DatabaseName& dbName) const {
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(dbName, NamespaceString::kSystemDotViewsCollectionName), MODE_IS));

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    if (uncommittedCatalogUpdates.shouldIgnoreExternalViewChanges(dbName)) {
        return Status::OK();
    }

    LOGV2_DEBUG(22546, 1, "Reloading view catalog for database", "db"_attr = dbName.toString());

    ViewsForDatabase viewsForDb;
    auto status = viewsForDb.reload(opCtx, _lookupSystemViews(opCtx, dbName));
    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog._replaceViewsForDatabase(dbName, std::move(viewsForDb));
    });

    return status;
}

std::shared_ptr<Collection> CollectionCatalog::openCollection(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              Timestamp readTimestamp) const {
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCV()) {
        return nullptr;
    }

    // Check if the storage transaction already has this collection instantiated.
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    auto lookupResult = uncommittedCatalogUpdates.lookupCollection(opCtx, nss);
    if (lookupResult.found && !lookupResult.newColl) {
        invariant(isCollectionCompatible(lookupResult.collection, readTimestamp));
        return lookupResult.collection;
    }

    // Try to find a catalog entry matching 'readTimestamp'.
    auto catalogEntry = _fetchPITCatalogEntry(opCtx, nss, readTimestamp);
    if (!catalogEntry) {
        // Treat the collection as non-existent at a point-in-time the same as not-existent at
        // latest.
        return nullptr;
    }

    auto latestCollection = _lookupCollectionByUUID(*catalogEntry->metadata->options.uuid);

    // Return the in-memory Collection instance if it is compatible with the read timestamp.
    if (isCollectionCompatible(latestCollection, readTimestamp)) {
        uncommittedCatalogUpdates.openCollection(opCtx, latestCollection);
        return latestCollection;
    }

    // Use the shared collection state from the latest Collection in the in-memory collection
    // catalog if it is compatible.
    auto compatibleCollection =
        _createCompatibleCollection(opCtx, latestCollection, readTimestamp, catalogEntry.get());
    if (compatibleCollection) {
        uncommittedCatalogUpdates.openCollection(opCtx, compatibleCollection);
        return compatibleCollection;
    }

    // There is no state in-memory that matches the catalog entry. Try to instantiate a new
    // Collection instance from scratch.
    auto newCollection = _createNewPITCollection(opCtx, readTimestamp, catalogEntry.get());
    if (newCollection) {
        uncommittedCatalogUpdates.openCollection(opCtx, newCollection);
    }
    return newCollection;
}

boost::optional<DurableCatalogEntry> CollectionCatalog::_fetchPITCatalogEntry(
    OperationContext* opCtx, const NamespaceString& nss, const Timestamp& readTimestamp) const {
    auto catalogId = lookupCatalogIdByNSS(nss, readTimestamp);
    if (!catalogId) {
        return boost::none;
    }

    return DurableCatalog::get(opCtx)->getParsedCatalogEntry(opCtx, *catalogId);
}

std::shared_ptr<Collection> CollectionCatalog::_createCompatibleCollection(
    OperationContext* opCtx,
    const std::shared_ptr<Collection>& latestCollection,
    const Timestamp& readTimestamp,
    const DurableCatalogEntry& catalogEntry) const {
    // Check if the collection is drop pending, not expired, and compatible with the read timestamp.
    std::shared_ptr<Collection> dropPendingColl = [&]() -> std::shared_ptr<Collection> {
        auto dropPendingIt = _dropPendingCollection.find(catalogEntry.ident);
        if (dropPendingIt == _dropPendingCollection.end()) {
            return nullptr;
        }
        return dropPendingIt->second.lock();
    }();

    if (isCollectionCompatible(dropPendingColl, readTimestamp)) {
        return dropPendingColl;
    }

    // If either the latest or drop pending collection exists, instantiate a new collection using
    // the shared state.
    if (latestCollection || dropPendingColl) {
        LOGV2_DEBUG(6825400,
                    1,
                    "Instantiating a collection using shared state",
                    logAttrs(catalogEntry.metadata->nss),
                    "ident"_attr = catalogEntry.ident,
                    "md"_attr = catalogEntry.metadata->toBSON(),
                    "timestamp"_attr = readTimestamp);

        std::shared_ptr<Collection> collToReturn =
            Collection::Factory::get(opCtx)->make(opCtx,
                                                  catalogEntry.metadata->nss,
                                                  catalogEntry.catalogId,
                                                  catalogEntry.metadata,
                                                  /*rs=*/nullptr);
        Status status = collToReturn->initFromExisting(
            opCtx, latestCollection ? latestCollection : dropPendingColl, readTimestamp);
        if (!status.isOK()) {
            LOGV2_DEBUG(
                6857100, 1, "Failed to instantiate collection", "reason"_attr = status.reason());
            return nullptr;
        }

        return collToReturn;
    }

    return nullptr;
}

std::shared_ptr<Collection> CollectionCatalog::_createNewPITCollection(
    OperationContext* opCtx,
    const Timestamp& readTimestamp,
    const DurableCatalogEntry& catalogEntry) const {
    // The ident is expired, but it still may not have been dropped by the reaper. Try to mark it as
    // in use.
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto newIdent = storageEngine->markIdentInUse(catalogEntry.ident);
    if (!newIdent) {
        LOGV2_DEBUG(6857101,
                    1,
                    "Collection ident is being dropped or is already dropped",
                    "ident"_attr = catalogEntry.ident);
        return nullptr;
    }

    // Instantiate a new collection without any shared state.
    LOGV2_DEBUG(6825401,
                1,
                "Instantiating a new collection",
                logAttrs(catalogEntry.metadata->nss),
                "ident"_attr = catalogEntry.ident,
                "md"_attr = catalogEntry.metadata->toBSON(),
                "timestamp"_attr = readTimestamp);

    std::unique_ptr<RecordStore> rs =
        opCtx->getServiceContext()->getStorageEngine()->getEngine()->getRecordStore(
            opCtx, catalogEntry.metadata->nss, catalogEntry.ident, catalogEntry.metadata->options);

    // Set the ident to the one returned by the ident reaper. This is to prevent the ident from
    // being dropping prematurely.
    rs->setIdent(std::move(newIdent));

    std::shared_ptr<Collection> collToReturn =
        Collection::Factory::get(opCtx)->make(opCtx,
                                              catalogEntry.metadata->nss,
                                              catalogEntry.catalogId,
                                              catalogEntry.metadata,
                                              std::move(rs));
    Status status = collToReturn->initFromExisting(opCtx, /*collection=*/nullptr, readTimestamp);
    if (!status.isOK()) {
        LOGV2_DEBUG(
            6857102, 1, "Failed to instantiate collection", "reason"_attr = status.reason());
        return nullptr;
    }

    return collToReturn;
}

std::shared_ptr<IndexCatalogEntry> CollectionCatalog::findDropPendingIndex(
    const std::string& ident) const {
    auto it = _dropPendingIndex.find(ident);
    if (it == _dropPendingIndex.end()) {
        return nullptr;
    }

    return it->second.lock();
}

void CollectionCatalog::onCreateCollection(OperationContext* opCtx,
                                           std::shared_ptr<Collection> coll) const {
    invariant(coll);

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    auto [found, existingColl, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, coll->ns());
    uassert(31370,
            str::stream() << "collection already exists. ns: " << coll->ns(),
            existingColl == nullptr);

    // When we already have a drop and recreate the collection, we want to seamlessly swap out the
    // collection in the catalog under a single critical section. So we register the recreated
    // collection in the same commit handler that we unregister the dropped collection (as opposed
    // to registering the new collection inside of a preCommitHook).
    if (found) {
        uncommittedCatalogUpdates.recreateCollection(opCtx, std::move(coll));
    } else {
        uncommittedCatalogUpdates.createCollection(opCtx, std::move(coll));
    }

    PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);
}

void CollectionCatalog::onCollectionRename(OperationContext* opCtx,
                                           Collection* coll,
                                           const NamespaceString& fromCollection) const {
    invariant(coll);

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    uncommittedCatalogUpdates.renameCollection(coll, fromCollection);
}

void CollectionCatalog::dropIndex(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  std::shared_ptr<IndexCatalogEntry> indexEntry,
                                  bool isDropPending) const {
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    uncommittedCatalogUpdates.dropIndex(nss, std::move(indexEntry), isDropPending);
    PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);
}

void CollectionCatalog::dropCollection(OperationContext* opCtx,
                                       Collection* coll,
                                       bool isDropPending) const {
    invariant(coll);

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    uncommittedCatalogUpdates.dropCollection(coll, isDropPending);

    // Requesting a writable collection normally ensures we have registered PublishCatalogUpdates
    // with the recovery unit. However, when the writable Collection was requested in Inplace mode
    // (or is the oplog) this is not the case. So make sure we are registered in all cases.
    PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);
}

void CollectionCatalog::onCloseDatabase(OperationContext* opCtx, DatabaseName dbName) {
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_X));
    ResourceCatalog::get(opCtx->getServiceContext()).remove({RESOURCE_DATABASE, dbName}, dbName);
    _viewsForDatabase.erase(dbName);
}

void CollectionCatalog::onCloseCatalog() {
    if (_shadowCatalog) {
        return;
    }

    _shadowCatalog.emplace();
    for (auto& entry : _catalog)
        _shadowCatalog->insert({entry.first, entry.second->ns()});
}

void CollectionCatalog::onOpenCatalog() {
    invariant(_shadowCatalog);
    _shadowCatalog.reset();
    ++_epoch;
}

uint64_t CollectionCatalog::getEpoch() const {
    return _epoch;
}

std::shared_ptr<const Collection> CollectionCatalog::lookupCollectionByUUIDForRead(
    OperationContext* opCtx, const UUID& uuid) const {
    auto [found, uncommittedColl, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, uuid);
    if (uncommittedColl) {
        return uncommittedColl;
    }

    auto coll = _lookupCollectionByUUID(uuid);
    return (coll && coll->isCommitted()) ? coll : nullptr;
}

Collection* CollectionCatalog::lookupCollectionByUUIDForMetadataWrite(OperationContext* opCtx,
                                                                      const UUID& uuid) const {
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    auto [found, uncommittedPtr, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, uuid);
    if (found) {
        // The uncommittedPtr will be nullptr in the case of drop.
        if (!uncommittedPtr.get()) {
            return nullptr;
        }

        auto nss = uncommittedPtr->ns();
        // If the collection is newly created, invariant on the collection being locked in MODE_IX.
        invariant(!newColl || opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX),
                  nss.toString());
        return uncommittedPtr.get();
    }

    std::shared_ptr<Collection> coll = _lookupCollectionByUUID(uuid);

    if (!coll || !coll->isCommitted())
        return nullptr;

    if (coll->ns().isOplog())
        return coll.get();

    invariant(opCtx->lockState()->isCollectionLockedForMode(coll->ns(), MODE_X));

    // Skip cloning and return directly if allowed.
    if (_alreadyClonedForBatchedWriter(coll)) {
        return coll.get();
    }

    auto cloned = coll->clone();
    auto ptr = cloned.get();

    // If we are in a batch write, set this Collection instance in the batched catalog write
    // instance. We don't want to store as uncommitted in this case as we need to observe the write
    // on the thread doing the batch write and it would trigger the regular path where we do a
    // copy-on-write on the catalog when committing.
    if (_isCatalogBatchWriter()) {
        // Do not update min valid timestamp in batched write as the write is not corresponding to
        // an oplog entry. If the write require an update to this timestamp it is the responsibility
        // of the user.
        PublishCatalogUpdates::setCollectionInCatalog(
            *batchedCatalogWriteInstance, std::move(cloned), boost::none);
        return ptr;
    }

    uncommittedCatalogUpdates.writableCollection(std::move(cloned));

    PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);

    return ptr;
}

CollectionPtr CollectionCatalog::lookupCollectionByUUID(OperationContext* opCtx, UUID uuid) const {
    // If UUID is managed by UncommittedCatalogUpdates (but not newly created) return the pointer
    // which will be nullptr in case of a drop.
    auto [found, uncommittedPtr, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, uuid);
    if (found) {
        return uncommittedPtr.get();
    }

    auto coll = _lookupCollectionByUUID(uuid);
    return (coll && coll->isCommitted())
        ? CollectionPtr(opCtx, coll.get(), LookupCollectionForYieldRestore(coll->ns()))
        : CollectionPtr();
}

bool CollectionCatalog::isCollectionAwaitingVisibility(UUID uuid) const {
    auto coll = _lookupCollectionByUUID(uuid);
    return coll && !coll->isCommitted();
}

std::shared_ptr<Collection> CollectionCatalog::_lookupCollectionByUUID(UUID uuid) const {
    auto foundIt = _catalog.find(uuid);
    return foundIt == _catalog.end() ? nullptr : foundIt->second;
}

std::shared_ptr<const Collection> CollectionCatalog::lookupCollectionByNamespaceForRead(
    OperationContext* opCtx, const NamespaceString& nss) const {

    auto [found, uncommittedColl, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, nss);
    if (uncommittedColl) {
        return uncommittedColl;
    }

    // Report the drop or rename as nothing new was created.
    if (found) {
        return nullptr;
    }

    auto it = _collections.find(nss);
    auto coll = (it == _collections.end() ? nullptr : it->second);
    return (coll && coll->isCommitted()) ? coll : nullptr;
}

Collection* CollectionCatalog::lookupCollectionByNamespaceForMetadataWrite(
    OperationContext* opCtx, const NamespaceString& nss) const {
    // Oplog is special and can only be modified in a few contexts. It is modified inplace and care
    // need to be taken for concurrency.
    if (nss.isOplog()) {
        return const_cast<Collection*>(lookupCollectionByNamespace(opCtx, nss).get());
    }

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    auto [found, uncommittedPtr, newColl] = UncommittedCatalogUpdates::lookupCollection(opCtx, nss);


    // If uncommittedPtr is valid, found is always true. Return the pointer as the collection still
    // exists.
    if (uncommittedPtr) {
        // If the collection is newly created, invariant on the collection being locked in MODE_IX.
        invariant(!newColl || opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX),
                  nss.toString());
        return uncommittedPtr.get();
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

    // Skip cloning and return directly if allowed.
    if (_alreadyClonedForBatchedWriter(coll)) {
        return coll.get();
    }

    auto cloned = coll->clone();
    auto ptr = cloned.get();

    // If we are in a batch write, set this Collection instance in the batched catalog write
    // instance. We don't want to store as uncommitted in this case as we need to observe the write
    // on the thread doing the batch write and it would trigger the regular path where we do a
    // copy-on-write on the catalog when committing.
    if (_isCatalogBatchWriter()) {
        // Do not update min valid timestamp in batched write as the write is not corresponding to
        // an oplog entry. If the write require an update to this timestamp it is the responsibility
        // of the user.
        PublishCatalogUpdates::setCollectionInCatalog(
            *batchedCatalogWriteInstance, std::move(cloned), boost::none);
        return ptr;
    }

    uncommittedCatalogUpdates.writableCollection(std::move(cloned));

    PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);

    return ptr;
}

CollectionPtr CollectionCatalog::lookupCollectionByNamespace(OperationContext* opCtx,
                                                             const NamespaceString& nss) const {
    // If uncommittedPtr is valid, found is always true. Return the pointer as the collection still
    // exists.
    auto [found, uncommittedPtr, newColl] = UncommittedCatalogUpdates::lookupCollection(opCtx, nss);
    if (uncommittedPtr) {
        return uncommittedPtr.get();
    }

    // Report the drop or rename as nothing new was created.
    if (found) {
        return nullptr;
    }

    auto it = _collections.find(nss);
    auto coll = (it == _collections.end() ? nullptr : it->second);
    return (coll && coll->isCommitted())
        ? CollectionPtr(opCtx, coll.get(), LookupCollectionForYieldRestore(coll->ns()))
        : nullptr;
}

boost::optional<NamespaceString> CollectionCatalog::lookupNSSByUUID(OperationContext* opCtx,
                                                                    const UUID& uuid) const {
    auto [found, uncommittedPtr, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, uuid);
    // If UUID is managed by uncommittedCatalogUpdates return its corresponding namespace if the
    // Collection exists, boost::none otherwise.
    if (found) {
        if (uncommittedPtr)
            return uncommittedPtr->ns();
        return boost::none;
    }

    auto foundIt = _catalog.find(uuid);
    if (foundIt != _catalog.end()) {
        boost::optional<NamespaceString> ns = foundIt->second->ns();
        invariant(!ns.value().isEmpty());
        return _collections.find(ns.value())->second->isCommitted() ? ns : boost::none;
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

boost::optional<UUID> CollectionCatalog::lookupUUIDByNSS(OperationContext* opCtx,
                                                         const NamespaceString& nss) const {
    auto [found, uncommittedPtr, newColl] = UncommittedCatalogUpdates::lookupCollection(opCtx, nss);
    if (uncommittedPtr) {
        return uncommittedPtr->uuid();
    }

    if (found) {
        return boost::none;
    }

    auto it = _collections.find(nss);
    if (it != _collections.end()) {
        const boost::optional<UUID>& uuid = it->second->uuid();
        return it->second->isCommitted() ? uuid : boost::none;
    }
    return boost::none;
}

boost::optional<RecordId> CollectionCatalog::lookupCatalogIdByNSS(
    const NamespaceString& nss, boost::optional<Timestamp> ts) const {
    if (auto it = _catalogIds.find(nss); it != _catalogIds.end()) {
        const auto& range = it->second;
        if (!ts) {
            return range.back().id;
        }

        auto rangeIt = std::upper_bound(
            range.begin(), range.end(), *ts, [](const auto& ts, const auto& entry) {
                return ts < entry.ts;
            });
        if (rangeIt != range.begin()) {
            return (--rangeIt)->id;
        }
    }
    return boost::none;
}

void CollectionCatalog::iterateViews(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const std::function<bool(const ViewDefinition& view)>& callback) const {
    auto viewsForDb = _getViewsForDatabase(opCtx, dbName);
    if (!viewsForDb) {
        return;
    }

    assertViewCatalogValid(*viewsForDb);
    viewsForDb->iterate(callback);
}

std::shared_ptr<const ViewDefinition> CollectionCatalog::lookupView(
    OperationContext* opCtx, const NamespaceString& ns) const {
    auto viewsForDb = _getViewsForDatabase(opCtx, ns.dbName());
    if (!viewsForDb) {
        return nullptr;
    }

    if (!viewsForDb->valid() && opCtx->getClient()->isFromUserConnection()) {
        // We want to avoid lookups on invalid collection names.
        if (!NamespaceString::validCollectionName(ns.ns())) {
            return nullptr;
        }

        // ApplyOps should work on a valid existing collection, despite the presence of bad views
        // otherwise the server would crash. The view catalog will remain invalid until the bad view
        // definitions are removed.
        assertViewCatalogValid(*viewsForDb);
    }

    return viewsForDb->lookup(ns);
}

std::shared_ptr<const ViewDefinition> CollectionCatalog::lookupViewWithoutValidatingDurable(
    OperationContext* opCtx, const NamespaceString& ns) const {
    auto viewsForDb = _getViewsForDatabase(opCtx, ns.dbName());
    if (!viewsForDb) {
        return nullptr;
    }

    return viewsForDb->lookup(ns);
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

bool CollectionCatalog::checkIfCollectionSatisfiable(UUID uuid, CollectionInfoFn predicate) const {
    invariant(predicate);

    auto collection = _lookupCollectionByUUID(uuid);

    if (!collection) {
        return false;
    }

    return predicate(collection.get());
}

std::vector<UUID> CollectionCatalog::getAllCollectionUUIDsFromDb(const DatabaseName& dbName) const {
    auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto it = _orderedCollections.lower_bound(std::make_pair(dbName, minUuid));

    std::vector<UUID> ret;
    while (it != _orderedCollections.end() && it->first.first == dbName) {
        if (it->second->isCommitted()) {
            ret.push_back(it->first.second);
        }
        ++it;
    }
    return ret;
}

std::vector<NamespaceString> CollectionCatalog::getAllCollectionNamesFromDb(
    OperationContext* opCtx, const DatabaseName& dbName) const {
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_S));

    auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();

    std::vector<NamespaceString> ret;
    for (auto it = _orderedCollections.lower_bound(std::make_pair(dbName, minUuid));
         it != _orderedCollections.end() && it->first.first == dbName;
         ++it) {
        if (it->second->isCommitted()) {
            ret.push_back(it->second->ns());
        }
    }
    return ret;
}

std::vector<DatabaseName> CollectionCatalog::_getAllDbNamesHelper(DatabaseName firstDbName) const {
    std::vector<DatabaseName> ret;
    auto maxUuid = UUID::parse("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF").getValue();
    // _orderedCollections is sorted by <dbName, uuid>. upper_bound will return the iterator to the
    // first element in _orderedCollections greater than <firstDbName, maxUuid>.
    auto iter = _orderedCollections.upper_bound(std::make_pair(firstDbName, maxUuid));
    while (iter != _orderedCollections.end()) {
        auto dbName = iter->first.first;
        if (firstDbName.tenantId() != boost::none && dbName.tenantId() != firstDbName.tenantId()) {
            break;
        }
        if (iter->second->isCommitted()) {
            ret.push_back(dbName);
        } else {
            // If the first collection found for `dbName` is not yet committed, increment the
            // iterator to find the next visible collection (possibly under a different
            // `dbName`).
            iter++;
            continue;
        }
        // Move on to the next database after `dbName`.
        iter = _orderedCollections.upper_bound(std::make_pair(dbName, maxUuid));
    }
    return ret;
}

std::vector<DatabaseName> CollectionCatalog::getAllDbNames() const {
    return _getAllDbNamesHelper(DatabaseName());
}

std::vector<DatabaseName> CollectionCatalog::getAllDbNamesForTenant(
    boost::optional<TenantId> tenantId) const {
    return _getAllDbNamesHelper(DatabaseName(tenantId, ""));
}

void CollectionCatalog::setDatabaseProfileSettings(
    const DatabaseName& dbName, CollectionCatalog::ProfileSettings newProfileSettings) {
    _databaseProfileSettings[dbName] = newProfileSettings;
}

CollectionCatalog::ProfileSettings CollectionCatalog::getDatabaseProfileSettings(
    const DatabaseName& dbName) const {
    auto it = _databaseProfileSettings.find(dbName);
    if (it != _databaseProfileSettings.end()) {
        return it->second;
    }

    return {serverGlobalParams.defaultProfile, ProfileFilter::getDefault()};
}

void CollectionCatalog::clearDatabaseProfileSettings(const DatabaseName& dbName) {
    _databaseProfileSettings.erase(dbName);
}

CollectionCatalog::Stats CollectionCatalog::getStats() const {
    return _stats;
}

boost::optional<ViewsForDatabase::Stats> CollectionCatalog::getViewStatsForDatabase(
    OperationContext* opCtx, const DatabaseName& dbName) const {
    auto viewsForDb = _getViewsForDatabase(opCtx, dbName);
    return viewsForDb ? boost::make_optional(viewsForDb->stats()) : boost::none;
}

CollectionCatalog::ViewCatalogSet CollectionCatalog::getViewCatalogDbNames(
    OperationContext* opCtx) const {
    ViewCatalogSet results;
    for (const auto& dbNameViewSetPair : _viewsForDatabase) {
        results.insert(dbNameViewSetPair.first);
    }

    return results;
}

void CollectionCatalog::registerCollection(OperationContext* opCtx,
                                           const UUID& uuid,
                                           std::shared_ptr<Collection> coll,
                                           boost::optional<Timestamp> commitTime) {
    invariant(opCtx->lockState()->isW());
    _registerCollection(opCtx, uuid, std::move(coll), /*twoPhase=*/false, commitTime);
}

void CollectionCatalog::registerCollectionTwoPhase(OperationContext* opCtx,
                                                   const UUID& uuid,
                                                   std::shared_ptr<Collection> coll,
                                                   boost::optional<Timestamp> commitTime) {
    _registerCollection(opCtx, uuid, std::move(coll), /*twoPhase=*/true, commitTime);
}

void CollectionCatalog::_registerCollection(OperationContext* opCtx,
                                            const UUID& uuid,
                                            std::shared_ptr<Collection> coll,
                                            bool twoPhase,
                                            boost::optional<Timestamp> commitTime) {
    auto nss = coll->ns();
    _ensureNamespaceDoesNotExist(opCtx, nss, NamespaceType::kAll);

    LOGV2_DEBUG(20280,
                1,
                "Registering collection {namespace} with UUID {uuid}",
                "Registering collection",
                logAttrs(nss),
                "uuid"_attr = uuid);

    auto dbIdPair = std::make_pair(nss.dbName(), uuid);

    // Make sure no entry related to this uuid.
    invariant(_catalog.find(uuid) == _catalog.end());
    invariant(_orderedCollections.find(dbIdPair) == _orderedCollections.end());

    _catalog[uuid] = coll;
    _collections[nss] = coll;
    _orderedCollections[dbIdPair] = coll;
    if (twoPhase) {
        _pendingCommitNamespaces[nss] = coll;
        _pendingCommitUUIDs[uuid] = coll;
    } else {
        _pendingCommitNamespaces.erase(nss);
        _pendingCommitUUIDs.erase(uuid);
    }

    if (commitTime && !commitTime->isNull()) {
        coll->setMinimumValidSnapshot(commitTime.value());
        _pushCatalogIdForNSS(nss, coll->getCatalogId(), commitTime);
    }


    if (!nss.isOnInternalDb() && !nss.isSystem()) {
        _stats.userCollections += 1;
        if (coll->isCapped()) {
            _stats.userCapped += 1;
        }
        if (coll->isClustered()) {
            _stats.userClustered += 1;
        }
    } else {
        _stats.internal += 1;
    }

    invariant(static_cast<size_t>(_stats.internal + _stats.userCollections) == _collections.size());

    auto& resourceCatalog = ResourceCatalog::get(opCtx->getServiceContext());
    resourceCatalog.add({RESOURCE_DATABASE, nss.dbName()}, nss.dbName());
    resourceCatalog.add({RESOURCE_COLLECTION, nss}, nss);

    if (!storageGlobalParams.repair && coll->ns().isSystemDotViews()) {
        auto [it, emplaced] = _viewsForDatabase.try_emplace(coll->ns().dbName());
        if (auto status = it->second.reload(opCtx, _lookupSystemViews(opCtx, coll->ns().dbName()));
            !status.isOK()) {
            LOGV2_WARNING_OPTIONS(20326,
                                  {logv2::LogTag::kStartupWarnings},
                                  "Unable to parse views; remove any invalid views from the "
                                  "collection to restore server functionality",
                                  "error"_attr = redact(status),
                                  logAttrs(coll->ns()));
        }
    }
}

std::shared_ptr<Collection> CollectionCatalog::deregisterCollection(
    OperationContext* opCtx,
    const UUID& uuid,
    bool isDropPending,
    boost::optional<Timestamp> commitTime) {
    invariant(_catalog.find(uuid) != _catalog.end());

    auto coll = std::move(_catalog[uuid]);
    auto ns = coll->ns();
    auto dbIdPair = std::make_pair(ns.dbName(), uuid);

    LOGV2_DEBUG(20281, 1, "Deregistering collection", logAttrs(ns), "uuid"_attr = uuid);

    // Make sure collection object exists.
    invariant(_collections.find(ns) != _collections.end());
    invariant(_orderedCollections.find(dbIdPair) != _orderedCollections.end());

    // TODO SERVER-68674: Remove feature flag check.
    if (feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCV() && isDropPending) {
        auto ident = coll->getSharedIdent()->getIdent();
        LOGV2_DEBUG(6825300, 1, "Registering drop pending collection ident", "ident"_attr = ident);

        auto it = _dropPendingCollection.find(ident);
        invariant(it == _dropPendingCollection.end());
        _dropPendingCollection[ident] = coll;
    }

    _orderedCollections.erase(dbIdPair);
    _collections.erase(ns);
    _catalog.erase(uuid);

    // Push drop unless this is a rollback of a create
    if (coll->isCommitted()) {
        _pushCatalogIdForNSS(ns, boost::none, commitTime);
    }

    if (!ns.isOnInternalDb() && !ns.isSystem()) {
        _stats.userCollections -= 1;
        if (coll->isCapped()) {
            _stats.userCapped -= 1;
        }
        if (coll->isClustered()) {
            _stats.userClustered -= 1;
        }
    } else {
        _stats.internal -= 1;
    }

    invariant(static_cast<size_t>(_stats.internal + _stats.userCollections) == _collections.size());

    coll->onDeregisterFromCatalog(opCtx);

    ResourceCatalog::get(opCtx->getServiceContext()).remove({RESOURCE_COLLECTION, ns}, ns);

    if (!storageGlobalParams.repair && coll->ns().isSystemDotViews()) {
        _viewsForDatabase.erase(coll->ns().dbName());
    }

    return coll;
}

void CollectionCatalog::registerUncommittedView(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(nss.dbName(), NamespaceString::kSystemDotViewsCollectionName), MODE_X));

    // Since writing to system.views requires an X lock, we only need to cross-check collection
    // namespaces here.
    _ensureNamespaceDoesNotExist(opCtx, nss, NamespaceType::kCollection);

    _uncommittedViews.emplace(nss);
}

void CollectionCatalog::deregisterUncommittedView(const NamespaceString& nss) {
    _uncommittedViews.erase(nss);
}

void CollectionCatalog::_ensureNamespaceDoesNotExist(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     NamespaceType type) const {
    auto existingCollection = _collections.find(nss);
    if (existingCollection != _collections.end()) {
        LOGV2(5725001,
              "Conflicted registering namespace, already have a collection with the same namespace",
              "nss"_attr = nss);
        throwWriteConflictException(str::stream() << "Collection namespace '" << nss.ns()
                                                  << "' is already in use.");
    }

    if (type == NamespaceType::kAll) {
        if (_uncommittedViews.contains(nss)) {
            LOGV2(5725002,
                  "Conflicted registering namespace, already have a view with the same namespace",
                  "nss"_attr = nss);
            throwWriteConflictException(str::stream() << "Collection namespace '" << nss.ns()
                                                      << "' is already in use.");
        }

        if (auto viewsForDb = _getViewsForDatabase(opCtx, nss.dbName())) {
            if (viewsForDb->lookup(nss) != nullptr) {
                LOGV2(
                    5725003,
                    "Conflicted registering namespace, already have a view with the same namespace",
                    "nss"_attr = nss);
                uasserted(ErrorCodes::NamespaceExists,
                          "Conflicted registering namespace, already have a view with the same "
                          "namespace");
            }
        }
    }
}

void CollectionCatalog::_pushCatalogIdForNSS(const NamespaceString& nss,
                                             boost::optional<RecordId> catalogId,
                                             boost::optional<Timestamp> ts) {
    // TODO SERVER-68674: Remove feature flag check.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCV()) {
        // No-op.
        return;
    }

    auto& ids = _catalogIds[nss];

    if (!ts) {
        // Make sure untimestamped writes have a single entry in mapping. If we're mixing
        // timestamped with untimestamped (such as repair). Ignore the untimestamped writes as an
        // untimestamped deregister will correspond with an untimestamped register. We should leave
        // the mapping as-is in this case.
        if (ids.empty() && catalogId) {
            // This namespace was added due to an untimestamped write, add an entry with min
            // timestamp
            ids.push_back(TimestampedCatalogId{catalogId, Timestamp::min()});
        } else if (ids.size() == 1 && !catalogId) {
            // This namespace was removed due to an untimestamped write, clear entries.
            ids.clear();
        }
        return;
    }

    // Re-write latest entry if timestamp match (multiple changes occured in this transaction)
    if (!ids.empty() && ids.back().ts == *ts) {
        ids.back().id = catalogId;
        return;
    }

    // Otherwise, push new entry at the end. Timestamp is always increasing
    invariant(ids.empty() || ids.back().ts < *ts);
    // If the catalogId is the same as last entry, there's nothing we need to do. This can happen
    // when the catalog is reopened.
    if (!ids.empty() && ids.back().id == catalogId) {
        return;
    }

    ids.push_back(TimestampedCatalogId{catalogId, *ts});
    _markNamespaceForCatalogIdCleanupIfNeeded(nss, ids);
}

void CollectionCatalog::_pushCatalogIdForRename(const NamespaceString& from,
                                                const NamespaceString& to,
                                                boost::optional<Timestamp> ts) {
    // TODO SERVER-68674: Remove feature flag check.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCV()) {
        // No-op.
        return;
    }

    if (!ts)
        return;

    // Get 'toIds' first, it may need to instantiate in the container which invalidates all
    // references.
    auto& toIds = _catalogIds[to];
    auto& fromIds = _catalogIds.at(from);
    invariant(!fromIds.empty());

    // Re-write latest entry if timestamp match (multiple changes occured in this transaction),
    // otherwise push at end
    if (!toIds.empty() && toIds.back().ts == *ts) {
        toIds.back().id = fromIds.back().id;
    } else {
        invariant(toIds.empty() || toIds.back().ts < *ts);
        toIds.push_back(TimestampedCatalogId{fromIds.back().id, *ts});
        _markNamespaceForCatalogIdCleanupIfNeeded(to, toIds);
    }

    // Re-write latest entry if timestamp match (multiple changes occured in this transaction),
    // otherwise push at end
    if (!fromIds.empty() && fromIds.back().ts == *ts) {
        fromIds.back().id = boost::none;
    } else {
        invariant(fromIds.empty() || fromIds.back().ts < *ts);
        fromIds.push_back(TimestampedCatalogId{boost::none, *ts});
        _markNamespaceForCatalogIdCleanupIfNeeded(from, fromIds);
    }
}

void CollectionCatalog::_markNamespaceForCatalogIdCleanupIfNeeded(
    const NamespaceString& nss, const std::vector<TimestampedCatalogId>& ids) {

    auto markForCleanup = [this, &nss](Timestamp ts) {
        _catalogIdChanges.insert(nss);
        if (ts < _lowestCatalogIdTimestampForCleanup) {
            _lowestCatalogIdTimestampForCleanup = ts;
        }
    };

    // Cleanup may occur if we have more than one entry for the namespace or if the only entry is a
    // drop. Use the first entry as lowest cleanup time if we have a drop and the second otherwise
    // (as the first is needed until that time is reached)
    if (!ids.empty() && ids.front().id == boost::none) {
        markForCleanup(ids.front().ts);
    } else if (ids.size() > 1) {
        markForCleanup(ids.at(1).ts);
    }
}

void CollectionCatalog::deregisterAllCollectionsAndViews(ServiceContext* svcCtx) {
    LOGV2(20282, "Deregistering all the collections");
    for (auto& entry : _catalog) {
        auto uuid = entry.first;
        auto ns = entry.second->ns();

        LOGV2_DEBUG(20283, 1, "Deregistering collection", logAttrs(ns), "uuid"_attr = uuid);

        entry.second.reset();
    }

    _collections.clear();
    _orderedCollections.clear();
    _catalog.clear();
    _viewsForDatabase.clear();
    _dropPendingCollection.clear();
    _dropPendingIndex.clear();
    _stats = {};

    ResourceCatalog::get(svcCtx).clear();
}

void CollectionCatalog::clearViews(OperationContext* opCtx, const DatabaseName& dbName) const {
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString(dbName, NamespaceString::kSystemDotViewsCollectionName), MODE_X));

    auto it = _viewsForDatabase.find(dbName);
    invariant(it != _viewsForDatabase.end());

    ViewsForDatabase viewsForDb = it->second;
    viewsForDb.clear(opCtx);

    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog._replaceViewsForDatabase(dbName, std::move(viewsForDb));
    });
}

void CollectionCatalog::deregisterIndex(OperationContext* opCtx,
                                        std::shared_ptr<IndexCatalogEntry> indexEntry,
                                        bool isDropPending) {
    // TODO SERVER-68674: Remove feature flag check.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCV() || !isDropPending) {
        // No-op.
        return;
    }

    // Unfinished index builds return a nullptr for getSharedIdent(). Use getIdent() instead.
    std::string ident = indexEntry->getIdent();

    auto it = _dropPendingIndex.find(ident);
    invariant(it == _dropPendingIndex.end());

    LOGV2_DEBUG(6825301, 1, "Registering drop pending index entry ident", "ident"_attr = ident);
    _dropPendingIndex[ident] = indexEntry;
}

void CollectionCatalog::notifyIdentDropped(const std::string& ident) {
    // It's possible that the ident doesn't exist in either map when the collection catalog is
    // re-opened, the _dropPendingIdent map is cleared. During rollback-to-stable we re-open the
    // collection catalog. The TimestampMonitor is a background thread that continues to run during
    // rollback-to-stable and maintains its own drop pending ident information. It generates a set
    // of drop pending idents outside of the global lock. However, during rollback-to-stable, we
    // clear the TimestampMonitors drop pending state. But it's possible that the TimestampMonitor
    // already generated a set of idents to drop for its next iteration, which would call into this
    // function, for idents we've already cleared from the collection catalogs in-memory state.
    LOGV2_DEBUG(6825302, 1, "Deregistering drop pending ident", "ident"_attr = ident);

    auto collIt = _dropPendingCollection.find(ident);
    if (collIt != _dropPendingCollection.end()) {
        _dropPendingCollection.erase(collIt);
        return;
    }

    auto indexIt = _dropPendingIndex.find(ident);
    if (indexIt != _dropPendingIndex.end()) {
        _dropPendingIndex.erase(indexIt);
        return;
    }
}

CollectionCatalog::iterator CollectionCatalog::begin(OperationContext* opCtx,
                                                     const DatabaseName& dbName) const {
    return iterator(opCtx, dbName, *this);
}

CollectionCatalog::iterator CollectionCatalog::end(OperationContext* opCtx) const {
    return iterator(opCtx, _orderedCollections.end(), *this);
}

bool CollectionCatalog::needsCleanupForOldestTimestamp(Timestamp oldest) const {
    // TODO SERVER-68674: Remove feature flag check.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCV()) {
        // No-op.
        return false;
    }

    return _lowestCatalogIdTimestampForCleanup <= oldest;
}

void CollectionCatalog::cleanupForOldestTimestampAdvanced(Timestamp oldest) {
    Timestamp nextLowestCleanupTimestamp = Timestamp::max();
    // Helper to calculate the smallest entry that needs to be kept and its timestamp
    auto assignLowestCleanupTimestamp = [&nextLowestCleanupTimestamp](const auto& range) {
        auto it = range.begin();
        // Drops can be cleaned up right away, otherwise the second entry is cleanup time.
        if (it->id.has_value()) {
            ++it;
        }
        nextLowestCleanupTimestamp = std::min(nextLowestCleanupTimestamp, it->ts);
    };

    // Iterate over all namespaces that is marked that they need cleanup
    for (auto it = _catalogIdChanges.begin(), end = _catalogIdChanges.end(); it != end;) {
        auto& range = _catalogIds[*it];

        // Binary search for next larger timestamp
        auto rangeIt = std::upper_bound(
            range.begin(), range.end(), oldest, [](const auto& ts, const auto& entry) {
                return ts < entry.ts;
            });

        // Continue if there is nothing to cleanup for this timestamp yet
        if (rangeIt == range.begin()) {
            assignLowestCleanupTimestamp(range);
            ++it;
            continue;
        }

        // The iterator is positioned to the closest entry that has a larger timestamp, decrement to
        // get a lower or equal timestamp
        --rangeIt;

        // If we are positioned on a drop it can be removed
        if (!rangeIt->id.has_value()) {
            ++rangeIt;
        }

        // Erase range
        range.erase(range.begin(), rangeIt);

        // If the range is now empty or we need to keep the last item we can unmark this namespace
        // for needing changes.
        if (range.size() <= 1) {
            _catalogIdChanges.erase(it++);
            continue;
        }

        // More changes are needed for this namespace, keep it in the set and keep track of lowest
        // timestamp.
        assignLowestCleanupTimestamp(range);
        ++it;
    }

    _lowestCatalogIdTimestampForCleanup = nextLowestCleanupTimestamp;
}

void CollectionCatalog::cleanupForCatalogReopen(Timestamp stable) {
    _catalogIdChanges.clear();
    _lowestCatalogIdTimestampForCleanup = Timestamp::max();

    for (auto it = _catalogIds.begin(); it != _catalogIds.end();) {
        auto& ids = it->second;

        // Remove all larger timestamps in this range
        ids.erase(std::upper_bound(ids.begin(),
                                   ids.end(),
                                   stable,
                                   [](Timestamp ts, const auto& entry) { return ts < entry.ts; }),
                  ids.end());

        // Remove namespace if there are no entries left
        if (ids.empty()) {
            _catalogIds.erase(it++);
            continue;
        }

        // Calculate when this namespace needs to be cleaned up next
        _markNamespaceForCatalogIdCleanupIfNeeded(it->first, ids);
        ++it;
    }
}

void CollectionCatalog::invariantHasExclusiveAccessToCollection(OperationContext* opCtx,
                                                                const NamespaceString& nss) {
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X) ||
                  (uncommittedCatalogUpdates.isCreatedCollection(opCtx, nss) &&
                   opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX)),
              nss.toString());
}

CollectionPtr CollectionCatalog::_lookupSystemViews(OperationContext* opCtx,
                                                    const DatabaseName& dbName) const {
    return lookupCollectionByNamespace(opCtx,
                                       {dbName, NamespaceString::kSystemDotViewsCollectionName});
}

boost::optional<const ViewsForDatabase&> CollectionCatalog::_getViewsForDatabase(
    OperationContext* opCtx, const DatabaseName& dbName) const {
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    auto uncommittedViews = uncommittedCatalogUpdates.getViewsForDatabase(dbName);
    if (uncommittedViews) {
        return uncommittedViews;
    }

    auto it = _viewsForDatabase.find(dbName);
    if (it == _viewsForDatabase.end()) {
        return boost::none;
    }
    return it->second;
}

void CollectionCatalog::_replaceViewsForDatabase(const DatabaseName& dbName,
                                                 ViewsForDatabase&& views) {
    _viewsForDatabase[dbName] = std::move(views);
}

bool CollectionCatalog::_isCatalogBatchWriter() const {
    return batchedCatalogWriteInstance.get() == this;
}

bool CollectionCatalog::_alreadyClonedForBatchedWriter(
    const std::shared_ptr<Collection>& collection) const {
    // We may skip cloning the Collection instance if and only if we are currently in a batched
    // catalog write and all references to this Collection is owned by the cloned CollectionCatalog
    // instance owned by the batch writer. i.e. the Collection is uniquely owned by the batch
    // writer. When the batch writer initially clones the catalog, all collections will have a
    // 'use_count' of at least kNumCollectionReferencesStored*2 (because there are at least 2
    // catalog instances). To check for uniquely owned we need to check that the reference count is
    // exactly kNumCollectionReferencesStored (owned by a single catalog) while also account for the
    // instance that is extracted from the catalog and provided as a parameter to this function, we
    // therefore need to add 1.
    return _isCatalogBatchWriter() && collection.use_count() == kNumCollectionReferencesStored + 1;
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
                                                              const UUID& uuid) const {
    auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByUUIDForRead(opCtx, uuid);

    // Collection dropped during yielding.
    if (!collection) {
        return nullptr;
    }

    // Collection renamed during yielding.
    // This check ensures that we are locked on the same namespace and that it is safe to return
    // the C-style pointer to the Collection.
    if (collection->ns() != _nss) {
        return nullptr;
    }

    // After yielding and reacquiring locks, the preconditions that were used to select our
    // ReadSource initially need to be checked again. We select a ReadSource based on replication
    // state. After a query yields its locks, the replication state may have changed, invalidating
    // our current choice of ReadSource. Using the same preconditions, change our ReadSource if
    // necessary.
    SnapshotHelper::changeReadSourceIfNeeded(opCtx, collection->ns());

    return collection.get();
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
    _batchedInstance = batchedCatalogWriteInstance.get();
}
BatchedCollectionCatalogWriter::~BatchedCollectionCatalogWriter() {
    invariant(_opCtx->lockState()->isW());
    invariant(_batchedInstance == batchedCatalogWriteInstance.get());

    // Publish out batched instance, validate that no other writers have been able to write during
    // the batcher.
    auto& storage = getCatalog(_opCtx->getServiceContext());
    invariant(
        atomic_compare_exchange_strong(&storage.catalog, &_base, batchedCatalogWriteInstance));

    // Clear out batched pointer so no more attempts of batching are made
    _batchedInstance = nullptr;
    batchedCatalogWriteInstance = nullptr;
}

}  // namespace mongo
