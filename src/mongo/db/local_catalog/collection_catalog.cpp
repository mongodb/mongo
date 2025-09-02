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

#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <immer/detail/hamts/champ_iterator.hpp>
#include <immer/detail/iterator_facade.hpp>
#include <immer/detail/rbts/rrbtree_iterator.hpp>
#include <immer/detail/util.hpp>
#include <immer/map.hpp>
#include <immer/map_transient.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/collection_record_store_options.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/lock_manager/resource_catalog.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/uncommitted_catalog_updates.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <list>
#include <mutex>
#include <shared_mutex>

#include "collection_catalog.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

// Failpoint which causes catalog updates to hang right before performing a durable commit of them.
// This causes updates to have been precommitted.
MONGO_FAIL_POINT_DEFINE(hangAfterPreCommittingCatalogUpdates);
static constexpr auto kDelayEntireCommitFailpointField = "pauseEntireCommitMillis"_sd;

// Failpoint which causes to hang after wuow commits, before publishing the catalog updates on a
// given namespace.
MONGO_FAIL_POINT_DEFINE(hangBeforePublishingCatalogUpdates);

MONGO_FAIL_POINT_DEFINE(setMinVisibleForAllCollectionsToOldestOnStartup);

/**
 * If a collection is initially created with an untimestamped write, but later DDL operations
 * (including drop) on this collection are timestamped, set this decoration to 'true' for
 * HistoricalCatalogIdTracker to support this mixed mode write sequence for a collection.
 *
 * CAUTION: This decoration is not to support other mixed mode write sequences (such as
 * timestamped collection creation followed by untimestamped drop) that violates wiredtiger's
 * timestamp rules.
 */
const SharedCollectionDecorations::Decoration<AtomicWord<bool>>
    historicalIDTrackerAllowsMixedModeWrites =
        SharedCollectionDecorations::declareDecoration<AtomicWord<bool>>();

namespace catalog {
void initializeCollectionCatalog(OperationContext* opCtx, StorageEngine* engine) {
    initializeCollectionCatalog(opCtx, engine, engine->getEngine()->getRecoveryTimestamp());
}

void initializeCollectionCatalog(OperationContext* opCtx,
                                 StorageEngine* engine,
                                 boost::optional<Timestamp> stableTs) {
    // Use the stable timestamp as minValid. We know for a fact that the collection exist at
    // this point and is in sync. If we use an earlier timestamp than replication rollback we
    // may be out-of-order for the collection catalog managing this namespace.
    const Timestamp minValidTs = stableTs ? *stableTs : Timestamp::min();
    CollectionCatalog::write(opCtx, [&minValidTs](CollectionCatalog& catalog) {
        // Let the CollectionCatalog know that we are maintaining timestamps from minValidTs
        catalog.catalogIdTracker().rollback(minValidTs);
    });

    bool setMinVisibleToOldestFailpointSet = false;
    if (MONGO_unlikely(setMinVisibleForAllCollectionsToOldestOnStartup.shouldFail())) {
        // Failpoint is intended to apply to all collections. Additionally, we want to leverage
        // nTimes to execute the failpoint for a single 'initializeCollectionCatalog' call.
        LOGV2(9106700, "setMinVisibleForAllCollectionsToOldestOnStartup failpoint is set");
        setMinVisibleToOldestFailpointSet = true;
    }

    std::vector<MDBCatalog::EntryIdentifier> catalogEntries =
        engine->getMDBCatalog()->getAllCatalogEntries(opCtx);
    for (MDBCatalog::EntryIdentifier entry : catalogEntries) {
        // If there's no recovery timestamp, every collection is available.
        auto collectionMinValidTs = minValidTs;
        if (MONGO_unlikely(stableTs && setMinVisibleToOldestFailpointSet)) {
            // This failpoint is useful for tests which want to exercise atClusterTime reads across
            // server starts (e.g. resharding). It is only safe for tests which can guarantee the
            // collection always exists for the atClusterTime value(s) and have not changed (i.e. no
            // DDL operations have run on them).
            //
            // Despite its name, the setMinVisibleForAllCollectionsToOldestOnStartup failpoint
            // controls the minValidTs in MongoDB Server versions with a point-in-time
            // CollectionCatalog but had controlled the minVisibleTs in older MongoDB Server
            // versions. We haven't renamed it to avoid issues in multiversion testing.
            auto shouldSetMinVisibleToOldest = [&]() {
                // We only do this for collections that existed at the oldest timestamp or after
                // startup when we aren't sure if it existed or not.
                const auto catalog = CollectionCatalog::latest(opCtx);
                const auto& tracker = catalog->catalogIdTracker();
                auto oldestTs = engine->getEngine()->getOldestTimestamp();
                auto lookup = tracker.lookup(entry.nss, oldestTs);
                return lookup.result !=
                    HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists;
            }();

            if (shouldSetMinVisibleToOldest) {
                auto oldestTs = engine->getEngine()->getOldestTimestamp();
                if (collectionMinValidTs > oldestTs)
                    collectionMinValidTs = oldestTs;
            }
        }

        initCollectionObject(opCtx,
                             engine,
                             entry.catalogId,
                             entry.nss,
                             storageGlobalParams.repair,
                             collectionMinValidTs);
    }
}

void initCollectionObject(OperationContext* opCtx,
                          StorageEngine* engine,
                          RecordId catalogId,
                          const NamespaceString& nss,
                          bool forRepair,
                          Timestamp minValidTs) {
    const auto mdbCatalog = engine->getMDBCatalog();
    const auto catalogEntry = durable_catalog::getParsedCatalogEntry(opCtx, catalogId, mdbCatalog);
    const auto md = catalogEntry->metadata;
    uassert(ErrorCodes::MustDowngrade,
            str::stream() << "Collection does not have UUID in KVCatalog. Collection: "
                          << nss.toStringForErrorMsg(),
            md->options.uuid);

    auto ident = mdbCatalog->getEntry(catalogId).ident;

    std::unique_ptr<RecordStore> rs;
    if (forRepair) {
        // Using a NULL rs since we don't want to open this record store before it has been
        // repaired. This also ensures that if we try to use it, it will blow up.
        rs = nullptr;
    } else {
        const auto uuid = md->options.uuid;
        const auto recordStoreOptions = getRecordStoreOptions(nss, md->options);
        rs = engine->getEngine()->getRecordStore(opCtx, nss, ident, recordStoreOptions, uuid);
        invariant(rs);
    }

    auto collectionFactory = Collection::Factory::get(getGlobalServiceContext());
    auto collection = collectionFactory->make(opCtx, nss, catalogId, md, std::move(rs));

    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog.registerCollection(opCtx, std::move(collection), /*commitTime*/ minValidTs);
    });
}

std::vector<DatabaseName> listDatabases(boost::optional<TenantId> tenantId) {
    auto res = tenantId
        ? CollectionCatalog::latest(getGlobalServiceContext())->getAllDbNamesForTenant(tenantId)
        : CollectionCatalog::latest(getGlobalServiceContext())->getAllDbNames();
    return res;
}
}  // namespace catalog

namespace {
constexpr auto kNumDurableCatalogScansDueToMissingMapping = "numScansDueToMissingMapping"_sd;

class LatestCollectionCatalog {
public:
    std::shared_ptr<CollectionCatalog> load() const {
        std::shared_lock lk(_readMutex);  // NOLINT
        return _catalog;
    }

