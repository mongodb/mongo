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
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/resource_catalog.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {
// Sentinel id for marking a catalogId mapping range as unknown. Must use an invalid RecordId.
static RecordId kUnknownRangeMarkerId = RecordId::minLong();
// Maximum number of entries in catalogId mapping when inserting catalogId missing at timestamp.
// Used to avoid quadratic behavior when inserting entries at the beginning. When threshold is
// reached we will fall back to more durable catalog scans.
static constexpr int kMaxCatalogIdMappingLengthForMissingInsert = 1000;

constexpr auto kNumDurableCatalogScansDueToMissingMapping = "numScansDueToMissingMapping"_sd;

struct LatestCollectionCatalog {
    std::shared_ptr<CollectionCatalog> catalog = std::make_shared<CollectionCatalog>();
};
const ServiceContext::Decoration<LatestCollectionCatalog> getCatalog =
    ServiceContext::declareDecoration<LatestCollectionCatalog>();

// Catalog instance for batched write when ongoing. The atomic bool is used to determine if a
// batched write is ongoing without having to take locks.
std::shared_ptr<CollectionCatalog> batchedCatalogWriteInstance;
AtomicWord<bool> ongoingBatchedWrite{false};

const RecoveryUnit::Snapshot::Decoration<std::shared_ptr<const CollectionCatalog>> stashedCatalog =
    RecoveryUnit::Snapshot::declareDecoration<std::shared_ptr<const CollectionCatalog>>();

/**
 * Returns true if the collection is compatible with the read timestamp.
 */
bool isExistingCollectionCompatible(std::shared_ptr<Collection> coll,
                                    boost::optional<Timestamp> readTimestamp) {
    if (!coll || !readTimestamp) {
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

const auto maxUuid = UUID::parse("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF").getValue();
const auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();


// Copy existing value from immutable data structure or default-construct if not existing
template <typename Container, typename Key>
auto copyIfExists(const Container& container, const Key& key) {
    const auto* value = container.find(key);
    if (value) {
        return *value;
    }
    return typename Container::mapped_type();
}

}  // namespace

/**
 * Defines a new serverStatus section "collectionCatalog".
 */
class CollectionCatalogSection final : public ServerStatusSection {
public:
    CollectionCatalogSection() : ServerStatusSection("collectionCatalog") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        BSONObjBuilder section;
        section.append(kNumDurableCatalogScansDueToMissingMapping,
                       numScansDueToMissingMapping.loadRelaxed());
        return section.obj();
    }

    AtomicWord<long long> numScansDueToMissingMapping;
} gCollectionCatalogSection;

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

        catalog._collections = catalog._collections.set(collection->ns(), collection);
        catalog._catalog = catalog._catalog.set(collection->uuid(), collection);
        auto dbIdPair = std::make_pair(collection->ns().dbName(), collection->uuid());
        catalog._orderedCollections[dbIdPair] = collection;

        catalog._pendingCommitNamespaces = catalog._pendingCommitNamespaces.erase(collection->ns());
        catalog._pendingCommitUUIDs = catalog._pendingCommitUUIDs.erase(collection->uuid());
    }

    PublishCatalogUpdates(UncommittedCatalogUpdates& uncommittedCatalogUpdates)
        : _uncommittedCatalogUpdates(uncommittedCatalogUpdates) {}

    static void ensureRegisteredWithRecoveryUnit(
        OperationContext* opCtx, UncommittedCatalogUpdates& uncommittedCatalogUpdates) {
        if (opCtx->recoveryUnit()->hasRegisteredChangeForCatalogVisibility())
            return;
        // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
        if (feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe()) {
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
                catalog._pendingCommitNamespaces =
                    catalog._pendingCommitNamespaces.set(entry.nss, entry.collection);

                if (entry.collection) {
                    // If we have a collection instance for this entry also mark the uuid as pending
                    catalog._pendingCommitUUIDs =
                        catalog._pendingCommitUUIDs.set(entry.collection->uuid(), entry.collection);
                } else if (entry.externalUUID) {
                    // Drops do not have a collection instance but set their UUID in the entry. Mark
                    // it as pending with no collection instance.
                    catalog._pendingCommitUUIDs =
                        catalog._pendingCommitUUIDs.set(*entry.externalUUID, nullptr);
                }
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
                        catalog._collections = catalog._collections.erase(from);
                        catalog._pendingCommitNamespaces =
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
                    writeJobs.push_back(
                        [coll = entry.collection.get(), commitTime](CollectionCatalog& catalog) {
                            if (commitTime) {
                                coll->setMinimumVisibleSnapshot(commitTime.value());
                                coll->setMinimumValidSnapshot(commitTime.value());
                            }
                            catalog._pushCatalogIdForNSSAndUUID(
                                coll->ns(), coll->uuid(), coll->getCatalogId(), commitTime);

                            catalog._pendingCommitNamespaces =
                                catalog._pendingCommitNamespaces.erase(coll->ns());
                            catalog._pendingCommitUUIDs =
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
        // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
        if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe())
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

                catalog._pendingCommitNamespaces =
                    catalog._pendingCommitNamespaces.erase(entry.nss);

                // Entry without collection, nothing more to do
                if (!entry.collection)
                    continue;

                catalog._pendingCommitUUIDs =
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
        return nullptr;
    }

    return _mapIter->second.get();
}

UUID CollectionCatalog::iterator::uuid() const {
    invariant(_uuid);
    return *_uuid;
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

std::shared_ptr<const CollectionCatalog> CollectionCatalog::latest(ServiceContext* svcCtx) {
    return atomic_load(&getCatalog(svcCtx).catalog);
}

std::shared_ptr<const CollectionCatalog> CollectionCatalog::get(OperationContext* opCtx) {
    const auto& stashed = stashedCatalog(opCtx->recoveryUnit()->getSnapshot());
    if (stashed)
        return stashed;

    return latest(opCtx);
}

std::shared_ptr<const CollectionCatalog> CollectionCatalog::latest(OperationContext* opCtx) {
    // If there is a batched catalog write ongoing and we are the one doing it return this instance
    // so we can observe our own writes. There may be other callers that reads the CollectionCatalog
    // without any locks, they must see the immutable regular instance. We can do a relaxed load
    // here because the value only matters for the thread that set it. The wrong value for other
    // threads always results in the non-batched write branch to be taken. Futhermore, the second
    // part of the condition is checking the lock state will give us sequentially consistent
    // ordering.
    if (ongoingBatchedWrite.loadRelaxed() && opCtx->lockState()->isW()) {
        return batchedCatalogWriteInstance;
    }

    return latest(opCtx->getServiceContext());
}

void CollectionCatalog::stash(OperationContext* opCtx,
                              std::shared_ptr<const CollectionCatalog> catalog) {
    stashedCatalog(opCtx->recoveryUnit()->getSnapshot()) = std::move(catalog);
}

void CollectionCatalog::write(ServiceContext* svcCtx, CatalogWriteFn job) {
    // We should never have ongoing batching here. When batching is in progress the caller should
    // use the overload with OperationContext so we can verify that the global exlusive lock is
    // being held.
    invariant(!ongoingBatchedWrite.load());

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
    // 'ongoingBatchedWrite' and 'batchedCatalogWriteInstance' are set. Make sure we are the one
    // holding the write lock.
    if (ongoingBatchedWrite.load()) {
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
        NamespaceString::makeSystemDotViewsNamespace(viewName.dbName()), MODE_X));

    invariant(_viewsForDatabase.find(viewName.dbName()));
    const ViewsForDatabase& viewsForDb = *_getViewsForDatabase(opCtx, viewName.dbName());

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    if (uncommittedCatalogUpdates.shouldIgnoreExternalViewChanges(viewName.dbName())) {
        return Status::OK();
    }

    if (viewName.db() != viewOn.db())
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    if (viewsForDb.lookup(viewName) || _collections.find(viewName))
        return Status(ErrorCodes::NamespaceExists, "Namespace already exists");

    if (!NamespaceString::validCollectionName(viewOn.coll()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid name for 'viewOn': " << viewOn.coll());

    IgnoreExternalViewChangesForDatabase ignore(opCtx, viewName.dbName());

    assertViewCatalogValid(viewsForDb);
    CollectionPtr systemViews(_lookupSystemViews(opCtx, viewName.dbName()));

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
        NamespaceString::makeSystemDotViewsNamespace(viewName.dbName()), MODE_X));
    invariant(_viewsForDatabase.find(viewName.dbName()));
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
                                  CollectionPtr(systemViews),
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
        NamespaceString::makeSystemDotViewsNamespace(viewName.dbName()), MODE_X));
    invariant(_viewsForDatabase.find(viewName.dbName()));
    const ViewsForDatabase& viewsForDb = *_getViewsForDatabase(opCtx, viewName.dbName());
    assertViewCatalogValid(viewsForDb);
    if (!viewsForDb.lookup(viewName)) {
        return Status::OK();
    }

    Status result = Status::OK();
    {
        IgnoreExternalViewChangesForDatabase ignore(opCtx, viewName.dbName());

        CollectionPtr systemViews(_lookupSystemViews(opCtx, viewName.dbName()));

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

void CollectionCatalog::reloadViews(OperationContext* opCtx, const DatabaseName& dbName) const {
    // Two-phase locking ensures that all locks are held while a Change's commit() or
    // rollback()function runs, for thread saftey. And, MODE_X locks always opt for two-phase
    // locking.
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString::makeSystemDotViewsNamespace(dbName), MODE_X));

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    if (uncommittedCatalogUpdates.shouldIgnoreExternalViewChanges(dbName)) {
        return;
    }

    LOGV2_DEBUG(22546, 1, "Reloading view catalog for database", logAttrs(dbName));

    ViewsForDatabase viewsForDb;
    auto status = viewsForDb.reload(opCtx, CollectionPtr(_lookupSystemViews(opCtx, dbName)));
    if (!status.isOK()) {
        // If we encountered an error while reloading views, then the 'viewsForDb' variable will be
        // empty, and marked invalid. Any further operations that attempt to use a view will fail
        // until the view catalog is fixed. Most of the time, this means the system.views collection
        // needs to be dropped.
        //
        // Unfortunately, we don't have a good way to respond to this error, as when we're calling
        // this function, we're in an op observer, and we expect the operation to succeed once it's
        // gotten to that point since it's passed all our other checks. Instead, we can log this
        // information to aid in diagnosing the problem.
        LOGV2(7267300,
              "Encountered an error while reloading the view catalog",
              "error"_attr = status);
    }

    uncommittedCatalogUpdates.replaceViewsForDatabase(dbName, std::move(viewsForDb));
    PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);
}