    void write(auto&& fn) {
        // The funny scoping here is to destroy the old catalog after releasing the locks, as if no
        // one else has a reference it can be mildly expensive and we don't want to block other
        // threads while we do it
        std::shared_ptr<CollectionCatalog> newCatalog;
        {
            std::lock_guard lk(_writeMutex);
            newCatalog = fn(*_catalog);
            std::lock_guard lk2(_readMutex);
            _catalog.swap(newCatalog);
        }
    }

private:
    // TODO SERVER-56428: Replace _readMutex std::atomic<std::shared_ptr> when supported in our
    // toolchain. _writeMutex should remain a mutex.
    mutable RWMutex _readMutex;
    mutable stdx::mutex _writeMutex;
    std::shared_ptr<CollectionCatalog> _catalog = std::make_shared<CollectionCatalog>();
};
const ServiceContext::Decoration<LatestCollectionCatalog> getCatalogStore =
    ServiceContext::declareDecoration<LatestCollectionCatalog>();

const RecoveryUnit::Snapshot::Decoration<std::shared_ptr<const CollectionCatalog>> stashedCatalog =
    RecoveryUnit::Snapshot::declareDecoration<std::shared_ptr<const CollectionCatalog>>();

/**
 * Returns true if the collection is compatible with the read timestamp.
 */
bool isExistingCollectionCompatible(const std::shared_ptr<const Collection>& coll,
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

ViewsForDatabase loadViewsForDatabase(OperationContext* opCtx,
                                      const CollectionCatalog& catalog,
                                      const DatabaseName& dbName) {
    ViewsForDatabase viewsForDb;
    auto systemDotViews = NamespaceString::makeSystemDotViewsNamespace(dbName);
    // The system.views is a special collection that is always present in the catalog and can't be
    // modified or dropped. The Collection* returned by the lookup can't disappear. The
    // initialization here is therefore safe.
    if (auto status =
            viewsForDb.reload(opCtx,
                              CollectionPtr::CollectionPtr_UNSAFE(
                                  catalog.lookupCollectionByNamespace(opCtx, systemDotViews)));
        !status.isOK()) {
        LOGV2_WARNING_OPTIONS(20326,
                              {logv2::LogTag::kStartupWarnings},
                              "Unable to parse views; remove any invalid views from the "
                              "collection to restore server functionality",
                              "error"_attr = redact(status),
                              logAttrs(systemDotViews));
    }
    return viewsForDb;
}

const auto maxUuid = UUID::parse("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF").getValue();
const auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();

// CSFLE 1 collections have a schema validator with the encrypt keyword
bool isCSFLE1Validator(BSONObj doc) {
    if (doc.isEmpty()) {
        return false;
    }

    std::stack<BSONObjIterator> frameStack;

    const ScopeGuard frameStackGuard([&] {
        while (!frameStack.empty()) {
            frameStack.pop();
        }
    });

    frameStack.emplace(BSONObjIterator(doc));

    while (frameStack.size() > 1 || frameStack.top().more()) {
        if (frameStack.size() == BSONDepth::kDefaultMaxAllowableDepth) {
            return false;
        }

        auto& iterator = frameStack.top();
        if (iterator.more()) {
            BSONElement elem = iterator.next();
            if (elem.type() == BSONType::object) {
                if (elem.fieldNameStringData() == "encrypt"_sd) {
                    return true;
                }

                frameStack.emplace(BSONObjIterator(elem.Obj()));
            } else if (elem.type() == BSONType::array) {
                frameStack.emplace(BSONObjIterator(elem.Obj()));
            }
        } else {
            frameStack.pop();
        }
    }

    dassert(frameStack.size() == 1);

    return false;
}
}  // namespace

/**
 * Defines a new serverStatus section "collectionCatalog".
 */
class CollectionCatalogSection final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

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
};

auto& gCollectionCatalogSection =
    *ServerStatusSectionBuilder<CollectionCatalogSection>("collectionCatalog").forShard();

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
        catalog._orderedCollections = catalog._orderedCollections.set(dbIdPair, collection);

        catalog._pendingCommitNamespaces = catalog._pendingCommitNamespaces.erase(collection->ns());
        catalog._pendingCommitUUIDs = catalog._pendingCommitUUIDs.erase(collection->uuid());
    }

    PublishCatalogUpdates(UncommittedCatalogUpdates& uncommittedCatalogUpdates)
        : _uncommittedCatalogUpdates(uncommittedCatalogUpdates) {}

    static void ensureRegisteredWithRecoveryUnit(
        OperationContext* opCtx, UncommittedCatalogUpdates& uncommittedCatalogUpdates) {
        if (uncommittedCatalogUpdates.hasRegisteredWithRecoveryUnit())
            return;

        shard_role_details::getRecoveryUnit(opCtx)->registerPreCommitHook(
            [](OperationContext* opCtx) { PublishCatalogUpdates::preCommit(opCtx); });
        shard_role_details::getRecoveryUnit(opCtx)->registerChangeForCatalogVisibility(
            std::make_unique<PublishCatalogUpdates>(uncommittedCatalogUpdates));
        uncommittedCatalogUpdates.markRegisteredWithRecoveryUnit();
    }

    static void preCommit(OperationContext* opCtx) {
        auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
        const auto& entries = uncommittedCatalogUpdates.entries();

        if (std::none_of(
                entries.begin(), entries.end(), UncommittedCatalogUpdates::isTwoPhaseCommitEntry)) {
            // Nothing to do, avoid calling CollectionCatalog::write.
            return;
        }
        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            // First do a pass to check that we are not conflicting with any namespace that we are
            // trying to create.
            for (auto&& entry : entries) {
                if (entry.action == UncommittedCatalogUpdates::Entry::Action::kCreatedCollection) {
                    catalog._ensureNamespaceDoesNotExist(
                        opCtx, entry.collection->ns(), NamespaceType::kAll);
                }
            }

            // We did not conflict with any namespace, mark all the collections as pending commit.
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

            // Mark that we've successfully run preCommit, this allows rollback to clean up the
            // collections marked as pending commit. We need to make sure we do not clean anything
            // up for other transactions.
            uncommittedCatalogUpdates.markPrecommitted();
        });
        hangAfterPreCommittingCatalogUpdates.execute([&](const BSONObj& data) {
            const auto& millis = data.getField(kDelayEntireCommitFailpointField);
            if (millis.ok()) {
                LOGV2(9369501,
                      "hangAfterPreCommittingCatalogUpdates causing a preCommit handler delay",
                      "millis"_attr = millis.numberInt());
                sleepFor(Milliseconds{millis.numberInt()});
            } else {
                hangAfterPreCommittingCatalogUpdates.pauseWhileSet();
            }
        });
    }

    void commit(OperationContext* opCtx, boost::optional<Timestamp> commitTime) noexcept override {
        boost::container::small_vector<unique_function<void(CollectionCatalog&)>, kNumStaticActions>
            writeJobs;

        // Create catalog write jobs for all updates registered in this WriteUnitOfWork
        auto entries = _uncommittedCatalogUpdates.releaseEntries();
        hangBeforePublishingCatalogUpdates.executeIf(
            [&](const BSONObj& data) {
                const auto millis = data.getField(kDelayEntireCommitFailpointField).numberInt();
                LOGV2(9369500,
                      "hangBeforePublishingCatalogUpdates causing a commit handler delay",
                      "millis"_attr = millis);
                sleepFor(Milliseconds{millis});
            },
            [&](const BSONObj& data) {
                return data.getField(kDelayEntireCommitFailpointField).ok();
            });
        for (auto&& entry : entries) {
            hangBeforePublishingCatalogUpdates.executeIf(
                [&](const BSONObj& data) {
                    LOGV2(
                        9089303, "hangBeforePublishingCatalogUpdates enabled", logAttrs(entry.nss));
                    hangBeforePublishingCatalogUpdates.pauseWhileSet();
                },
                [&](const BSONObj& data) {
                    // We've already paused before, no need to pause again.
                    if (data.getField(kDelayEntireCommitFailpointField).ok())
                        return false;

                    const auto tenantField = data.getField("tenant");
                    const auto tenantId = tenantField.ok()
                        ? boost::optional<TenantId>(TenantId::parseFromBSON(tenantField))
                        : boost::none;
                    const auto fpNss =
                        NamespaceStringUtil::parseFailPointData(data, "collectionNS", tenantId);
                    return fpNss.isEmpty() || entry.nss == fpNss;
                });
            switch (entry.action) {
                case UncommittedCatalogUpdates::Entry::Action::kWritableCollection: {
                    writeJobs.push_back([collection = std::move(entry.collection),
                                         commitTime](CollectionCatalog& catalog) {
                        setCollectionInCatalog(catalog, std::move(collection), commitTime);
                    });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kRenamedCollection: {
                    writeJobs.push_back(
                        [opCtx, &from = entry.nss, &to = entry.renameTo, commitTime](
                            CollectionCatalog& catalog) {
                            // We just need to do modifications on 'from' here. 'to' is taken care
                            // of by a separate kWritableCollection entry.
                            catalog._collections = catalog._collections.erase(from);
                            catalog._pendingCommitNamespaces =
                                catalog._pendingCommitNamespaces.erase(from);

                            auto& resourceCatalog = ResourceCatalog::get();
                            resourceCatalog.remove({RESOURCE_COLLECTION, from}, from);
                            resourceCatalog.add({RESOURCE_COLLECTION, to}, to);

                            catalog._catalogIdTracker.rename(from, to, commitTime);
                        });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kDroppedCollection: {
                    writeJobs.push_back(
                        [opCtx, uuid = *entry.uuid(), commitTime](CollectionCatalog& catalog) {
                            catalog.deregisterCollection(opCtx, uuid, commitTime);
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
                                                    std::move(collection),
                                                    /*ts=*/commitTime);
                    });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kCreatedCollection: {
                    writeJobs.push_back([opCtx,
                                         collection = std::move(entry.collection),
                                         commitTime](CollectionCatalog& catalog) {
                        catalog._registerCollection(opCtx, std::move(collection), commitTime);
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
                        ResourceCatalog::get().add({RESOURCE_COLLECTION, viewName}, viewName);
                        catalog.deregisterUncommittedView(viewName);
                    });
                    break;
                }
                case UncommittedCatalogUpdates::Entry::Action::kRemoveViewResource: {
                    writeJobs.push_back([opCtx, &viewName = entry.nss](CollectionCatalog& catalog) {
                        ResourceCatalog::get().remove({RESOURCE_COLLECTION, viewName}, viewName);
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

    void rollback(OperationContext* opCtx) noexcept override {
        auto entries = _uncommittedCatalogUpdates.releaseEntries();

        // Skip rollback logic if we failed to preCommit this transaction. We must make sure we
        // don't clean anything up for other transactions.
        if (!_uncommittedCatalogUpdates.hasPrecommitted()) {
            return;
        }

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

CollectionCatalog::iterator::iterator(const DatabaseName& dbName,
                                      OrderedCollectionMap::iterator it,
                                      const OrderedCollectionMap& map)
    : _map{map}, _mapIter{it} {}

CollectionCatalog::iterator::value_type CollectionCatalog::iterator::operator*() {
    if (_mapIter == _map.end()) {
        return nullptr;
    }
    return _mapIter->second.get();
}

CollectionCatalog::iterator CollectionCatalog::iterator::operator++() {
    invariant(_mapIter != _map.end());
    ++_mapIter;
    return *this;
}

bool CollectionCatalog::iterator::operator==(const iterator& other) const {
    invariant(_map == other._map);

    if (other._mapIter == other._map.end()) {
        return _mapIter == _map.end();
    } else if (_mapIter == _map.end()) {
        return other._mapIter == other._map.end();
    }

    return _mapIter->first.second == other._mapIter->first.second;
}

bool CollectionCatalog::iterator::operator!=(const iterator& other) const {
    return !(*this == other);
}

CollectionCatalog::Range::Range(const OrderedCollectionMap& map, const DatabaseName& dbName)
    : _map{map}, _dbName{dbName} {}

CollectionCatalog::iterator CollectionCatalog::Range::begin() const {
    return {_dbName, _map.lower_bound(std::make_pair(_dbName, minUuid)), _map};
}

CollectionCatalog::iterator CollectionCatalog::Range::end() const {
    return {_dbName, _map.upper_bound(std::make_pair(_dbName, maxUuid)), _map};
}

bool CollectionCatalog::Range::empty() const {
    return begin() == end();
}

std::shared_ptr<const CollectionCatalog> CollectionCatalog::latest(ServiceContext* svcCtx) {
    return getCatalogStore(svcCtx).load();
}

std::shared_ptr<const CollectionCatalog> CollectionCatalog::get(OperationContext* opCtx) {
    const auto& stashed = stashedCatalog(shard_role_details::getRecoveryUnit(opCtx)->getSnapshot());
    if (stashed)
        return stashed;

    return latest(opCtx);
}

std::shared_ptr<const CollectionCatalog> CollectionCatalog::latest(OperationContext* opCtx) {
    return latest(opCtx->getServiceContext());
}

void CollectionCatalog::stash(OperationContext* opCtx,
                              std::shared_ptr<const CollectionCatalog> catalog) {
    stashedCatalog(shard_role_details::getRecoveryUnit(opCtx)->getSnapshot()) = std::move(catalog);
}

void CollectionCatalog::write(ServiceContext* svcCtx, CatalogWriteFn job) {
    auto& storage = getCatalogStore(svcCtx);
    storage.write([&](auto& catalog) {
        auto clone = std::make_shared<CollectionCatalog>(catalog);
        job(*clone);
        return clone;
    });
}

void CollectionCatalog::write(OperationContext* opCtx, CatalogWriteFn job) {
    write(opCtx->getServiceContext(), job);
}

Status CollectionCatalog::createView(OperationContext* opCtx,
                                     const NamespaceString& viewName,
                                     const NamespaceString& viewOn,
                                     const BSONArray& pipeline,
                                     const ViewsForDatabase::PipelineValidatorFn& validatePipeline,
                                     const BSONObj& collation,
                                     ViewsForDatabase::Durability durability) const {
    invariant(durability == ViewsForDatabase::Durability::kAlreadyDurable ||
              shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(viewName, MODE_IX));
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
        NamespaceString::makeSystemDotViewsNamespace(viewName.dbName()), MODE_X));

    auto optViewsForDB = _getViewsForDatabase(opCtx, viewName.dbName());
    if (!optViewsForDB) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "cannot create view on non existing database "
                                    << viewName.toStringForErrorMsg());
    }
    const ViewsForDatabase& viewsForDb = *optViewsForDB;

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    if (uncommittedCatalogUpdates.shouldIgnoreExternalViewChanges(viewName.dbName())) {
        return Status::OK();
    }

    if (!viewName.isEqualDb(viewOn))
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    if (viewsForDb.lookup(viewName) || _collections.find(viewName))
        return Status(ErrorCodes::NamespaceExists, "Namespace already exists");

    if (!NamespaceString::validCollectionName(viewOn.coll()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid name for 'viewOn': " << viewOn.coll());

    IgnoreExternalViewChangesForDatabase ignore(opCtx, viewName.dbName());

    assertViewCatalogValid(viewsForDb);
    // The system.views is a special collection that is always present in the catalog and can't be
    // modified or dropped. The Collection* returned by the lookup can't disappear. The
    // initialization here is therefore safe.
    CollectionPtr systemViews =
        CollectionPtr::CollectionPtr_UNSAFE(_lookupSystemViews(opCtx, viewName.dbName()));

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
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(viewName, MODE_X));
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
        NamespaceString::makeSystemDotViewsNamespace(viewName.dbName()), MODE_X));

    auto optViewsForDB = _getViewsForDatabase(opCtx, viewName.dbName());
    if (!optViewsForDB) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "cannot modify view on non existing database "
                                    << viewName.toStringForErrorMsg());
    }
    const ViewsForDatabase& viewsForDb = *optViewsForDB;

    if (!viewName.isEqualDb(viewOn))
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    auto viewPtr = viewsForDb.lookup(viewName);
    if (!viewPtr)
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream()
                          << "cannot modify missing view " << viewName.toStringForErrorMsg());

    if (!NamespaceString::validCollectionName(viewOn.coll()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid name for 'viewOn': " << viewOn.coll());

    IgnoreExternalViewChangesForDatabase ignore(opCtx, viewName.dbName());

    assertViewCatalogValid(viewsForDb);
    auto systemViews = _lookupSystemViews(opCtx, viewName.dbName());

    ViewsForDatabase writable{viewsForDb};
    // The system.views is a special collection that is always present in the catalog and can't be
    // modified or dropped. The Collection* returned by the lookup can't disappear. The
    // initialization here is therefore safe.
    auto status = writable.update(opCtx,
                                  CollectionPtr::CollectionPtr_UNSAFE(systemViews),
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
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(viewName, MODE_IX));
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
        NamespaceString::makeSystemDotViewsNamespace(viewName.dbName()), MODE_X));

    auto optViewsForDB = _getViewsForDatabase(opCtx, viewName.dbName());
    if (!optViewsForDB) {
        // If the database does not exist, the view does not exist either
        return Status::OK();
    }
    const ViewsForDatabase& viewsForDb = *optViewsForDB;

    assertViewCatalogValid(viewsForDb);
    if (!viewsForDb.lookup(viewName)) {
        return Status::OK();
    }

    Status result = Status::OK();
    {
        IgnoreExternalViewChangesForDatabase ignore(opCtx, viewName.dbName());

        // The system.views is a special collection that is always present in the catalog and can't
        // be modified or dropped. The Collection* returned by the lookup can't disappear. The
        // initialization here is therefore safe.
        CollectionPtr systemViews =
            CollectionPtr::CollectionPtr_UNSAFE(_lookupSystemViews(opCtx, viewName.dbName()));

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
    invariantHasExclusiveAccessToCollection(opCtx,
                                            NamespaceString::makeSystemDotViewsNamespace(dbName));

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    if (uncommittedCatalogUpdates.shouldIgnoreExternalViewChanges(dbName)) {
        return;
    }

    LOGV2_DEBUG(22546, 1, "Reloading view catalog for database", logAttrs(dbName));

    uncommittedCatalogUpdates.replaceViewsForDatabase(dbName,
                                                      loadViewsForDatabase(opCtx, *this, dbName));
    PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);
}

ConsistentCollection CollectionCatalog::establishConsistentCollection(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nssOrUUID,
    boost::optional<Timestamp> readTimestamp) const {
    // Return any previously instantiated collection for this snapshot
    if (auto instantiatedColl = _findInstantiatedCollectionByNamespaceOrUUID(opCtx, nssOrUUID)) {
        return ConsistentCollection{opCtx, instantiatedColl->get()};
    }

    if (_needsOpenCollection(opCtx, nssOrUUID, readTimestamp)) {
        auto coll = _openCollection(opCtx, nssOrUUID, readTimestamp);
        return ConsistentCollection{opCtx, coll};
    }

    auto coll = _lookupCollectionByNamespaceOrUUIDNoFindInstantiated(nssOrUUID).get();
    return ConsistentCollection{opCtx, coll};
}

std::vector<ConsistentCollection> CollectionCatalog::establishConsistentCollections(
    OperationContext* opCtx, const DatabaseName& dbName) const {
    std::vector<ConsistentCollection> result;
    stdx::unordered_set<const Collection*> visitedCollections;
    auto appendIfUnique = [&result, &visitedCollections](ConsistentCollection coll) {
        auto [_, isNewCollection] = visitedCollections.emplace(coll.get());
        if (coll && isNewCollection) {
            result.emplace_back(std::move(coll));
        }
    };

    // We iterate both already committed and uncommitted changes and validate them with
    // the storage snapshot.
    for (const auto& coll : range(dbName)) {
        auto currentCollection = establishConsistentCollection(opCtx, coll->ns(), boost::none);
        appendIfUnique(std::move(currentCollection));
    }

    for (auto const& [ns, coll] : _pendingCommitNamespaces) {
        if (ns.dbName() == dbName) {
            auto currentCollection = establishConsistentCollection(opCtx, ns, boost::none);
            appendIfUnique(std::move(currentCollection));
        }
    }

    return result;
}

bool CollectionCatalog::_collectionHasPendingCommits(const NamespaceStringOrUUID& nssOrUUID) const {
    if (nssOrUUID.isNamespaceString()) {
        return _pendingCommitNamespaces.find(nssOrUUID.nss());
    } else {
        return _pendingCommitUUIDs.find(nssOrUUID.uuid());
    }
}

bool CollectionCatalog::_needsOpenCollection(OperationContext* opCtx,
                                             const NamespaceStringOrUUID& nsOrUUID,
                                             boost::optional<Timestamp> readTimestamp) const {
    if (readTimestamp) {
        auto coll = lookupCollectionByNamespaceOrUUID(opCtx, nsOrUUID);

        // If the collections doesn't exist then we have to reinstantiate it from the WT snapshot.
        if (!coll)
            return true;

        // Otherwise we only verify that the collection is valid for the given timestamp.
        return *readTimestamp < coll->getMinimumValidSnapshot();
    } else {
        return _collectionHasPendingCommits(nsOrUUID);
    }
}

const Collection* CollectionCatalog::_openCollection(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nssOrUUID,
    boost::optional<Timestamp> readTimestamp) const {
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
        if (nssOrUUID.isNamespaceString()) {
            const std::shared_ptr<Collection>* pending =
                _pendingCommitNamespaces.find(nssOrUUID.nss());
            invariant(pending);
            return *pending;
        }

        const std::shared_ptr<Collection>* pending = _pendingCommitUUIDs.find(nssOrUUID.uuid());
        invariant(pending);
        return *pending;
    }();

    auto latestCollection = [&]() -> std::shared_ptr<const Collection> {
        if (nssOrUUID.isNamespaceString()) {
            return _getCollectionByNamespace(opCtx, nssOrUUID.nss());
        }
        return _getCollectionByUUID(opCtx, nssOrUUID.uuid());
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

    auto catalogEntry =
        durable_catalog::getParsedCatalogEntry(opCtx, catalogId, MDBCatalog::get(opCtx));

    const NamespaceString& nss = [&]() {
        if (nssOrUUID.isNamespaceString()) {
            return nssOrUUID.nss();
        }
        return latestCollection ? latestCollection->ns() : pendingCollection->ns();
    }();

    const UUID uuid = [&]() {
        if (nssOrUUID.isUUID()) {
            return nssOrUUID.uuid();
        }

        // If pendingCollection is nullptr, the collection is being dropped, so latestCollection
        // must be non-nullptr and must contain a uuid.
        return pendingCollection ? pendingCollection->uuid() : latestCollection->uuid();
    }();

    // If the catalog entry is not found in our snapshot then the collection is being dropped and we
    // can observe the drop. Lookups by this namespace or uuid should not find a collection.
    if (!catalogEntry) {
        // If we performed this lookup by UUID we could be in a case where we're looking up
        // concurrently with a rename with dropTarget=true where the UUID that we use is the target
        // that got dropped. If that rename has committed we need to put the correct collection
        // under open collection for this namespace. We can detect this case by comparing the
        // catalogId with what is pending for this namespace.
        if (nssOrUUID.isUUID()) {
            const std::shared_ptr<Collection>* found = _pendingCommitNamespaces.find(nss);
            // When openCollection is called with no timestamp, the namespace must be pending
            // commit.
            invariant(found);
            auto& pending = *found;
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
    NamespaceString nsInDurableCatalog = catalogEntry->metadata->nss;
    if (nssOrUUID.isNamespaceString() && nss != nsInDurableCatalog) {
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
    if (nssOrUUID.isUUID() && latestCollection && pendingCollection &&
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

    auto metadataObj = catalogEntry->metadata->toBSON();

    if (latestCollection && latestCollection->isMetadataEqual(metadataObj)) {
        openedCollections.store(latestCollection, nss, uuid);
        return latestCollection.get();
    }

    // Use the pendingCollection if there is no latestCollection or if the metadata of the
    // latestCollection doesn't match the durable catalogEntry.
    if (pendingCollection && pendingCollection->isMetadataEqual(metadataObj)) {
        // If the latest collection doesn't exist then the pending collection must exist as it's
        // being created in this snapshot. Otherwise, if the latest collection is incompatible
        // with this snapshot, then the change came from an uncommitted update by an operation
        // operating on this snapshot. If both latestCollection and pendingCollection exists check
        // if their uuid differs in which case this is a rename with dropTarget=true that just
        // committed.
        if (pendingCollection && latestCollection &&
            pendingCollection->uuid() != latestCollection->uuid()) {
            openedCollections.store(nullptr, boost::none, latestCollection->uuid());
        }
        openedCollections.store(pendingCollection, nss, uuid);
        return pendingCollection.get();
    }

    // If neither `latestCollection` or `pendingCollection` match the metadata we fully instantiate
    // a new collection instance from durable storage that is guaranteed to match. This can happen
    // when multikey is not consistent with the storage snapshot. We use 'pendingCollection' as the
    // base when available as it might contain an index that is about to be added. Dropped indexes
    // can be found through other means in the drop pending state.
    invariant(latestCollection || pendingCollection);
    auto durableCatalogEntry =
        durable_catalog::getParsedCatalogEntry(opCtx, catalogId, MDBCatalog::get(opCtx));
    invariant(durableCatalogEntry);
    auto compatibleCollection =
        _createCompatibleCollection(opCtx,
                                    pendingCollection ? pendingCollection : latestCollection,
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
        openedCollections.store(
            nullptr,
            [&]() -> boost::optional<NamespaceString> {
                if (nssOrUUID.isNamespaceString()) {
                    return nssOrUUID.nss();
                }
                return boost::none;
            }(),
            [&]() -> boost::optional<UUID> {
                if (nssOrUUID.isUUID()) {
                    return nssOrUUID.uuid();
                }
                return boost::none;
            }());
        return nullptr;
    }

    auto latestCollection =
        _lookupCollectionByUUIDNoFindInstantiated(*catalogEntry->metadata->options.uuid);

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

    openedCollections.store(
        nullptr,
        [nssOrUUID]() -> boost::optional<NamespaceString> {
            if (nssOrUUID.isNamespaceString()) {
                return nssOrUUID.nss();
            }
            return boost::none;
        }(),
        [nssOrUUID]() -> boost::optional<UUID> {
            if (nssOrUUID.isUUID()) {
                return nssOrUUID.uuid();
            }
            return boost::none;
        }());
    return nullptr;
}

boost::optional<durable_catalog::CatalogEntry> CollectionCatalog::_fetchPITCatalogEntry(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nssOrUUID,
    boost::optional<Timestamp> readTimestamp) const {
    auto [catalogId, result] = nssOrUUID.isNamespaceString()
        ? _catalogIdTracker.lookup(nssOrUUID.nss(), readTimestamp)
        : _catalogIdTracker.lookup(nssOrUUID.uuid(), readTimestamp);
    if (result == HistoricalCatalogIdTracker::LookupResult::Existence::kNotExists) {
        return boost::none;
    }

    auto writeCatalogIdAfterScan =
        [&](const boost::optional<durable_catalog::CatalogEntry>& catalogEntry) {
            if (!catalogEntry) {
                if (nssOrUUID.isNamespaceString()) {
                    if (!_catalogIdTracker.canRecordNonExisting(nssOrUUID.nss())) {
                        return;
                    }
                } else {
                    if (!_catalogIdTracker.canRecordNonExisting(nssOrUUID.uuid())) {
                        return;
                    }
                }
            }

            CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
                // Insert catalogId for both the namespace and UUID if the catalog entry is found.
                if (catalogEntry) {
                    catalog._catalogIdTracker.recordExistingAtTime(
                        catalogEntry->metadata->nss,
                        *catalogEntry->metadata->options.uuid,
                        catalogEntry->catalogId,
                        *readTimestamp);
                } else if (nssOrUUID.isNamespaceString()) {
                    catalog._catalogIdTracker.recordNonExistingAtTime(nssOrUUID.nss(),
                                                                      *readTimestamp);
                } else {
                    catalog._catalogIdTracker.recordNonExistingAtTime(nssOrUUID.uuid(),
                                                                      *readTimestamp);
                }
            });
        };

    auto mdbCatalog = MDBCatalog::get(opCtx);
    if (result == HistoricalCatalogIdTracker::LookupResult::Existence::kUnknown) {
        // We shouldn't receive kUnknown when we don't have a timestamp since no timestamp means
        // we're operating on the latest.
        invariant(readTimestamp);

        // Scan durable catalog when we don't have accurate catalogId mapping for this timestamp.
        gCollectionCatalogSection.numScansDueToMissingMapping.fetchAndAddRelaxed(1);
        auto catalogEntry = nssOrUUID.isNamespaceString()
            ? durable_catalog::scanForCatalogEntryByNss(opCtx, nssOrUUID.nss(), mdbCatalog)
            : durable_catalog::scanForCatalogEntryByUUID(opCtx, nssOrUUID.uuid(), mdbCatalog);
        writeCatalogIdAfterScan(catalogEntry);
        return catalogEntry;
    }

    auto catalogEntry = durable_catalog::getParsedCatalogEntry(opCtx, catalogId, mdbCatalog);
    if (!catalogEntry ||
        (nssOrUUID.isNamespaceString() && nssOrUUID.nss() != catalogEntry->metadata->nss)) {
        invariant(readTimestamp);
        // If no entry is found or the entry contains a different namespace, the mapping might be
        // incorrect since it is incomplete after startup; scans durable catalog to confirm.
        auto catalogEntry = nssOrUUID.isNamespaceString()
            ? durable_catalog::scanForCatalogEntryByNss(opCtx, nssOrUUID.nss(), mdbCatalog)
            : durable_catalog::scanForCatalogEntryByUUID(opCtx, nssOrUUID.uuid(), mdbCatalog);
        writeCatalogIdAfterScan(catalogEntry);
        return catalogEntry;
    }
    return catalogEntry;
}

std::shared_ptr<Collection> CollectionCatalog::_createCompatibleCollection(
    OperationContext* opCtx,
    const std::shared_ptr<const Collection>& latestCollection,
    boost::optional<Timestamp> readTimestamp,
    const durable_catalog::CatalogEntry& catalogEntry) const {
    if (!latestCollection) {
        return nullptr;
    }

    if (latestCollection->getRecordStore()->getIdent() != catalogEntry.ident) {
        // Protect against an edge case where the same namespace / UUID combination is used to
        // create a collection after a drop. In this case, using the shared state in the latest
        // instance would be an error, because the collection at the requested timestamp is not
        // actually the same as the latest, it just happens to have the same namespace and UUID.
        // Even if it were the same "logical" collection, this would still be incorrect because the
        // 'ident' would be different, and the PIT read would be accessing an incorrect ident. The
        // 'ident' is guaranteed to be unique across collection re-creation, and can be used to
        // determine if the shared state is incompatible.
        return nullptr;
    }

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
        collToReturn->initFromExisting(opCtx, latestCollection, catalogEntry, readTimestamp);
    if (!status.isOK()) {
        LOGV2_DEBUG(
            6857100, 1, "Failed to instantiate collection", "reason"_attr = status.reason());
        return nullptr;
    }

    return collToReturn;
}

std::shared_ptr<Collection> CollectionCatalog::_createNewPITCollection(
    OperationContext* opCtx,
    boost::optional<Timestamp> readTimestamp,
    const durable_catalog::CatalogEntry& catalogEntry) const {
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
    const auto nss = catalogEntry.metadata->nss;
    LOGV2_DEBUG(6825401,
                1,
                "Instantiating a new collection",
                logAttrs(nss),
                "ident"_attr = catalogEntry.ident,
                "md"_attr = catalogEntry.metadata->toBSON(),
                "timestamp"_attr = readTimestamp);

    const auto collectionOptions = catalogEntry.metadata->options;
    std::unique_ptr<RecordStore> rs =
        opCtx->getServiceContext()->getStorageEngine()->getEngine()->getRecordStore(
            opCtx,
            nss,
            catalogEntry.ident,
            getRecordStoreOptions(nss, collectionOptions),
            collectionOptions.uuid);

    // Set the ident to the one returned by the ident reaper. This is to prevent the ident from
    // being dropping prematurely.
    rs->setIdent(std::move(newIdent));

    std::shared_ptr<Collection> collToReturn = Collection::Factory::get(opCtx)->make(
        opCtx, nss, catalogEntry.catalogId, catalogEntry.metadata, std::move(rs));
    Status status =
        collToReturn->initFromExisting(opCtx, /*collection=*/nullptr, catalogEntry, readTimestamp);
    if (!status.isOK()) {
        LOGV2_DEBUG(
            6857102, 1, "Failed to instantiate collection", "reason"_attr = status.reason());
        return nullptr;
    }

    return collToReturn;
}

void CollectionCatalog::onCreateCollection(OperationContext* opCtx,
                                           std::shared_ptr<Collection> coll) const {
    invariant(coll);
    const auto& nss = coll->ns();

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    auto [found, existingColl, newColl] = UncommittedCatalogUpdates::lookupCollection(opCtx, nss);
    uassert(31370,
            str::stream() << "collection already exists. ns: " << nss.toStringForErrorMsg(),
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

    if (!storageGlobalParams.repair && nss.isSystemDotViews()) {
        reloadViews(opCtx, nss.dbName());
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

void CollectionCatalog::dropCollection(OperationContext* opCtx, Collection* coll) const {
    invariant(coll);

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    uncommittedCatalogUpdates.dropCollection(coll);

    // Requesting a writable collection normally ensures we have registered PublishCatalogUpdates
    // with the recovery unit. However, when the writable Collection was requested in Inplace mode
    // (or is the oplog) this is not the case. So make sure we are registered in all cases.
    PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);
}

void CollectionCatalog::onCloseDatabase(OperationContext* opCtx, DatabaseName dbName) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_X));
    ResourceCatalog::get().remove({RESOURCE_DATABASE, dbName}, dbName);
    _viewsForDatabase = _viewsForDatabase.erase(dbName);
}

void CollectionCatalog::onCloseCatalog() {
    if (_shadowCatalog) {
        return;
    }

    mongo::stdx::unordered_map<UUID, NamespaceString, UUID::Hash> shadowCatalog;
    shadowCatalog.reserve(_catalog.size());
    for (auto& entry : _catalog)
        shadowCatalog.insert({entry.first, entry.second->ns()});
    _shadowCatalog =
        std::make_shared<const mongo::stdx::unordered_map<UUID, NamespaceString, UUID::Hash>>(
            std::move(shadowCatalog));
}

void CollectionCatalog::onOpenCatalog() {
    invariant(_shadowCatalog);
    _shadowCatalog.reset();
    ++_epoch;
}

uint64_t CollectionCatalog::getEpoch() const {
    return _epoch;
}

CollectionCatalog::Range CollectionCatalog::range(const DatabaseName& dbName) const {
    return {_orderedCollections, dbName};
}

std::shared_ptr<const Collection> CollectionCatalog::_getCollectionByUUID(OperationContext* opCtx,
                                                                          const UUID& uuid) const {
    // Return any previously instantiated collection for this snapshot
    if (auto instantiatedColl = _findInstantiatedCollectionByUUID(opCtx, uuid)) {
        return *instantiatedColl;
    }

    return _lookupCollectionByUUIDNoFindInstantiated(uuid);
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
        invariant(!newColl ||
                      shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IX),
                  nss.toStringForErrorMsg());
        return uncommittedPtr.get();
    }

    std::shared_ptr<Collection> coll = _lookupCollectionByUUIDNoFindInstantiated(uuid);

    if (!coll)
        return nullptr;

    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(coll->ns(), MODE_X));

    auto cloned = coll->clone();
    auto ptr = cloned.get();

    uncommittedCatalogUpdates.writableCollection(std::move(cloned));

    if (shard_role_details::getRecoveryUnit(opCtx)->inUnitOfWork() || opCtx->readOnly()) {
        PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);
    } else {
        invariant(shard_role_details::getLocker(opCtx)->isW(),
                  "either a WriteUnitOfWork or exclusive global lock is expected");
        PublishCatalogUpdates(uncommittedCatalogUpdates).commit(opCtx, boost::none);
    }

    return ptr;
}