const Collection* CollectionCatalog::establishConsistentCollection(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nssOrUUID,
    boost::optional<Timestamp> readTimestamp) const {
    if (_needsOpenCollection(opCtx, nssOrUUID, readTimestamp)) {
        return _openCollection(opCtx, nssOrUUID, readTimestamp);
    }

    return lookupCollectionByNamespaceOrUUID(opCtx, nssOrUUID);
}

bool CollectionCatalog::_needsOpenCollection(OperationContext* opCtx,
                                             const NamespaceStringOrUUID& nsOrUUID,
                                             boost::optional<Timestamp> readTimestamp) const {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe()) {
        return false;
    }

    // Don't need to open the collection if it was already previously instantiated.
    if (nsOrUUID.nss()) {
        if (OpenedCollections::get(opCtx).lookupByNamespace(*nsOrUUID.nss())) {
            return false;
        }
    } else {
        if (OpenedCollections::get(opCtx).lookupByUUID(*nsOrUUID.uuid())) {
            return false;
        }
    }

    if (readTimestamp) {
        auto coll = lookupCollectionByNamespaceOrUUID(opCtx, nsOrUUID);
        return !coll || *readTimestamp < coll->getMinimumValidSnapshot();
    } else {
        if (nsOrUUID.nss()) {
            return _pendingCommitNamespaces.find(*nsOrUUID.nss());
        } else {
            return _pendingCommitUUIDs.find(*nsOrUUID.uuid());
        }
    }
}

const Collection* CollectionCatalog::_openCollection(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nssOrUUID,
    boost::optional<Timestamp> readTimestamp) const {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe()) {
        return nullptr;
    }

    // The implementation of openCollection() is quite different at a timestamp compared to at
    // latest. Separated the implementation into helper functions and we call the right one
    // depending on the input parameters.
    if (!readTimestamp) {
        return _openCollectionAtLatestByNamespaceOrUUID(opCtx, nssOrUUID);
    }

    return _openCollectionAtPointInTimeByNamespaceOrUUID(opCtx, nssOrUUID, *readTimestamp);
}