const Collection* CollectionCatalog::lookupCollectionByUUID(OperationContext* opCtx,
                                                            UUID uuid) const {
    // Return any previously instantiated collection for this snapshot
    if (auto instantiatedColl = _findInstantiatedCollectionByUUID(opCtx, uuid)) {
        return instantiatedColl->get();
    }

    return _lookupCollectionByUUIDNoFindInstantiated(uuid).get();
}

const Collection* CollectionCatalog::lookupCollectionByNamespaceOrUUID(
    OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) const {
    if (nssOrUUID.isUUID()) {
        return lookupCollectionByUUID(opCtx, nssOrUUID.uuid());
    }

    return lookupCollectionByNamespace(opCtx, nssOrUUID.nss());
}

std::shared_ptr<Collection> CollectionCatalog::_lookupCollectionByNamespaceNoFindInstantiated(
    const NamespaceString& nss) const {
    const std::shared_ptr<Collection>* coll = _collections.find(nss);
    return coll ? *coll : nullptr;
}

std::shared_ptr<Collection> CollectionCatalog::_lookupCollectionByUUIDNoFindInstantiated(
    UUID uuid) const {
    const std::shared_ptr<Collection>* coll = _catalog.find(uuid);
    return coll ? *coll : nullptr;
}

std::shared_ptr<Collection> CollectionCatalog::_lookupCollectionByNamespaceOrUUIDNoFindInstantiated(
    const NamespaceStringOrUUID& nssOrUUID) const {
    if (nssOrUUID.isUUID()) {
        return _lookupCollectionByUUIDNoFindInstantiated(nssOrUUID.uuid());
    }

    return _lookupCollectionByNamespaceNoFindInstantiated(nssOrUUID.nss());
}