const Collection* CollectionCatalog::_openCollectionAtLatestByNamespaceOrUUID(
    OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) const {
    auto& openedCollections = OpenedCollections::get(opCtx);

    // When openCollection is called with no timestamp, the namespace must be pending commit. We
    // compare the collection instance in _pendingCommitNamespaces and the collection instance in
    // the in-memory catalog with the durable catalog entry to determine which instance to return.
    const auto& pendingCollection = [&]() -> std::shared_ptr<Collection> {
        if (const auto& nss = nssOrUUID.nss()) {
            const std::shared_ptr<Collection>* pending = _pendingCommitNamespaces.find(*nss);
            invariant(pending);
            return *pending;
        }

        const std::shared_ptr<Collection>* pending = _pendingCommitUUIDs.find(*nssOrUUID.uuid());
        invariant(pending);
        return *pending;
    }();

    auto latestCollection = [&]() -> std::shared_ptr<const Collection> {
        if (const auto& nss = nssOrUUID.nss()) {
            return _getCollectionByNamespace(opCtx, *nss);
        }
        return _getCollectionByUUID(opCtx, *nssOrUUID.uuid());
    }();

    // At least one of latest and pending should be a valid pointer.
    invariant(latestCollection || pendingCollection);

    const RecordId catalogId = [&]() {
        if (pendingCollection) {
            return pendingCollection->getCatalogId();
        }

        // If pendingCollection is nullptr then it is a concurrent drop and the uuid should exist at
        // latest.
        return latestCollection->getCatalogId();
    }();

    auto catalogEntry = DurableCatalog::get(opCtx)->getCatalogEntry(opCtx, catalogId);

    const NamespaceString& nss = [&]() {
        if (auto nss = nssOrUUID.nss()) {
            return *nss;
        }
        return latestCollection ? latestCollection->ns() : pendingCollection->ns();
    }();

    const UUID uuid = [&]() {
        if (auto uuid = nssOrUUID.uuid()) {
            return *uuid;
        }

        // If pendingCollection is nullptr, the collection is being dropped, so latestCollection
        // must be non-nullptr and must contain a uuid.
        return pendingCollection ? pendingCollection->uuid() : latestCollection->uuid();
    }();

    // If the catalog entry is not found in our snapshot then the collection is being dropped and we
    // can observe the drop. Lookups by this namespace or uuid should not find a collection.
    if (catalogEntry.isEmpty()) {
        // If we performed this lookup by UUID we could be in a case where we're looking up
        // concurrently with a rename with dropTarget=true where the UUID that we use is the target
        // that got dropped. If that rename has committed we need to put the correct collection
        // under open collection for this namespace. We can detect this case by comparing the
        // catalogId with what is pending for this namespace.
        if (nssOrUUID.uuid()) {
            const std::shared_ptr<Collection>& pending = *_pendingCommitNamespaces.find(nss);
            if (pending && pending->getCatalogId() != catalogId) {
                openedCollections.store(nullptr, boost::none, uuid);
                openedCollections.store(pending, nss, pending->uuid());
                return nullptr;
            }
        }
        openedCollections.store(nullptr, nss, uuid);
        return nullptr;
    }

    // When trying to open the latest collection by namespace and the catalog entry has a different
    // namespace in our snapshot, then there is a rename operation concurrent with this call.
    NamespaceString nsInDurableCatalog = DurableCatalog::getNamespaceFromCatalogEntry(catalogEntry);
    if (nssOrUUID.nss() && nss != nsInDurableCatalog) {
        // There are two types of rename depending on the dropTarget flag.
        if (pendingCollection && latestCollection &&
            pendingCollection->getCatalogId() != latestCollection->getCatalogId()) {
            // When there is a rename with dropTarget=true the two possible choices for the
            // collection we need to observe are different logical collections, they have different
            // UUID and catalogId. In this case storing a single entry in open collections is
            // sufficient. We know that the instance we are looking for must be under
            // 'latestCollection' as we used the catalogId from 'pendingCollection' when fetching
            // durable catalog entry and the namespace in it did not match the namespace for
            // 'pendingCollection' (the rename has not been comitted yet)
            openedCollections.store(latestCollection, nss, latestCollection->uuid());
            return latestCollection.get();
        }

        // For a regular rename of the same logical collection with dropTarget=false have the same
        // UUID and catalogId for the two choices. In this case we need to store entries under open
        // collections for two namespaces (rename 'from' and 'to') so we can make sure lookups by
        // UUID is supported and will return a Collection with its namespace in sync with the
        // storage snapshot. Like above, the correct instance is either in the catalog or under
        // pending. First lookup in pending by UUID to determine if it contains the right namespace.
        const std::shared_ptr<Collection>* pending = _pendingCommitUUIDs.find(uuid);
        invariant(pending);
        const auto& pendingCollectionByUUID = *pending;
        if (pendingCollectionByUUID->ns() == nsInDurableCatalog) {
            openedCollections.store(pendingCollectionByUUID, pendingCollectionByUUID->ns(), uuid);
        } else {
            // If pending by UUID does not contain the right namespace, a regular lookup in
            // the catalog by UUID should have it.
            auto latestCollectionByUUID = _getCollectionByUUID(opCtx, uuid);
            invariant(latestCollectionByUUID && latestCollectionByUUID->ns() == nsInDurableCatalog);
            openedCollections.store(latestCollectionByUUID, latestCollectionByUUID->ns(), uuid);
        }

        // Last, mark 'nss' as not existing
        openedCollections.store(nullptr, nss, boost::none);
        return nullptr;
    }

    // When trying to open the latest collection by UUID and the Collection instances has different
    // namespaces, then there is a rename operation concurrent with this call. We need to store
    // entries under uncommitted catalog changes for two namespaces (rename 'from' and 'to') so we
    // can make sure lookups by UUID is supported and will return a Collection with its namespace in
    // sync with the storage snapshot.
    if (nssOrUUID.uuid() && latestCollection && pendingCollection &&
        latestCollection->ns() != pendingCollection->ns()) {
        if (latestCollection->ns() == nsInDurableCatalog) {
            // If this is a rename with dropTarget=true and we're looking up with the 'from' UUID
            // before the rename committed, the namespace would correspond to a valid collection
            // that we need to store under open collections.
            auto latestCollectionByNamespace =
                _getCollectionByNamespace(opCtx, pendingCollection->ns());
            if (latestCollectionByNamespace) {
                openedCollections.store(latestCollectionByNamespace,
                                        latestCollectionByNamespace->ns(),
                                        latestCollectionByNamespace->uuid());
            } else {
                openedCollections.store(nullptr, pendingCollection->ns(), boost::none);
            }
            openedCollections.store(latestCollection, nsInDurableCatalog, uuid);
            return latestCollection.get();
        } else {
            invariant(pendingCollection->ns() == nsInDurableCatalog);
            openedCollections.store(nullptr, latestCollection->ns(), boost::none);
            openedCollections.store(pendingCollection, nsInDurableCatalog, uuid);
            return pendingCollection.get();
        }
    }

    auto metadata = DurableCatalog::getMetadataFromCatalogEntry(catalogEntry);

    if (latestCollection && latestCollection->isMetadataEqual(metadata)) {
        openedCollections.store(latestCollection, nss, uuid);
        return latestCollection.get();
    }

    // Use the pendingCollection if there is no latestCollection or if the metadata of the
    // latestCollection doesn't match the durable catalogEntry.
    if (pendingCollection && pendingCollection->isMetadataEqual(metadata)) {
        // If the latest collection doesn't exist then the pending collection must exist as it's
        // being created in this snapshot. Otherwise, if the latest collection is incompatible
        // with this snapshot, then the change came from an uncommitted update by an operation
        // operating on this snapshot.
        openedCollections.store(pendingCollection, nss, uuid);
        return pendingCollection.get();
    }

    // If neither `latestCollection` or `pendingCollection` match the metadata we fully instantiate
    // a new collection instance from durable storage that is guaranteed to match. This can happen
    // when multikey is not consistent with the storage snapshot.
    invariant(latestCollection || pendingCollection);
    auto durableCatalogEntry = DurableCatalog::get(opCtx)->getParsedCatalogEntry(opCtx, catalogId);
    invariant(durableCatalogEntry);
    auto compatibleCollection =
        _createCompatibleCollection(opCtx,
                                    latestCollection ? latestCollection : pendingCollection,
                                    /*readTimestamp=*/boost::none,
                                    durableCatalogEntry.get());

    // This may nullptr if the collection was not instantiated successfully. This is the case when
    // timestamps aren't used (e.g. standalone mode) even though the durable catalog entry was
    // found. When timestamps aren't used, the drop pending reaper immediately drops idents which
    // may be needed to instantiate this collection.
    openedCollections.store(compatibleCollection, nss, uuid);
    return compatibleCollection.get();
}

const Collection* CollectionCatalog::_openCollectionAtPointInTimeByNamespaceOrUUID(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nssOrUUID,
    Timestamp readTimestamp) const {
    auto& openedCollections = OpenedCollections::get(opCtx);

    // Try to find a catalog entry matching 'readTimestamp'.
    auto catalogEntry = _fetchPITCatalogEntry(opCtx, nssOrUUID, readTimestamp);
    if (!catalogEntry) {
        openedCollections.store(nullptr, nssOrUUID.nss(), nssOrUUID.uuid());
        return nullptr;
    }

    auto latestCollection = _lookupCollectionByUUID(*catalogEntry->metadata->options.uuid);

    // Return the in-memory Collection instance if it is compatible with the read timestamp.
    if (isExistingCollectionCompatible(latestCollection, readTimestamp)) {
        openedCollections.store(latestCollection, latestCollection->ns(), latestCollection->uuid());
        return latestCollection.get();
    }

    // Use the shared collection state from the latest Collection in the in-memory collection
    // catalog if it is compatible.
    auto compatibleCollection =
        _createCompatibleCollection(opCtx, latestCollection, readTimestamp, catalogEntry.get());
    if (compatibleCollection) {
        openedCollections.store(
            compatibleCollection, compatibleCollection->ns(), compatibleCollection->uuid());
        return compatibleCollection.get();
    }

    // There is no state in-memory that matches the catalog entry. Try to instantiate a new
    // Collection instance from scratch.
    auto newCollection = _createNewPITCollection(opCtx, readTimestamp, catalogEntry.get());
    if (newCollection) {
        openedCollections.store(newCollection, newCollection->ns(), newCollection->uuid());
        return newCollection.get();
    }

    openedCollections.store(nullptr, nssOrUUID.nss(), nssOrUUID.uuid());
    return nullptr;
}

CollectionCatalog::CatalogIdLookup CollectionCatalog::_checkWithOldestCatalogIdTimestampMaintained(
    boost::optional<Timestamp> ts) const {
    // If the request was with a time prior to the oldest maintained time it is unknown, otherwise
    // we know it is not existing.
    return {RecordId{},
            ts && *ts < _oldestCatalogIdTimestampMaintained
                ? CollectionCatalog::CatalogIdLookup::Existence::kUnknown
                : CollectionCatalog::CatalogIdLookup::Existence::kNotExists};
}

CollectionCatalog::CatalogIdLookup CollectionCatalog::_findCatalogIdInRange(
    boost::optional<Timestamp> ts, const std::vector<TimestampedCatalogId>& range) const {
    if (!ts) {
        auto catalogId = range.back().id;
        if (catalogId) {
            return {*catalogId, CatalogIdLookup::Existence::kExists};
        }
        return {RecordId{}, CatalogIdLookup::Existence::kNotExists};
    }

    auto rangeIt =
        std::upper_bound(range.begin(), range.end(), *ts, [](const auto& ts, const auto& entry) {
            return ts < entry.ts;
        });
    if (rangeIt == range.begin()) {
        return _checkWithOldestCatalogIdTimestampMaintained(ts);
    }
    // Upper bound returns an iterator to the first entry with a larger timestamp. Decrement the
    // iterator to get the last entry where the time is less or equal.
    auto catalogId = (--rangeIt)->id;
    if (catalogId) {
        if (*catalogId != kUnknownRangeMarkerId) {
            return {*catalogId, CatalogIdLookup::Existence::kExists};
        } else {
            return {RecordId{}, CatalogIdLookup::Existence::kUnknown};
        }
    }
    return {RecordId{}, CatalogIdLookup::Existence::kNotExists};
}

boost::optional<DurableCatalogEntry> CollectionCatalog::_fetchPITCatalogEntry(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nssOrUUID,
    boost::optional<Timestamp> readTimestamp) const {
    auto [catalogId, result] = nssOrUUID.nss()
        ? lookupCatalogIdByNSS(*nssOrUUID.nss(), readTimestamp)
        : lookupCatalogIdByUUID(*nssOrUUID.uuid(), readTimestamp);
    if (result == CatalogIdLookup::Existence::kNotExists) {
        return boost::none;
    }

    auto writeCatalogIdAfterScan = [&](const boost::optional<DurableCatalogEntry>& catalogEntry) {
        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            // Convert from 'const boost::optional<NamespaceString>&' to 'boost::optional<const
            // NamespaceString&>' without copy.
            auto nss = [&]() -> boost::optional<const NamespaceString&> {
                if (const boost::optional<NamespaceString>& ns = nssOrUUID.nss())
                    return ns.value();
                return boost::none;
            }();
            // Insert catalogId for both the namespace and UUID if the catalog entry is found.
            catalog._insertCatalogIdForNSSAndUUIDAfterScan(
                catalogEntry ? catalogEntry->metadata->nss : nss,
                catalogEntry ? catalogEntry->metadata->options.uuid : nssOrUUID.uuid(),
                catalogEntry ? boost::make_optional(catalogEntry->catalogId) : boost::none,
                *readTimestamp);
        });
    };

    if (result == CatalogIdLookup::Existence::kUnknown) {
        // We shouldn't receive kUnknown when we don't have a timestamp since no timestamp means
        // we're operating on the latest.
        invariant(readTimestamp);

        // Scan durable catalog when we don't have accurate catalogId mapping for this timestamp.
        gCollectionCatalogSection.numScansDueToMissingMapping.fetchAndAdd(1);
        auto catalogEntry = nssOrUUID.nss()
            ? DurableCatalog::get(opCtx)->scanForCatalogEntryByNss(opCtx, *nssOrUUID.nss())
            : DurableCatalog::get(opCtx)->scanForCatalogEntryByUUID(opCtx, *nssOrUUID.uuid());
        writeCatalogIdAfterScan(catalogEntry);
        return catalogEntry;
    }

    auto catalogEntry = DurableCatalog::get(opCtx)->getParsedCatalogEntry(opCtx, catalogId);
    if (const auto& nss = nssOrUUID.nss();
        !catalogEntry || (nss && nss != catalogEntry->metadata->nss)) {
        invariant(readTimestamp);
        // If no entry is found or the entry contains a different namespace, the mapping might be
        // incorrect since it is incomplete after startup; scans durable catalog to confirm.
        auto catalogEntry = nss
            ? DurableCatalog::get(opCtx)->scanForCatalogEntryByNss(opCtx, *nss)
            : DurableCatalog::get(opCtx)->scanForCatalogEntryByUUID(opCtx, *nssOrUUID.uuid());
        writeCatalogIdAfterScan(catalogEntry);
        return catalogEntry;
    }
    return catalogEntry;
}