std::shared_ptr<const Collection> CollectionCatalog::_getCollectionByNamespace(
    OperationContext* opCtx, const NamespaceString& nss) const {
    // Return any previously instantiated collection for this snapshot
    if (auto instantiatedColl = _findInstantiatedCollectionByNamespace(opCtx, nss)) {
        return *instantiatedColl;
    }

    return _lookupCollectionByNamespaceNoFindInstantiated(nss);
}

Collection* CollectionCatalog::lookupCollectionByNamespaceForMetadataWrite(
    OperationContext* opCtx, const NamespaceString& nss) const {

    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    auto [found, uncommittedPtr, newColl] = UncommittedCatalogUpdates::lookupCollection(opCtx, nss);


    // If uncommittedPtr is valid, found is always true. Return the pointer as the collection still
    // exists.
    if (uncommittedPtr) {
        // If the collection is newly created, invariant on the collection being locked in MODE_IX.
        invariant(!newColl ||
                      shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IX),
                  nss.toStringForErrorMsg());
        return uncommittedPtr.get();
    }

    // Report the drop or rename as nothing new was created.
    if (found) {
        return nullptr;
    }

    const std::shared_ptr<Collection>* collPtr = _collections.find(nss);
    auto coll = collPtr ? *collPtr : nullptr;

    if (!coll)
        return nullptr;

    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_X));

    auto cloned = coll->clone();
    auto ptr = cloned.get();

    uncommittedCatalogUpdates.writableCollection(std::move(cloned));

    if (shard_role_details::getRecoveryUnit(opCtx)->inUnitOfWork() || opCtx->readOnly()) {
        PublishCatalogUpdates::ensureRegisteredWithRecoveryUnit(opCtx, uncommittedCatalogUpdates);
    } else {
        invariant(shard_role_details::getLocker(opCtx)->isW(),
                  "either a WriteUnitOfWork or exclusive global lock is expected");
        PublishCatalogUpdates(uncommittedCatalogUpdates).commit(opCtx, boost::none);
    }

    return ptr;
}