std::shared_ptr<Collection> CollectionCatalog::_createCompatibleCollection(
    OperationContext* opCtx,
    const std::shared_ptr<const Collection>& latestCollection,
    boost::optional<Timestamp> readTimestamp,
    const DurableCatalogEntry& catalogEntry) const {
    // Check if the collection is drop pending, not expired, and compatible with the read timestamp.
    std::shared_ptr<Collection> dropPendingColl = [&]() -> std::shared_ptr<Collection> {
        const std::weak_ptr<Collection>* dropPending =
            _dropPendingCollection.find(catalogEntry.ident);
        if (!dropPending) {
            return nullptr;
        }
        return dropPending->lock();
    }();

    if (isExistingCollectionCompatible(dropPendingColl, readTimestamp)) {
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
        Status status =
            collToReturn->initFromExisting(opCtx,
                                           latestCollection ? latestCollection : dropPendingColl,
                                           catalogEntry,
                                           readTimestamp);
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
    boost::optional<Timestamp> readTimestamp,
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
    Status status =
        collToReturn->initFromExisting(opCtx, /*collection=*/nullptr, catalogEntry, readTimestamp);
    if (!status.isOK()) {
        LOGV2_DEBUG(
            6857102, 1, "Failed to instantiate collection", "reason"_attr = status.reason());
        return nullptr;
    }

    return collToReturn;
}

std::shared_ptr<IndexCatalogEntry> CollectionCatalog::findDropPendingIndex(StringData ident) const {
    const std::weak_ptr<IndexCatalogEntry>* dropPending = _dropPendingIndex.find(ident);
    if (!dropPending) {
        return nullptr;
    }

    return dropPending->lock();
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
    _viewsForDatabase = _viewsForDatabase.erase(dbName);
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

std::shared_ptr<const Collection> CollectionCatalog::_getCollectionByUUID(OperationContext* opCtx,
                                                                          const UUID& uuid) const {
    // It's important to look in UncommittedCatalogUpdates before OpenedCollections because in a
    // multi-document transaction it's permitted to perform a lookup on a non-existent
    // collection followed by creating the collection. This lookup will store a nullptr in
    // OpenedCollections.
    auto [found, uncommittedColl, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, uuid);
    if (uncommittedColl) {
        return uncommittedColl;
    }

    // Return any previously instantiated collection on this namespace for this snapshot
    if (auto openedColl = OpenedCollections::get(opCtx).lookupByUUID(uuid)) {
        return openedColl.value();
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

const Collection* CollectionCatalog::lookupCollectionByUUID(OperationContext* opCtx,
                                                            UUID uuid) const {
    // If UUID is managed by UncommittedCatalogUpdates (but not newly created) return the pointer
    // which will be nullptr in case of a drop. It's important to look in UncommittedCatalogUpdates
    // before OpenedCollections because in a multi-document transaction it's permitted to perform a
    // lookup on a non-existent collection followed by creating the collection. This lookup will
    // store a nullptr in OpenedCollections.
    auto [found, uncommittedPtr, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, uuid);
    if (found) {
        return uncommittedPtr.get();
    }

    // Return any previously instantiated collection on this namespace for this snapshot
    if (auto openedColl = OpenedCollections::get(opCtx).lookupByUUID(uuid)) {
        return openedColl.value() ? openedColl->get() : nullptr;
    }

    auto coll = _lookupCollectionByUUID(uuid);
    return (coll && coll->isCommitted()) ? coll.get() : nullptr;
}

const Collection* CollectionCatalog::lookupCollectionByNamespaceOrUUID(
    OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) const {
    if (boost::optional<UUID> uuid = nssOrUUID.uuid())
        return lookupCollectionByUUID(opCtx, *uuid);
    return lookupCollectionByNamespace(opCtx, *nssOrUUID.nss());
}

bool CollectionCatalog::isCollectionAwaitingVisibility(UUID uuid) const {
    auto coll = _lookupCollectionByUUID(uuid);
    return coll && !coll->isCommitted();
}

std::shared_ptr<Collection> CollectionCatalog::_lookupCollectionByUUID(UUID uuid) const {
    const std::shared_ptr<Collection>* coll = _catalog.find(uuid);
    return coll ? *coll : nullptr;
}

std::shared_ptr<const Collection> CollectionCatalog::_getCollectionByNamespace(
    OperationContext* opCtx, const NamespaceString& nss) const {
    // It's important to look in UncommittedCatalogUpdates before OpenedCollections because in a
    // multi-document transaction it's permitted to perform a lookup on a non-existent
    // collection followed by creating the collection. This lookup will store a nullptr in
    // OpenedCollections.
    auto [found, uncommittedColl, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, nss);
    if (uncommittedColl) {
        return uncommittedColl;
    }

    // Report the drop or rename as nothing new was created.
    if (found) {
        return nullptr;
    }

    // Return any previously instantiated collection on this namespace for this snapshot
    if (auto openedColl = OpenedCollections::get(opCtx).lookupByNamespace(nss)) {
        return openedColl.value();
    }

    const std::shared_ptr<Collection>* collPtr = _collections.find(nss);
    auto coll = collPtr ? *collPtr : nullptr;
    return (coll && coll->isCommitted()) ? coll : nullptr;
}

Collection* CollectionCatalog::lookupCollectionByNamespaceForMetadataWrite(
    OperationContext* opCtx, const NamespaceString& nss) const {
    // Oplog is special and can only be modified in a few contexts. It is modified inplace and care
    // need to be taken for concurrency.
    if (nss.isOplog()) {
        return const_cast<Collection*>(lookupCollectionByNamespace(opCtx, nss));
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

    const std::shared_ptr<Collection>* collPtr = _collections.find(nss);
    auto coll = collPtr ? *collPtr : nullptr;

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

const Collection* CollectionCatalog::lookupCollectionByNamespace(OperationContext* opCtx,
                                                                 const NamespaceString& nss) const {
    // If uncommittedPtr is valid, found is always true. Return the pointer as the collection still
    // exists. It's important to look in UncommittedCatalogUpdates before OpenedCollections because
    // in a multi-document transaction it's permitted to perform a lookup on a non-existent
    // collection followed by creating the collection. This lookup will store a nullptr in
    // OpenedCollections.
    auto [found, uncommittedPtr, newColl] = UncommittedCatalogUpdates::lookupCollection(opCtx, nss);
    if (uncommittedPtr) {
        return uncommittedPtr.get();
    }

    // Report the drop or rename as nothing new was created.
    if (found) {
        return nullptr;
    }

    // Return any previously instantiated collection on this namespace for this snapshot
    if (auto openedColl = OpenedCollections::get(opCtx).lookupByNamespace(nss)) {
        return openedColl->get();
    }

    const std::shared_ptr<Collection>* collPtr = _collections.find(nss);
    auto coll = collPtr ? *collPtr : nullptr;
    return (coll && coll->isCommitted()) ? coll.get() : nullptr;
}

boost::optional<NamespaceString> CollectionCatalog::lookupNSSByUUID(OperationContext* opCtx,
                                                                    const UUID& uuid) const {
    // It's important to look in UncommittedCatalogUpdates before OpenedCollections because in a
    // multi-document transaction it's permitted to perform a lookup on a non-existent
    // collection followed by creating the collection. This lookup will store a nullptr in
    // OpenedCollections.
    auto [found, uncommittedPtr, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, uuid);
    // If UUID is managed by uncommittedCatalogUpdates return its corresponding namespace if the
    // Collection exists, boost::none otherwise.
    if (found) {
        if (uncommittedPtr)
            return uncommittedPtr->ns();
        return boost::none;
    }

    // Return any previously instantiated collection on this namespace for this snapshot
    if (auto openedColl = OpenedCollections::get(opCtx).lookupByUUID(uuid)) {
        if (openedColl.value()) {
            return openedColl.value()->ns();
        } else {
            return boost::none;
        }
    }

    const std::shared_ptr<Collection>* collPtr = _catalog.find(uuid);
    if (collPtr) {
        auto coll = *collPtr;
        boost::optional<NamespaceString> ns = coll->ns();
        invariant(!ns.value().isEmpty());
        return coll->isCommitted() ? ns : boost::none;
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
    // It's important to look in UncommittedCatalogUpdates before OpenedCollections because in a
    // multi-document transaction it's permitted to perform a lookup on a non-existent
    // collection followed by creating the collection. This lookup will store a nullptr in
    // OpenedCollections.
    auto [found, uncommittedPtr, newColl] = UncommittedCatalogUpdates::lookupCollection(opCtx, nss);
    if (uncommittedPtr) {
        return uncommittedPtr->uuid();
    }

    if (found) {
        return boost::none;
    }

    // Return any previously instantiated collection on this namespace for this snapshot
    if (auto openedColl = OpenedCollections::get(opCtx).lookupByNamespace(nss)) {
        if (openedColl.value()) {
            return openedColl.value()->uuid();
        } else {
            return boost::none;
        }
    }

    const std::shared_ptr<Collection>* collPtr = _collections.find(nss);
    if (collPtr) {
        auto coll = *collPtr;
        const boost::optional<UUID>& uuid = coll->uuid();
        return coll->isCommitted() ? uuid : boost::none;
    }
    return boost::none;
}

bool CollectionCatalog::containsCollection(OperationContext* opCtx,
                                           const Collection* collection) const {
    // Any writable Collection instance created under MODE_X lock is considered to belong to this
    // catalog instance
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    const auto& entries = uncommittedCatalogUpdates.entries();
    auto entriesIt = std::find_if(entries.begin(),
                                  entries.end(),
                                  [&collection](const UncommittedCatalogUpdates::Entry& entry) {
                                      return entry.collection.get() == collection;
                                  });
    if (entriesIt != entries.end())
        return true;

    // Verify that we store the same instance in this catalog
    const std::shared_ptr<Collection>* coll = _catalog.find(collection->uuid());
    if (!coll)
        return false;

    return coll->get() == collection;
}

CollectionCatalog::CatalogIdLookup CollectionCatalog::lookupCatalogIdByNSS(
    const NamespaceString& nss, boost::optional<Timestamp> ts) const {
    if (const std::vector<TimestampedCatalogId>* mapping = _nssCatalogIds.find(nss)) {
        return _findCatalogIdInRange(ts, *mapping);
    }
    return _checkWithOldestCatalogIdTimestampMaintained(ts);
}

CollectionCatalog::CatalogIdLookup CollectionCatalog::lookupCatalogIdByUUID(
    const UUID& uuid, boost::optional<Timestamp> ts) const {
    if (const std::vector<TimestampedCatalogId>* mapping = _uuidCatalogIds.find(uuid)) {
        return _findCatalogIdInRange(ts, *mapping);
    }
    return _checkWithOldestCatalogIdTimestampMaintained(ts);
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
            str::stream() << "UUID: " << nsOrUUID.toString()
                          << " specified in provided db name: " << nsOrUUID.dbname()
                          << " resolved to a collection in a different database, resolved nss: "
                          << *resolvedNss,
            resolvedNss->dbName() == nsOrUUID.dbName());

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

Status CollectionCatalog::_iterAllDbNamesHelper(
    const boost::optional<TenantId>& tenantId,
    const std::function<Status(const DatabaseName&)>& callback,
    const std::function<std::pair<DatabaseName, UUID>(const DatabaseName&)>& nextUpperBound) const {
    // _orderedCollections is sorted by <dbName, uuid>. upper_bound will return the iterator to the
    // first element in _orderedCollections greater than <firstDbName, maxUuid>.
    auto iter =
        _orderedCollections.upper_bound(std::make_pair(DatabaseName(tenantId, ""), maxUuid));
    while (iter != _orderedCollections.end()) {
        auto dbName = iter->first.first;
        if (tenantId && dbName.tenantId() != tenantId) {
            break;
        }
        if (iter->second->isCommitted()) {
            auto status = callback(dbName);
            if (!status.isOK()) {
                return status;
            }
        } else {
            // If the first collection found for `dbName` is not yet committed, increment the
            // iterator to find the next visible collection (possibly under a different
            // `dbName`).
            iter++;
            continue;
        }
        // Move on to the next database after `dbName`.
        iter = _orderedCollections.upper_bound(nextUpperBound(dbName));
    }
    return Status::OK();
}

std::vector<DatabaseName> CollectionCatalog::getAllDbNames() const {
    return getAllDbNamesForTenant(boost::none);
}

std::vector<DatabaseName> CollectionCatalog::getAllDbNamesForTenant(
    boost::optional<TenantId> tenantId) const {
    std::vector<DatabaseName> ret;
    (void)_iterAllDbNamesHelper(
        tenantId,
        [&ret](const DatabaseName& dbName) {
            ret.push_back(dbName);
            return Status::OK();
        },
        [](const DatabaseName& dbName) { return std::make_pair(dbName, maxUuid); });
    return ret;
}

std::set<TenantId> CollectionCatalog::getAllTenants() const {
    std::set<TenantId> ret;
    (void)_iterAllDbNamesHelper(
        boost::none,
        [&ret](const DatabaseName& dbName) {
            if (const auto& tenantId = dbName.tenantId()) {
                ret.insert(*tenantId);
            }
            return Status::OK();
        },
        [](const DatabaseName& dbName) {
            return std::make_pair(DatabaseName(dbName.tenantId(), "\xff"), maxUuid);
        });
    return ret;
}

void CollectionCatalog::setAllDatabaseProfileFilters(std::shared_ptr<ProfileFilter> filter) {
    auto dbProfileSettingsWriter = _databaseProfileSettings.transient();
    for (const auto& [dbName, settings] : _databaseProfileSettings) {
        ProfileSettings clone = settings;
        clone.filter = filter;
        dbProfileSettingsWriter.set(dbName, std::move(clone));
    }
    _databaseProfileSettings = dbProfileSettingsWriter.persistent();
}

void CollectionCatalog::setDatabaseProfileSettings(
    const DatabaseName& dbName, CollectionCatalog::ProfileSettings newProfileSettings) {
    _databaseProfileSettings = _databaseProfileSettings.set(dbName, std::move(newProfileSettings));
}

CollectionCatalog::ProfileSettings CollectionCatalog::getDatabaseProfileSettings(
    const DatabaseName& dbName) const {
    const ProfileSettings* settings = _databaseProfileSettings.find(dbName);
    if (settings) {
        return *settings;
    }

    return {serverGlobalParams.defaultProfile, ProfileFilter::getDefault()};
}

void CollectionCatalog::clearDatabaseProfileSettings(const DatabaseName& dbName) {
    _databaseProfileSettings = _databaseProfileSettings.erase(dbName);
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
    invariant(!_catalog.find(uuid));
    invariant(_orderedCollections.find(dbIdPair) == _orderedCollections.end());

    _catalog = _catalog.set(uuid, coll);
    _collections = _collections.set(nss, coll);
    _orderedCollections[dbIdPair] = coll;
    if (twoPhase) {
        _pendingCommitNamespaces = _pendingCommitNamespaces.set(nss, coll);
        _pendingCommitUUIDs = _pendingCommitUUIDs.set(uuid, coll);
    } else {
        _pendingCommitNamespaces = _pendingCommitNamespaces.erase(nss);
        _pendingCommitUUIDs = _pendingCommitUUIDs.erase(uuid);
    }

    if (commitTime && !commitTime->isNull()) {
        coll->setMinimumValidSnapshot(commitTime.value());
        _pushCatalogIdForNSSAndUUID(nss, uuid, coll->getCatalogId(), commitTime);
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
        ViewsForDatabase viewsForDb;
        if (auto status = viewsForDb.reload(
                opCtx, CollectionPtr(_lookupSystemViews(opCtx, coll->ns().dbName())));
            !status.isOK()) {
            LOGV2_WARNING_OPTIONS(20326,
                                  {logv2::LogTag::kStartupWarnings},
                                  "Unable to parse views; remove any invalid views from the "
                                  "collection to restore server functionality",
                                  "error"_attr = redact(status),
                                  logAttrs(coll->ns()));
        }
        _viewsForDatabase = _viewsForDatabase.set(coll->ns().dbName(), std::move(viewsForDb));
    }
}

std::shared_ptr<Collection> CollectionCatalog::deregisterCollection(
    OperationContext* opCtx,
    const UUID& uuid,
    bool isDropPending,
    boost::optional<Timestamp> commitTime) {
    invariant(_catalog.find(uuid));

    auto coll = std::move(_catalog[uuid]);
    auto ns = coll->ns();
    auto dbIdPair = std::make_pair(ns.dbName(), uuid);

    LOGV2_DEBUG(20281, 1, "Deregistering collection", logAttrs(ns), "uuid"_attr = uuid);

    // Make sure collection object exists.
    invariant(_collections.find(ns));
    invariant(_orderedCollections.find(dbIdPair) != _orderedCollections.end());

    // TODO SERVER-68674: Remove feature flag check.
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe() && isDropPending) {
        if (auto sharedIdent = coll->getSharedIdent(); sharedIdent) {
            auto ident = sharedIdent->getIdent();
            LOGV2_DEBUG(
                6825300, 1, "Registering drop pending collection ident", "ident"_attr = ident);

            invariant(!_dropPendingCollection.find(ident));
            _dropPendingCollection = _dropPendingCollection.set(ident, coll);
        }
    }

    _orderedCollections.erase(dbIdPair);
    _collections = _collections.erase(ns);
    _catalog = _catalog.erase(uuid);
    _pendingCommitNamespaces = _pendingCommitNamespaces.erase(ns);
    _pendingCommitUUIDs = _pendingCommitUUIDs.erase(uuid);

    // Push drop unless this is a rollback of a create
    if (coll->isCommitted()) {
        _pushCatalogIdForNSSAndUUID(ns, uuid, boost::none, commitTime);
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
        _viewsForDatabase = _viewsForDatabase.erase(coll->ns().dbName());
    }

    return coll;
}

void CollectionCatalog::registerUncommittedView(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString::makeSystemDotViewsNamespace(nss.dbName()), MODE_X));

    // Since writing to system.views requires an X lock, we only need to cross-check collection
    // namespaces here.
    _ensureNamespaceDoesNotExist(opCtx, nss, NamespaceType::kCollection);

    _uncommittedViews = _uncommittedViews.insert(nss);
}

void CollectionCatalog::deregisterUncommittedView(const NamespaceString& nss) {
    _uncommittedViews = _uncommittedViews.erase(nss);
}

void CollectionCatalog::_ensureNamespaceDoesNotExist(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     NamespaceType type) const {
    auto existingCollection = _collections.find(nss);
    if (existingCollection) {
        LOGV2(5725001,
              "Conflicted registering namespace, already have a collection with the same namespace",
              "nss"_attr = nss);
        throwWriteConflictException(str::stream() << "Collection namespace '" << nss.ns()
                                                  << "' is already in use.");
    }

    if (type == NamespaceType::kAll) {
        if (_uncommittedViews.find(nss)) {
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

void CollectionCatalog::_pushCatalogIdForNSSAndUUID(const NamespaceString& nss,
                                                    const UUID& uuid,
                                                    boost::optional<RecordId> catalogId,
                                                    boost::optional<Timestamp> ts) {
    // TODO SERVER-68674: Remove feature flag check.
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe()) {
        // No-op.
        return;
    }

    auto doPushCatalogId = [this, &ts, &catalogId](auto& catalogIdsContainer,
                                                   auto& catalogIdChangesContainer,
                                                   const auto& key) {
        auto ids = copyIfExists(catalogIdsContainer, key);

        // Helper to write updated id mapping back into container at scope exit
        ScopeGuard scopedGuard([&] {
            // Make sure we erase mapping for namespace or UUID if the list is left empty as
            // lookups expect at least one entry for existing namespaces or UUIDs.
            if (ids.empty()) {
                catalogIdsContainer = catalogIdsContainer.erase(key);
            } else {
                catalogIdsContainer = catalogIdsContainer.set(key, std::move(ids));
            }
        });

        if (!ts) {
            // Make sure untimestamped writes have a single entry in mapping. If we're mixing
            // timestamped with untimestamped (such as repair). Ignore the untimestamped writes as
            // an untimestamped deregister will correspond with an untimestamped register. We should
            // leave the mapping as-is in this case.
            if (ids.empty() && catalogId) {
                // This namespace or UUID was added due to an untimestamped write, add an entry
                // with min timestamp
                ids.push_back(TimestampedCatalogId{catalogId, Timestamp::min()});
            } else if (ids.size() == 1 && !catalogId) {
                // This namespace or UUID was removed due to an untimestamped write, clear entries.
                ids.clear();
            }

            return;
        }

        // An entry could exist already if concurrent writes are performed, keep the latest change
        // in that case.
        if (!ids.empty() && ids.back().ts == *ts) {
            ids.back().id = catalogId;
            return;
        }

        // Otherwise, push new entry at the end. Timestamp is always increasing
        invariant(ids.empty() || ids.back().ts < *ts);
        // If the catalogId is the same as last entry, there's nothing we need to do. This can
        // happen when the catalog is reopened.
        if (!ids.empty() && ids.back().id == catalogId) {
            return;
        }

        ids.push_back(TimestampedCatalogId{catalogId, *ts});

        auto changes = catalogIdChangesContainer.transient();
        _markForCatalogIdCleanupIfNeeded(key, changes, ids);
        catalogIdChangesContainer = changes.persistent();
    };

    doPushCatalogId(_nssCatalogIds, _nssCatalogIdChanges, nss);
    doPushCatalogId(_uuidCatalogIds, _uuidCatalogIdChanges, uuid);
}

void CollectionCatalog::_pushCatalogIdForRename(const NamespaceString& from,
                                                const NamespaceString& to,
                                                boost::optional<Timestamp> ts) {
    // TODO SERVER-68674: Remove feature flag check.
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe()) {
        // No-op.
        return;
    }

    // Get 'toIds' first, it may need to instantiate in the container which invalidates all
    // references.
    auto idsWriter = _nssCatalogIds.transient();
    auto changesWriter = _nssCatalogIdChanges.transient();
    auto toIds = copyIfExists(idsWriter, to);
    auto fromIds = copyIfExists(idsWriter, from);
    invariant(!fromIds.empty());

    // Helper to write updated id mappings back into containers at scope exit
    ScopeGuard scopedGuard([&] {
        // Make sure we erase mapping for namespace or UUID if the list is left empty as
        // lookups expect at least one entry for existing namespaces or UUIDs.
        idsWriter.set(to, std::move(toIds));
        if (fromIds.empty()) {
            idsWriter.erase(from);
        } else {
            idsWriter.set(from, std::move(fromIds));
        }
        _nssCatalogIds = idsWriter.persistent();
        _nssCatalogIdChanges = changesWriter.persistent();
    });

    // Make sure untimestamped writes have a single entry in mapping. We move the single entry from
    // 'from' to 'to'. We do not have to worry about mixing timestamped and untimestamped like
    // _pushCatalogId.
    if (!ts) {
        // We should never perform rename in a mixed-mode environment. 'from' should contain a
        // single entry and there should be nothing in 'to' .
        invariant(fromIds.size() == 1);
        invariant(toIds.empty());
        toIds.push_back(TimestampedCatalogId{fromIds.back().id, Timestamp::min()});
        fromIds.clear();
        return;
    }

    // An entry could exist already if concurrent writes are performed, keep the latest change in
    // that case.
    if (!toIds.empty() && toIds.back().ts == *ts) {
        toIds.back().id = fromIds.back().id;
    } else {
        invariant(toIds.empty() || toIds.back().ts < *ts);
        toIds.push_back(TimestampedCatalogId{fromIds.back().id, *ts});
        _markForCatalogIdCleanupIfNeeded(to, changesWriter, toIds);
    }

    // Re-write latest entry if timestamp match (multiple changes occured in this transaction),
    // otherwise push at end
    if (!fromIds.empty() && fromIds.back().ts == *ts) {
        fromIds.back().id = boost::none;
    } else {
        invariant(fromIds.empty() || fromIds.back().ts < *ts);
        fromIds.push_back(TimestampedCatalogId{boost::none, *ts});
        _markForCatalogIdCleanupIfNeeded(from, changesWriter, fromIds);
    }
}

void CollectionCatalog::_insertCatalogIdForNSSAndUUIDAfterScan(
    boost::optional<const NamespaceString&> nss,
    boost::optional<UUID> uuid,
    boost::optional<RecordId> catalogId,
    Timestamp ts) {
    // TODO SERVER-68674: Remove feature flag check.
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe()) {
        // No-op.
        return;
    }

    auto doInsert = [this, &catalogId, &ts](auto& catalogIdsContainer,
                                            auto& catalogIdChangesContainer,
                                            const auto& key) {
        auto changesWriter = catalogIdChangesContainer.transient();
        auto ids = copyIfExists(catalogIdsContainer, key);

        // Helper to write updated id mapping back into container at scope exit
        ScopeGuard scopedGuard([&] {
            // Make sure we erase mapping for namespace or UUID if the list is left empty as
            // lookups expect at least one entry for existing namespaces or UUIDs.
            if (ids.empty()) {
                catalogIdsContainer = catalogIdsContainer.erase(key);
            } else {
                catalogIdsContainer = catalogIdsContainer.set(key, std::move(ids));
            }
            catalogIdChangesContainer = changesWriter.persistent();
        });

        // Binary search for to the entry with same or larger timestamp
        auto it = std::lower_bound(
            ids.begin(), ids.end(), ts, [](const auto& entry, const Timestamp& ts) {
                return entry.ts < ts;
            });

        // The logic of what we need to do differs whether we are inserting a valid catalogId or
        // not.
        if (catalogId) {
            if (it != ids.end()) {
                // An entry could exist already if concurrent writes are performed, keep the latest
                // change in that case.
                if (it->ts == ts) {
                    it->id = catalogId;
                    return;
                }

                // If next element has same catalogId, we can adjust its timestamp to cover a longer
                // range
                if (it->id == catalogId) {
                    it->ts = ts;
                    _markForCatalogIdCleanupIfNeeded(key, changesWriter, ids);
                    return;
                }
            }

            // Otherwise insert new entry at timestamp
            ids.insert(it, TimestampedCatalogId{catalogId, ts});
            _markForCatalogIdCleanupIfNeeded(key, changesWriter, ids);
            return;
        }

        // Avoid inserting missing mapping when the list has grown past the threshold. Will cause
        // the system to fall back to scanning the durable catalog.
        if (ids.size() >= kMaxCatalogIdMappingLengthForMissingInsert) {
            return;
        }

        if (it != ids.end() && it->ts == ts) {
            // An entry could exist already if concurrent writes are performed, keep the latest
            // change in that case.
            it->id = boost::none;
        } else {
            // Otherwise insert new entry
            it = ids.insert(it, TimestampedCatalogId{boost::none, ts});
        }

        // The iterator is positioned on the added/modified element above, reposition it to the next
        // entry
        ++it;

        // We don't want to assume that the namespace or UUID remains not existing until the next
        // entry, as there can be times where the namespace or UUID actually does exist. To make
        // sure we trigger the scanning of the durable catalog in this range we will insert a bogus
        // entry using an invalid RecordId at the next timestamp. This will treat the range forward
        // as unknown.
        auto nextTs = ts + 1;

        // If the next entry is on the next timestamp already, we can skip adding the bogus entry.
        // If this function is called for a previously unknown namespace or UUID, we may not have
        // any future valid entries and the iterator would be positioned at and at this point.
        if (it == ids.end() || it->ts != nextTs) {
            ids.insert(it, TimestampedCatalogId{kUnknownRangeMarkerId, nextTs});
        }

        _markForCatalogIdCleanupIfNeeded(key, changesWriter, ids);
    };

    if (nss) {
        doInsert(_nssCatalogIds, _nssCatalogIdChanges, *nss);
    }

    if (uuid) {
        doInsert(_uuidCatalogIds, _uuidCatalogIdChanges, *uuid);
    }
}

template <class Key, class CatalogIdChangesContainer>
void CollectionCatalog::_markForCatalogIdCleanupIfNeeded(
    const Key& key,
    CatalogIdChangesContainer& catalogIdChangesContainer,
    const std::vector<TimestampedCatalogId>& ids) {

    auto markForCleanup = [this, &key, &catalogIdChangesContainer](Timestamp ts) {
        catalogIdChangesContainer.insert(key);
        if (ts < _lowestCatalogIdTimestampForCleanup) {
            _lowestCatalogIdTimestampForCleanup = ts;
        }
    };

    // Cleanup may occur if we have more than one entry for the namespace or if the only entry is a
    // drop.
    if (ids.size() > 1) {
        // When we have multiple entries, use the time at the second entry as the cleanup time,
        // when the oldest timestamp advances past this we no longer need the first entry.
        markForCleanup(ids.at(1).ts);
    } else if (ids.front().id == boost::none) {
        // If we just have a single delete, we can clean this up when the oldest timestamp advances
        // past this time.
        markForCleanup(ids.front().ts);
    }
}

void CollectionCatalog::deregisterAllCollectionsAndViews(ServiceContext* svcCtx) {
    LOGV2(20282, "Deregistering all the collections");
    for (auto& entry : _catalog) {
        auto uuid = entry.first;
        auto ns = entry.second->ns();

        LOGV2_DEBUG(20283, 1, "Deregistering collection", logAttrs(ns), "uuid"_attr = uuid);
    }

    _collections = {};
    _orderedCollections.clear();
    _catalog = {};
    _viewsForDatabase = {};
    _dropPendingCollection = {};
    _dropPendingIndex = {};
    _stats = {};

    ResourceCatalog::get(svcCtx).clear();
}

void CollectionCatalog::clearViews(OperationContext* opCtx, const DatabaseName& dbName) const {
    invariant(opCtx->lockState()->isCollectionLockedForMode(
        NamespaceString::makeSystemDotViewsNamespace(dbName), MODE_X));

    const ViewsForDatabase* viewsForDbPtr = _viewsForDatabase.find(dbName);
    invariant(viewsForDbPtr);

    ViewsForDatabase viewsForDb = *viewsForDbPtr;
    viewsForDb.clear(opCtx);

    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog._replaceViewsForDatabase(dbName, std::move(viewsForDb));
    });
}

void CollectionCatalog::deregisterIndex(OperationContext* opCtx,
                                        std::shared_ptr<IndexCatalogEntry> indexEntry,
                                        bool isDropPending) {
    // TODO SERVER-68674: Remove feature flag check.
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe() ||
        !isDropPending) {
        // No-op.
        return;
    }

    // Unfinished index builds return a nullptr for getSharedIdent(). Use getIdent() instead.
    std::string ident = indexEntry->getIdent();

    invariant(!_dropPendingIndex.find(ident));

    LOGV2_DEBUG(6825301, 1, "Registering drop pending index entry ident", "ident"_attr = ident);
    _dropPendingIndex = _dropPendingIndex.set(ident, indexEntry);
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

    _dropPendingCollection = _dropPendingCollection.erase(ident);
    _dropPendingIndex = _dropPendingIndex.erase(ident);
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
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (!feature_flags::gPointInTimeCatalogLookups.isEnabledAndIgnoreFCVUnsafe()) {
        // No-op.
        return false;
    }

    return _lowestCatalogIdTimestampForCleanup <= oldest;
}

void CollectionCatalog::cleanupForOldestTimestampAdvanced(Timestamp oldest) {
    Timestamp nextLowestCleanupTimestamp = Timestamp::max();
    // Helper to calculate the smallest entry that needs to be kept and its timestamp
    auto assignLowestCleanupTimestamp = [&nextLowestCleanupTimestamp](const auto& range) {
        // The second entry is cleanup time as at that point the first entry is no longer needed.
        // The input range have at a minimum two entries.
        auto it = range.begin() + 1;
        nextLowestCleanupTimestamp = std::min(nextLowestCleanupTimestamp, it->ts);
    };

    auto doCleanup = [this, &oldest, &assignLowestCleanupTimestamp](
                         auto& catalogIdsContainer, auto& catalogIdChangesContainer) {
        // Batch all changes together
        auto ids = catalogIdsContainer.transient();
        auto changes = catalogIdChangesContainer.transient();

        for (auto it = catalogIdChangesContainer.begin(), end = catalogIdChangesContainer.end();
             it != end;) {
            auto range = ids[*it];

            // Binary search for next larger timestamp
            auto rangeIt = std::upper_bound(
                range.begin(), range.end(), oldest, [](const auto& ts, const auto& entry) {
                    return ts < entry.ts;
                });

            // Continue if there is nothing to cleanup for this timestamp yet
            if (rangeIt == range.begin()) {
                // There should always be at least two entries in the range when we hit this
                // branch. For the namespace to be put in '_nssCatalogIdChanges' we normally
                // need at least two entries. The namespace could require cleanup with just a
                // single entry if 'cleanupForCatalogReopen' leaves a single drop entry in the
                // range. But because we cannot initialize the namespace with a single drop
                // there must have been a non-drop entry earlier that got cleaned up in a
                // previous call to 'cleanupForOldestTimestampAdvanced', which happens when the
                // oldest timestamp advances past the drop timestamp. This guarantees that the
                // oldest timestamp is larger than the timestamp in the single drop entry
                // resulting in this branch cannot be taken when we only have a drop in the
                // range.
                invariant(range.size() > 1);
                assignLowestCleanupTimestamp(range);
                ++it;
                continue;
            }

            // The iterator is positioned to the closest entry that has a larger timestamp,
            // decrement to get a lower or equal timestamp
            --rangeIt;

            // Erase range, we will leave at least one element due to the decrement above
            range.erase(range.begin(), rangeIt);

            // If more changes are needed for this namespace, keep it in the set and keep track
            // of lowest timestamp.
            if (range.size() > 1) {
                assignLowestCleanupTimestamp(range);
                ids.set(*it, std::move(range));
                ++it;
                continue;
            }
            // If the last remaining element is a drop earlier than the oldest timestamp, we can
            // remove tracking this namespace
            if (range.back().id == boost::none) {
                ids.erase(*it);
            } else {
                ids.set(*it, std::move(range));
            }

            // Unmark this namespace or UUID for needing changes.
            changes.erase(*it);
            ++it;
        }

        // Write back all changes to main container
        catalogIdChangesContainer = changes.persistent();
        catalogIdsContainer = ids.persistent();
    };

    // Iterate over all namespaces and UUIDs that is marked that they need cleanup
    doCleanup(_nssCatalogIds, _nssCatalogIdChanges);
    doCleanup(_uuidCatalogIds, _uuidCatalogIdChanges);

    _lowestCatalogIdTimestampForCleanup = nextLowestCleanupTimestamp;
    _oldestCatalogIdTimestampMaintained = std::max(_oldestCatalogIdTimestampMaintained, oldest);
}

void CollectionCatalog::cleanupForCatalogReopen(Timestamp stable) {
    _nssCatalogIdChanges = {};
    _uuidCatalogIdChanges = {};
    _lowestCatalogIdTimestampForCleanup = Timestamp::max();
    _oldestCatalogIdTimestampMaintained = std::min(_oldestCatalogIdTimestampMaintained, stable);

    auto removeLargerTimestamps = [this, &stable](auto& catalogIdsContainer,
                                                  auto& catalogIdChangesContainer) {
        // Batch all changes together
        auto idsWriter = catalogIdsContainer.transient();
        auto changesWriter = catalogIdChangesContainer.transient();

        for (auto it = catalogIdsContainer.begin(); it != catalogIdsContainer.end();) {
            auto ids = it->second;

            // Remove all larger timestamps in this range
            ids.erase(
                std::upper_bound(ids.begin(),
                                 ids.end(),
                                 stable,
                                 [](Timestamp ts, const auto& entry) { return ts < entry.ts; }),
                ids.end());

            // Remove namespace or UUID if there are no entries left
            if (ids.empty()) {
                idsWriter.erase(it->first);
                ++it;
                continue;
            }

            // Calculate when this namespace needs to be cleaned up next
            _markForCatalogIdCleanupIfNeeded(it->first, changesWriter, ids);
            idsWriter.set(it->first, std::move(ids));
            ++it;
        }

        // Write back all changes to main container
        catalogIdChangesContainer = changesWriter.persistent();
        catalogIdsContainer = idsWriter.persistent();
    };

    removeLargerTimestamps(_nssCatalogIds, _nssCatalogIdChanges);
    removeLargerTimestamps(_uuidCatalogIds, _uuidCatalogIdChanges);
}

void CollectionCatalog::invariantHasExclusiveAccessToCollection(OperationContext* opCtx,
                                                                const NamespaceString& nss) {
    invariant(hasExclusiveAccessToCollection(opCtx, nss), nss.toString());
}

bool CollectionCatalog::hasExclusiveAccessToCollection(OperationContext* opCtx,
                                                       const NamespaceString& nss) {
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    return opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X) ||
        (uncommittedCatalogUpdates.isCreatedCollection(opCtx, nss) &&
         opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));
}