const Collection* CollectionCatalog::lookupCollectionByNamespace(OperationContext* opCtx,
                                                                 const NamespaceString& nss) const {
    // Return any previously instantiated collection for this snapshot
    if (auto instantiatedColl = _findInstantiatedCollectionByNamespace(opCtx, nss)) {
        return instantiatedColl->get();
    }

    return _lookupCollectionByNamespaceNoFindInstantiated(nss).get();
}

boost::optional<NamespaceString> CollectionCatalog::lookupNSSByUUID(OperationContext* opCtx,
                                                                    const UUID& uuid) const {
    return _lookupNSSByUUID(opCtx, uuid, false);
}

boost::optional<NamespaceString> CollectionCatalog::_lookupNSSByUUID(OperationContext* opCtx,
                                                                     const UUID& uuid,
                                                                     bool withCommitPending) const {
    // Return any previously instantiated collection for this snapshot
    if (auto instantiatedColl = _findInstantiatedCollectionByUUID(opCtx, uuid)) {
        if (const auto collPtr = instantiatedColl->get()) {
            return collPtr->ns();
        }
        return boost::none;
    }

    if (withCommitPending) {
        if (const auto collPtr = _pendingCommitUUIDs.find(uuid); collPtr && *collPtr) {
            auto coll = *collPtr;
            return coll->ns();
        }
    }

    if (const auto collPtr = _catalog.find(uuid)) {
        auto coll = *collPtr;
        return coll->ns();
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
    // Return any previously instantiated collection for this snapshot
    if (auto instantiatedColl = _findInstantiatedCollectionByNamespace(opCtx, nss)) {
        if (const auto collPtr = instantiatedColl->get()) {
            return collPtr->uuid();
        }
        return boost::none;
    }

    const std::shared_ptr<Collection>* collPtr = _collections.find(nss);
    if (collPtr) {
        auto coll = *collPtr;
        return coll->uuid();
    }
    return boost::none;
}

bool CollectionCatalog::checkIfUUIDExistsAtLatest(OperationContext* opCtx, UUID uuid) const {
    // First look within uncommitted collection creations performed under the currently open storage
    // transaction (if any)...
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    const auto& entries = uncommittedCatalogUpdates.entries();
    auto entriesIt = std::find_if(
        entries.begin(), entries.end(), [&uuid](const UncommittedCatalogUpdates::Entry& entry) {
            return (entry.collection && entry.collection->uuid() == uuid);
        });
    if (entriesIt != entries.end()) {
        return true;
    }

    // Then within the catalog instance itself...
    auto* coll = _catalog.find(uuid);
    if (coll) {
        return true;
    }

    // ... and finally within not-yet flushed past commits.
    coll = _pendingCommitUUIDs.find(uuid);
    return coll != nullptr;
}

bool CollectionCatalog::isLatestCollection(OperationContext* opCtx,
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
    if (!coll) {
        // If there is nothing in the main catalog check for pending commit, we could have just
        // committed a newly created collection which would be considered latest.
        coll = _pendingCommitUUIDs.find(collection->uuid());
        if (!coll || !coll->get()) {
            return false;
        }
    }

    return coll->get() == collection;
}

void CollectionCatalog::ensureCollectionIsNew(OperationContext* opCtx,
                                              const NamespaceString& nss) const {
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    const auto& entries = uncommittedCatalogUpdates.entries();
    auto hasUncommittedCreateEntry = std::any_of(
        entries.begin(), entries.end(), [&](const UncommittedCatalogUpdates::Entry& entry) {
            return entry.action == UncommittedCatalogUpdates::Entry::Action::kCreatedCollection &&
                entry.nss == nss;
        });
    invariant(hasUncommittedCreateEntry);
    _ensureNamespaceDoesNotExist(opCtx, nss, NamespaceType::kAll);
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
        if (!NamespaceString::validCollectionName(NamespaceStringUtil::serializeForCatalog(ns))) {
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
    OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID) const {
    if (nsOrUUID.isNamespaceString()) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace " << nsOrUUID.toStringForErrorMsg()
                              << " is not a valid collection name",
                nsOrUUID.nss().isValid());
        return nsOrUUID.nss();
    }

    return _resolveNamespaceStringFromDBNameAndUUID(
        opCtx, nsOrUUID.dbName(), nsOrUUID.uuid(), false);
}

NamespaceString CollectionCatalog::resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
    OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID) const {
    if (nsOrUUID.isNamespaceString()) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace " << nsOrUUID.toStringForErrorMsg()
                              << " is not a valid collection name",
                nsOrUUID.nss().isValid());
        return nsOrUUID.nss();
    }

    return _resolveNamespaceStringFromDBNameAndUUID(
        opCtx, nsOrUUID.dbName(), nsOrUUID.uuid(), true);
}

NamespaceString CollectionCatalog::resolveNamespaceStringFromDBNameAndUUID(
    OperationContext* opCtx, const DatabaseName& dbName, const UUID& uuid) const {
    return _resolveNamespaceStringFromDBNameAndUUID(opCtx, dbName, uuid, false);
}

NamespaceString CollectionCatalog::_resolveNamespaceStringFromDBNameAndUUID(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const UUID& uuid,
    bool withCommitPending) const {
    auto resolvedNss = _lookupNSSByUUID(opCtx, uuid, withCommitPending);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Unable to resolve " << uuid.toString(),
            resolvedNss && resolvedNss->isValid());

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "UUID: " << uuid.toString()
                          << " specified in provided db name: " << dbName.toStringForErrorMsg()
                          << " resolved to a collection in a different database, resolved nss: "
                          << (*resolvedNss).toStringForErrorMsg(),
            resolvedNss->dbName() == dbName);
    return std::move(*resolvedNss);
}

boost::optional<std::shared_ptr<const Collection>>
CollectionCatalog::_findInstantiatedCollectionByNamespace(OperationContext* opCtx,
                                                          const NamespaceString& nss) const {
    // It's important to look in UncommittedCatalogUpdates before OpenedCollections because in a
    // multi-document transaction it's permitted to perform a lookup on a non-existent
    // collection followed by creating the collection. This lookup will store a nullptr in
    // OpenedCollections.
    auto [found, uncommittedColl, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, nss);
    if (uncommittedColl) {
        return std::shared_ptr<const Collection>(std::move(uncommittedColl));
    }

    // Report the drop or rename as nothing new was created.
    if (found) {
        return std::shared_ptr<const Collection>(nullptr);
    }

    // Return any previously instantiated collection for this snapshot
    if (auto openedColl = OpenedCollections::get(opCtx).lookupByNamespace(nss)) {
        return openedColl.value();
    }

    return boost::none;
}

boost::optional<std::shared_ptr<const Collection>>
CollectionCatalog::_findInstantiatedCollectionByUUID(OperationContext* opCtx,
                                                     const UUID& uuid) const {
    // If UUID is managed by UncommittedCatalogUpdates (but not newly created) return the pointer
    // which will be nullptr in case of a drop. It's important to look in UncommittedCatalogUpdates
    // before OpenedCollections because in a multi-document transaction it's permitted to perform a
    // lookup on a non-existent collection followed by creating the collection. This lookup will
    // store a nullptr in OpenedCollections.
    auto [found, uncommittedColl, newColl] =
        UncommittedCatalogUpdates::lookupCollection(opCtx, uuid);
    if (uncommittedColl) {
        return std::shared_ptr<const Collection>(std::move(uncommittedColl));
    }

    // Report the drop or rename as nothing new was created.
    if (found) {
        return std::shared_ptr<const Collection>(nullptr);
    }

    // Return any previously instantiated collection for this snapshot
    if (auto openedColl = OpenedCollections::get(opCtx).lookupByUUID(uuid)) {
        return openedColl.value();
    }

    return boost::none;
}

boost::optional<std::shared_ptr<const Collection>>
CollectionCatalog::_findInstantiatedCollectionByNamespaceOrUUID(
    OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) const {
    if (nssOrUUID.isUUID()) {
        return _findInstantiatedCollectionByUUID(opCtx, nssOrUUID.uuid());
    }

    return _findInstantiatedCollectionByNamespace(opCtx, nssOrUUID.nss());
}

bool CollectionCatalog::checkIfCollectionSatisfiable(UUID uuid,
                                                     const CollectionInfoFn& predicate) const {
    invariant(predicate);

    auto collection = _lookupCollectionByUUIDNoFindInstantiated(uuid);

    if (!collection) {
        return false;
    }

    return predicate(collection.get());
}

std::vector<UUID> CollectionCatalog::getAllCollectionUUIDsFromDb(const DatabaseName& dbName) const {
    auto it = _orderedCollections.lower_bound(std::make_pair(dbName, minUuid));

    std::vector<UUID> ret;
    while (it != _orderedCollections.end() && it->first.first == dbName) {
        ret.push_back(it->first.second);
        ++it;
    }
    return ret;
}

std::vector<NamespaceString> CollectionCatalog::getAllCollectionNamesFromDb(
    OperationContext* opCtx, const DatabaseName& dbName) const {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_S));

    std::vector<NamespaceString> ret;
    for (auto it = _orderedCollections.lower_bound(std::make_pair(dbName, minUuid));
         it != _orderedCollections.end() && it->first.first == dbName;
         ++it) {
        ret.push_back(it->second->ns());
    }
    return ret;
}

Status CollectionCatalog::_iterAllDbNamesHelper(
    const boost::optional<TenantId>& tenantId,
    const std::function<Status(const DatabaseName&)>& callback,
    const std::function<std::pair<DatabaseName, UUID>(const DatabaseName&)>& nextUpperBound) const {
    // _orderedCollections is sorted by <dbName, uuid>. upper_bound will return the iterator to the
    // first element in _orderedCollections greater than <firstDbName, maxUuid>.
    auto iter = _orderedCollections.upper_bound(std::make_pair(
        DatabaseNameUtil::deserialize(
            tenantId, "", SerializationContext(SerializationContext::Source::Catalog)),
        maxUuid));
    while (iter != _orderedCollections.end()) {
        auto dbName = iter->first.first;
        if (tenantId && dbName.tenantId() != tenantId) {
            break;
        }

        auto status = callback(dbName);
        if (!status.isOK()) {
            return status;
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
            return std::make_pair(DatabaseNameUtil::deserialize(
                                      dbName.tenantId(),
                                      "\xff",
                                      SerializationContext(SerializationContext::Source::Catalog)),
                                  maxUuid);
        });
    return ret;
}

std::vector<DatabaseName> CollectionCatalog::getAllConsistentDbNames(
    OperationContext* opCtx) const {
    return getAllConsistentDbNamesForTenant(opCtx, boost::none);
}

std::vector<DatabaseName> CollectionCatalog::getAllConsistentDbNamesForTenant(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) const {
    // The caller must have an active storage snapshot
    tassert(9089300,
            "cannot get database list consistent to a snapshot without an active snapshot",
            shard_role_details::getRecoveryUnit(opCtx)->isActive());

    // First get the dbnames that are not pending commit
    std::vector<DatabaseName> ret = getAllDbNamesForTenant(tenantId);
    stdx::unordered_set<DatabaseName> visitedDBs(ret.begin(), ret.end());
    auto insertSortedIfUnique = [&ret, &visitedDBs](DatabaseName dbname) {
        auto [_, isNewDB] = visitedDBs.emplace(dbname);
        if (isNewDB) {
            ret.insert(std::lower_bound(ret.begin(), ret.end(), dbname), dbname);
        }
    };

    // Now iterate over uncommitted list and validate against the storage snapshot.
    // Only consider databases we have not seen so far.
    auto readTimestamp = shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
    tassert(9089301,
            "point in time catalog lookup for a database list is not supported",
            RecoveryUnit::ReadSource::kNoTimestamp ==
                shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource());
    for (auto const& [ns, coll] : _pendingCommitNamespaces) {
        if (!visitedDBs.contains(ns.dbName())) {
            if (establishConsistentCollection(opCtx, ns, readTimestamp)) {
                insertSortedIfUnique(ns.dbName());
            }
        }
    }

    return ret;
}

void CollectionCatalog::addDropPending(const DatabaseName& dbName) {
    _dropPendingDatabases = _dropPendingDatabases.insert(dbName);
}

void CollectionCatalog::removeDropPending(const DatabaseName& dbName) {
    _dropPendingDatabases = _dropPendingDatabases.erase(dbName);
}