const Collection* CollectionCatalog::_lookupSystemViews(OperationContext* opCtx,
                                                        const DatabaseName& dbName) const {
    return lookupCollectionByNamespace(opCtx, NamespaceString::makeSystemDotViewsNamespace(dbName));
}

boost::optional<const ViewsForDatabase&> CollectionCatalog::_getViewsForDatabase(
    OperationContext* opCtx, const DatabaseName& dbName) const {
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    auto uncommittedViews = uncommittedCatalogUpdates.getViewsForDatabase(dbName);
    if (uncommittedViews) {
        return uncommittedViews;
    }

    const ViewsForDatabase* viewsForDb = _viewsForDatabase.find(dbName);
    if (!viewsForDb) {
        return boost::none;
    }
    return *viewsForDb;
}

void CollectionCatalog::_replaceViewsForDatabase(const DatabaseName& dbName,
                                                 ViewsForDatabase&& views) {
    _viewsForDatabase = _viewsForDatabase.set(dbName, std::move(views));
}

bool CollectionCatalog::_isCatalogBatchWriter() const {
    return ongoingBatchedWrite.load() && batchedCatalogWriteInstance.get() == this;
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
    ongoingBatchedWrite.store(true);
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
    ongoingBatchedWrite.store(false);
    _batchedInstance = nullptr;
    batchedCatalogWriteInstance = nullptr;
}

}  // namespace mongo