bool CollectionCatalog::isDropPending(const DatabaseName& dbName) const {
    return _dropPendingDatabases.count(dbName);
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
                                           std::shared_ptr<Collection> coll,
                                           boost::optional<Timestamp> commitTime) {
    invariant(shard_role_details::getLocker(opCtx)->isW());

    const auto& nss = coll->ns();

    _ensureNamespaceDoesNotExist(opCtx, coll->ns(), NamespaceType::kAll);
    _registerCollection(opCtx, coll, commitTime);

    if (!storageGlobalParams.repair && coll->ns().isSystemDotViews()) {
        _viewsForDatabase =
            _viewsForDatabase.set(nss.dbName(), loadViewsForDatabase(opCtx, *this, nss.dbName()));
    }
}

void CollectionCatalog::_registerCollection(OperationContext* opCtx,
                                            std::shared_ptr<Collection> coll,
                                            boost::optional<Timestamp> commitTime) {
    const auto& nss = coll->ns();
    auto uuid = coll->uuid();

    LOGV2_DEBUG(20280, 1, "Registering collection", logAttrs(nss), "uuid"_attr = uuid);

    auto dbIdPair = std::make_pair(nss.dbName(), uuid);

    // Make sure no entry related to this uuid.
    invariant(!_catalog.find(uuid));
    invariant(_orderedCollections.find(dbIdPair) == _orderedCollections.end());

    _catalog = _catalog.set(uuid, coll);
    _collections = _collections.set(nss, coll);
    _orderedCollections = _orderedCollections.set(dbIdPair, coll);
    _pendingCommitNamespaces = _pendingCommitNamespaces.erase(nss);
    _pendingCommitUUIDs = _pendingCommitUUIDs.erase(uuid);

    if (commitTime) {
        coll->setMinimumValidSnapshot(commitTime.value());
    }

    const auto allowMixedModeWrites = coll->getSharedDecorations() &&
        historicalIDTrackerAllowsMixedModeWrites(coll->getSharedDecorations()).load();

    // When restarting from standalone mode to a replica set, the stable timestamp may be null.
    // We still need to register the nss and UUID with the catalog.
    _catalogIdTracker.create(nss, uuid, coll->getCatalogId(), commitTime, allowMixedModeWrites);


    if (!nss.isOnInternalDb()) {
        if (nss.isSystem()) {
            _stats.internal += 1;
        } else if (coll->isTimeseriesCollection() && coll->isNewTimeseriesWithoutView()) {
            _stats.userTimeseries += 1;
        } else {
            _stats.userCollections += 1;
            if (coll->isCapped()) {
                _stats.userCapped += 1;
            }
            if (coll->isClustered()) {
                _stats.userClustered += 1;
            }
            if (coll->getCollectionOptions().encryptedFieldConfig) {
                _stats.queryableEncryption += 1;
            }
            if (isCSFLE1Validator(coll->getValidatorDoc())) {
                _stats.csfle += 1;
            }
        }

        if (nss.isSystemDotProfile()) {
            _stats.systemProfile += 1;
        }
    } else {
        _stats.internal += 1;
    }

    invariant(static_cast<size_t>(_stats.internal + _stats.userCollections +
                                  _stats.userTimeseries) == _collections.size());

    auto& resourceCatalog = ResourceCatalog::get();
    resourceCatalog.add({RESOURCE_DATABASE, nss.dbName()}, nss.dbName());
    resourceCatalog.add({RESOURCE_COLLECTION, nss}, nss);
}

std::shared_ptr<Collection> CollectionCatalog::deregisterCollection(
    OperationContext* opCtx, const UUID& uuid, boost::optional<Timestamp> commitTime) {
    invariant(_catalog.find(uuid));

    auto coll = std::move(_catalog[uuid]);
    auto ns = coll->ns();
    auto dbIdPair = std::make_pair(ns.dbName(), uuid);

    LOGV2_DEBUG(20281, 1, "Deregistering collection", logAttrs(ns), "uuid"_attr = uuid);

    // Make sure collection object exists.
    invariant(_collections.find(ns));
    invariant(_orderedCollections.find(dbIdPair) != _orderedCollections.end());

    _orderedCollections = _orderedCollections.erase(dbIdPair);
    _collections = _collections.erase(ns);
    _catalog = _catalog.erase(uuid);
    _pendingCommitNamespaces = _pendingCommitNamespaces.erase(ns);
    _pendingCommitUUIDs = _pendingCommitUUIDs.erase(uuid);

    _catalogIdTracker.drop(ns, uuid, commitTime);

    if (!ns.isOnInternalDb()) {
        if (ns.isSystem()) {
            _stats.internal -= 1;
        } else if (coll->isTimeseriesCollection() && coll->isNewTimeseriesWithoutView()) {
            _stats.userTimeseries -= 1;
        } else {
            _stats.userCollections -= 1;
            if (coll->isCapped()) {
                _stats.userCapped -= 1;
            }
            if (coll->isClustered()) {
                _stats.userClustered -= 1;
            }
            if (coll->getCollectionOptions().encryptedFieldConfig) {
                _stats.queryableEncryption -= 1;
            }
            if (isCSFLE1Validator(coll->getValidatorDoc())) {
                _stats.csfle -= 1;
            }
        }

        if (ns.isSystemDotProfile()) {
            _stats.systemProfile -= 1;
        }
    } else {
        _stats.internal -= 1;
    }

    invariant(static_cast<size_t>(_stats.internal + _stats.userCollections +
                                  _stats.userTimeseries) == _collections.size());

    coll->onDeregisterFromCatalog(opCtx->getServiceContext());

    ResourceCatalog::get().remove({RESOURCE_COLLECTION, ns}, ns);

    if (!storageGlobalParams.repair && coll->ns().isSystemDotViews()) {
        _viewsForDatabase = _viewsForDatabase.erase(coll->ns().dbName());
    }

    return coll;
}

void CollectionCatalog::registerUncommittedView(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
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
        throwWriteConflictException(str::stream()
                                    << "Collection namespace '" << nss.toStringForErrorMsg()
                                    << "' is already in use.");
    }

    existingCollection = _pendingCommitNamespaces.find(nss);
    if (existingCollection && existingCollection->get()) {
        LOGV2(7683900,
              "Conflicted registering namespace, already have a collection with the same namespace",
              "nss"_attr = nss);
        throwWriteConflictException(str::stream()
                                    << "Collection namespace '" << nss.toStringForErrorMsg()
                                    << "' is already in use.");
    }

    if (type == NamespaceType::kAll) {
        if (_uncommittedViews.find(nss)) {
            LOGV2(5725002,
                  "Conflicted registering namespace, already have a view with the same namespace",
                  "nss"_attr = nss);
            throwWriteConflictException(str::stream()
                                        << "Collection namespace '" << nss.toStringForErrorMsg()
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

void CollectionCatalog::deregisterAllCollectionsAndViews(ServiceContext* svcCtx) {
    LOGV2(20282, "Deregistering all the collections");
    for (auto& entry : _catalog) {
        auto& uuid = entry.first;
        auto ns = entry.second->ns();

        LOGV2_DEBUG(20283, 1, "Deregistering collection", logAttrs(ns), "uuid"_attr = uuid);
        entry.second->onDeregisterFromCatalog(svcCtx);
    }

    _collections = {};
    _orderedCollections = {};
    _catalog = {};
    _viewsForDatabase = {};
    _stats = {};

    ResourceCatalog::get().clear();
}

void CollectionCatalog::clearViews(OperationContext* opCtx, const DatabaseName& dbName) const {
    invariant(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
        NamespaceString::makeSystemDotViewsNamespace(dbName), MODE_X));

    const ViewsForDatabase* viewsForDbPtr = _viewsForDatabase.find(dbName);
    invariant(viewsForDbPtr);

    ViewsForDatabase viewsForDb = *viewsForDbPtr;
    viewsForDb.clear(opCtx);

    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog._replaceViewsForDatabase(dbName, std::move(viewsForDb));
    });
}

void CollectionCatalog::invariantHasExclusiveAccessToCollection(OperationContext* opCtx,
                                                                const NamespaceString& nss) {
    invariant(hasExclusiveAccessToCollection(opCtx, nss), nss.toStringForErrorMsg());
}

bool CollectionCatalog::hasExclusiveAccessToCollection(OperationContext* opCtx,
                                                       const NamespaceString& nss) {
    auto& uncommittedCatalogUpdates = UncommittedCatalogUpdates::get(opCtx);
    return shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_X) ||
        (uncommittedCatalogUpdates.isCreatedCollection(opCtx, nss) &&
         shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IX));
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

const HistoricalCatalogIdTracker& CollectionCatalog::catalogIdTracker() const {
    return _catalogIdTracker;
}
HistoricalCatalogIdTracker& CollectionCatalog::catalogIdTracker() {
    return _catalogIdTracker;
}

}  // namespace mongo
