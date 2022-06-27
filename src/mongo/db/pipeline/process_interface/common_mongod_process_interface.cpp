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


#include "mongo/db/pipeline/process_interface/common_mongod_process_interface.h"

#include <algorithm>
#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog/list_indexes.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/sbe_plan_cache.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/transaction_coordinator_curop.h"
#include "mongo/db/s/transaction_coordinator_worker_curop_repository.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/transaction_participant_resource_yielder.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/query/document_source_merge_cursors.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace {

// Returns true if the field names of 'keyPattern' are exactly those in 'uniqueKeyPaths', and each
// of the elements of 'keyPattern' is numeric, i.e. not "text", "$**", or any other special type of
// index.
bool keyPatternNamesExactPaths(const BSONObj& keyPattern,
                               const std::set<FieldPath>& uniqueKeyPaths) {
    size_t nFieldsMatched = 0;
    for (auto&& elem : keyPattern) {
        if (!elem.isNumber()) {
            return false;
        }
        if (uniqueKeyPaths.find(elem.fieldNameStringData()) == uniqueKeyPaths.end()) {
            return false;
        }
        ++nFieldsMatched;
    }
    return nFieldsMatched == uniqueKeyPaths.size();
}

bool supportsUniqueKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const IndexCatalogEntry* index,
                       const std::set<FieldPath>& uniqueKeyPaths) {
    return (index->descriptor()->unique() && !index->descriptor()->isPartial() &&
            keyPatternNamesExactPaths(index->descriptor()->keyPattern(), uniqueKeyPaths) &&
            CollatorInterface::collatorsMatch(index->getCollator(), expCtx->getCollator()));
}

// Proactively assert that this operation can safely write before hitting an assertion in the
// storage engine. We can safely write if we are enforcing prepare conflicts by blocking or if we
// are ignoring prepare conflicts and explicitly allowing writes. Ignoring prepare conflicts
// without allowing writes will cause this operation to fail in the storage engine.
void assertIgnorePrepareConflictsBehavior(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    tassert(5996900,
            "Expected operation to either be blocking on prepare conflicts or ignoring prepare "
            "conflicts and allowing writes",
            expCtx->opCtx->recoveryUnit()->getPrepareConflictBehavior() !=
                PrepareConflictBehavior::kIgnoreConflicts);
}

/**
 * Returns all documents from _mdb_catalog along with a sorted list of all
 * <db>.system.views namespaces found.
 */
void listDurableCatalog(OperationContext* opCtx,
                        StringData shardName,
                        std::deque<BSONObj>* docs,
                        std::vector<NamespaceStringOrUUID>* systemViewsNamespaces) {
    auto durableCatalog = DurableCatalog::get(opCtx);
    auto rs = durableCatalog->getRecordStore();
    if (!rs) {
        return;
    }

    auto cursor = rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();

        // For backwards compatibility where older version have a written feature document.
        // See SERVER-57125.
        if (DurableCatalog::isFeatureDocument(obj)) {
            continue;
        }

        NamespaceString ns(obj.getStringField("ns"));
        if (ns.isSystemDotViews()) {
            systemViewsNamespaces->push_back(ns);
        }

        BSONObjBuilder builder;
        builder.append("db", ns.db());
        builder.append("name", ns.coll());
        builder.append("type", "collection");
        if (!shardName.empty()) {
            builder.append("shard", shardName);
        }
        builder.appendElements(obj);
        docs->push_back(builder.obj());
    }
}

}  // namespace

std::unique_ptr<TransactionHistoryIteratorBase>
CommonMongodProcessInterface::createTransactionHistoryIterator(repl::OpTime time) const {
    bool permitYield = true;
    return std::unique_ptr<TransactionHistoryIteratorBase>(
        new TransactionHistoryIterator(time, permitYield));
}

std::vector<Document> CommonMongodProcessInterface::getIndexStats(OperationContext* opCtx,
                                                                  const NamespaceString& ns,
                                                                  StringData host,
                                                                  bool addShardName) {
    AutoGetCollectionForReadCommandMaybeLockFree collection(opCtx, ns);

    std::vector<Document> indexStats;
    if (!collection) {
        LOGV2_DEBUG(23881,
                    2,
                    "Collection not found on index stats retrieval: {ns_ns}",
                    "ns_ns"_attr = ns.ns());
        return indexStats;
    }

    auto indexStatsMap =
        CollectionIndexUsageTrackerDecoration::get(collection->getSharedDecorations())
            .getUsageStats();
    for (auto&& indexStatsMapIter : *indexStatsMap) {
        auto indexName = indexStatsMapIter.first;
        auto stats = indexStatsMapIter.second;
        MutableDocument doc;
        doc["name"] = Value(indexName);
        doc["key"] = Value(stats->indexKey);
        doc["host"] = Value(host);
        doc["accesses"]["ops"] = Value(stats->accesses.loadRelaxed());
        doc["accesses"]["since"] = Value(stats->trackerStartTime);

        if (addShardName)
            doc["shard"] = Value(getShardName(opCtx));

        // Retrieve the relevant index entry.
        auto idxCatalog = collection->getIndexCatalog();
        auto idx = idxCatalog->findIndexByName(opCtx,
                                               indexName,
                                               IndexCatalog::InclusionPolicy::kReady |
                                                   IndexCatalog::InclusionPolicy::kUnfinished);
        uassert(ErrorCodes::IndexNotFound,
                "Could not find entry in IndexCatalog for index " + indexName,
                idx);
        auto entry = idxCatalog->getEntry(idx);
        doc["spec"] = Value(idx->infoObj());

        // Not all indexes in the CollectionIndexUsageTracker may be visible or consistent with our
        // snapshot. For this reason, it is unsafe to check `isReady` on the entry, which
        // asserts that the index's in-memory state is consistent with our snapshot.
        if (!entry->isPresentInMySnapshot(opCtx)) {
            continue;
        }

        if (!entry->isReadyInMySnapshot(opCtx)) {
            doc["building"] = Value(true);
        }

        indexStats.push_back(doc.freeze());
    }
    return indexStats;
}

std::deque<BSONObj> CommonMongodProcessInterface::listCatalog(OperationContext* opCtx) const {
    while (true) {
        std::deque<BSONObj> docs;
        std::vector<NamespaceStringOrUUID> systemViewsNamespaces;
        {
            Lock::GlobalLock globalLock(opCtx, MODE_IS);
            listDurableCatalog(opCtx, getShardName(opCtx), &docs, &systemViewsNamespaces);
        }

        if (systemViewsNamespaces.empty()) {
            return docs;
        }

        // Clear 'docs' because we will read _mdb_catalog again using a consistent snapshot for all
        // the system.views collections.
        docs.clear();

        // We want to read all the system.views as well as _mdb_catalog (again) using a consistent
        // snapshot.
        // TODO(SERVER-63754): Replace with a less verbose constructor overload when available.
        AutoGetCollectionForReadCommandMaybeLockFree collLock(
            opCtx,
            systemViewsNamespaces.front(),
            AutoGetCollectionViewMode::kViewsForbidden,
            Date_t::max(),
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
            {++systemViewsNamespaces.cbegin(), systemViewsNamespaces.cend()});

        // If the primary collection is not available, it means the information from parsing
        // _mdb_catalog is no longer valid. Therefore, we restart this process from the top.
        if (!collLock.getCollection()) {
            continue;
        }

        // Read _mdb_catalog again using the same snapshot set up by our collection(s) lock helper.
        // If _mdb_catalog contains a different set of system.views namespaces from the first time
        // we read it, we should discard this set of results and retry from the top (with the
        // global read lock) of this loop.
        std::vector<NamespaceStringOrUUID> systemViewsNamespacesFromSecondCatalogRead;
        listDurableCatalog(
            opCtx, getShardName(opCtx), &docs, &systemViewsNamespacesFromSecondCatalogRead);
        if (!std::equal(
                systemViewsNamespaces.cbegin(),
                systemViewsNamespaces.cend(),
                systemViewsNamespacesFromSecondCatalogRead.cbegin(),
                [](const auto& lhs, const auto& rhs) { return *lhs.nss() == *rhs.nss(); })) {
            continue;
        }

        for (const auto& svns : systemViewsNamespaces) {
            auto collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForRead(
                opCtx, *svns.nss());
            if (!collection) {
                continue;
            }

            auto cursor = collection->getCursor(opCtx);
            while (auto record = cursor->next()) {
                BSONObj obj = record->data.releaseToBson();

                NamespaceString ns(obj.getStringField("_id"));
                NamespaceString viewOnNs(ns.db(), obj.getStringField("viewOn"));

                BSONObjBuilder builder;
                builder.append("db", ns.db());
                builder.append("name", ns.coll());
                if (viewOnNs.isTimeseriesBucketsCollection()) {
                    builder.append("type", "timeseries");
                } else {
                    builder.append("type", "view");
                }
                if (auto shardName = getShardName(opCtx); !shardName.empty()) {
                    builder.append("shard", shardName);
                }
                builder.appendAs(obj["_id"], "ns");
                builder.appendElements(obj);
                docs.push_back(builder.obj());
            }
        }

        return docs;
    }
}

boost::optional<BSONObj> CommonMongodProcessInterface::getCatalogEntry(
    OperationContext* opCtx, const NamespaceString& ns) const {
    Lock::GlobalLock globalLock{opCtx, MODE_IS};

    auto rs = DurableCatalog::get(opCtx)->getRecordStore();
    if (!rs) {
        return boost::none;
    }

    auto cursor = rs->getCursor(opCtx);
    while (auto record = cursor->next()) {
        auto obj = record->data.toBson();
        if (NamespaceString{obj.getStringField("ns")} != ns) {
            continue;
        }

        BSONObjBuilder builder;
        builder.append("db", ns.db());
        builder.append("name", ns.coll());
        builder.append("type", "collection");
        if (auto shardName = getShardName(opCtx); !shardName.empty()) {
            builder.append("shard", shardName);
        }
        builder.appendElements(obj);

        return builder.obj();
    }

    return boost::none;
}

void CommonMongodProcessInterface::appendLatencyStats(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      bool includeHistograms,
                                                      BSONObjBuilder* builder) const {
    Top::get(opCtx->getServiceContext()).appendLatencyStats(nss, includeHistograms, builder);
}

Status CommonMongodProcessInterface::appendStorageStats(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        const StorageStatsSpec& spec,
                                                        BSONObjBuilder* builder) const {
    return appendCollectionStorageStats(opCtx, nss, spec, builder);
}

Status CommonMongodProcessInterface::appendRecordCount(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       BSONObjBuilder* builder) const {
    return appendCollectionRecordCount(opCtx, nss, builder);
}

Status CommonMongodProcessInterface::appendQueryExecStats(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          BSONObjBuilder* builder) const {
    AutoGetCollectionForReadCommand collection(opCtx, nss);
    if (!collection) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection [" << nss.toString() << "] not found."};
    }

    auto collectionScanStats =
        CollectionIndexUsageTrackerDecoration::get(collection->getSharedDecorations())
            .getCollectionScanStats();

    dassert(collectionScanStats.collectionScans <=
            static_cast<unsigned long long>(std::numeric_limits<long long>::max()));
    dassert(collectionScanStats.collectionScansNonTailable <=
            static_cast<unsigned long long>(std::numeric_limits<long long>::max()));
    builder->append("queryExecStats",
                    BSON("collectionScans" << BSON(
                             "total" << static_cast<long long>(collectionScanStats.collectionScans)
                                     << "nonTailable"
                                     << static_cast<long long>(
                                            collectionScanStats.collectionScansNonTailable))));

    return Status::OK();
}

BSONObj CommonMongodProcessInterface::getCollectionOptionsLocally(OperationContext* opCtx,
                                                                  const NamespaceString& nss) {
    AutoGetCollectionForReadCommand collection(opCtx, nss);
    BSONObj collectionOptions = {};
    if (!collection) {
        return collectionOptions;
    }

    collectionOptions = collection->getCollectionOptions().toBSON();
    return collectionOptions;
}

BSONObj CommonMongodProcessInterface::getCollectionOptions(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    return getCollectionOptionsLocally(opCtx, nss);
}

std::unique_ptr<Pipeline, PipelineDeleter>
CommonMongodProcessInterface::attachCursorSourceToPipelineForLocalRead(Pipeline* ownedPipeline) {
    auto expCtx = ownedPipeline->getContext();
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(ownedPipeline,
                                                        PipelineDeleter(expCtx->opCtx));

    boost::optional<DocumentSource*> firstStage = pipeline->getSources().empty()
        ? boost::optional<DocumentSource*>{}
        : pipeline->getSources().front().get();
    invariant(!firstStage || !dynamic_cast<DocumentSourceCursor*>(*firstStage));
    if (firstStage && !(*firstStage)->constraints().requiresInputDocSource) {
        // There's no need to attach a cursor here.
        return pipeline;
    }

    boost::optional<AutoGetCollectionForReadCommandMaybeLockFree> autoColl;
    const NamespaceStringOrUUID nsOrUUID = expCtx->uuid
        ? NamespaceStringOrUUID{expCtx->ns.db().toString(), *expCtx->uuid}
        : expCtx->ns;

    // Reparse 'pipeline' to discover whether there are secondary namespaces that we need to lock
    // when constructing our query executor.
    auto lpp = LiteParsedPipeline(expCtx->ns, pipeline->serializeToBson());
    std::vector<NamespaceStringOrUUID> secondaryNamespaces = lpp.getForeignExecutionNamespaces();

    autoColl.emplace(expCtx->opCtx,
                     nsOrUUID,
                     AutoGetCollectionViewMode::kViewsForbidden,
                     Date_t::max(),
                     AutoStatsTracker::LogMode::kUpdateTop,
                     secondaryNamespaces);

    MultipleCollectionAccessor holder{expCtx->opCtx,
                                      &autoColl->getCollection(),
                                      autoColl->getNss(),
                                      autoColl->isAnySecondaryNamespaceAViewOrSharded(),
                                      secondaryNamespaces};
    PipelineD::buildAndAttachInnerQueryExecutorToPipeline(
        holder, expCtx->ns, nullptr, pipeline.get());

    return pipeline;
}

std::string CommonMongodProcessInterface::getShardName(OperationContext* opCtx) const {
    if (ShardingState::get(opCtx)->enabled()) {
        return ShardingState::get(opCtx)->shardId().toString();
    }

    return std::string();
}

bool CommonMongodProcessInterface::inShardedEnvironment(OperationContext* opCtx) const {
    return ShardingState::get(opCtx)->enabled();
}

std::vector<GenericCursor> CommonMongodProcessInterface::getIdleCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, CurrentOpUserMode userMode) const {
    return CursorManager::get(expCtx->opCtx)->getIdleCursors(expCtx->opCtx, userMode);
}

boost::optional<Document> CommonMongodProcessInterface::doLookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    UUID collectionUUID,
    const Document& documentKey,
    MakePipelineOptions opts) {
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    try {
        // Be sure to do the lookup using the collection default collation
        auto foreignExpCtx = expCtx->copyWith(
            nss,
            collectionUUID,
            _getCollectionDefaultCollator(expCtx->opCtx, nss.db(), collectionUUID));

        // If we are here, we are either executing the pipeline normally or running in one of the
        // execution stat explain verbosities. In either case, we disable explain on the foreign
        // context so that we actually retrieve the document.
        foreignExpCtx->explain = boost::none;

        pipeline = Pipeline::makePipeline({BSON("$match" << documentKey)}, foreignExpCtx, opts);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        return boost::none;
    }

    auto lookedUpDocument = pipeline->getNext();
    if (auto next = pipeline->getNext()) {
        uasserted(ErrorCodes::ChangeStreamFatalError,
                  str::stream() << "found more than one document with document key "
                                << documentKey.toString() << " [" << lookedUpDocument->toString()
                                << ", " << next->toString() << "]");
    }

    return lookedUpDocument;
}

BackupCursorState CommonMongodProcessInterface::openBackupCursor(
    OperationContext* opCtx, const StorageEngine::BackupOptions& options) {
    auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
    if (backupCursorHooks->enabled()) {
        return backupCursorHooks->openBackupCursor(opCtx, options);
    } else {
        uasserted(50956, "Backup cursors are an enterprise only feature.");
    }
}

void CommonMongodProcessInterface::closeBackupCursor(OperationContext* opCtx,
                                                     const UUID& backupId) {
    auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
    if (backupCursorHooks->enabled()) {
        backupCursorHooks->closeBackupCursor(opCtx, backupId);
    } else {
        uasserted(50955, "Backup cursors are an enterprise only feature.");
    }
}

BackupCursorExtendState CommonMongodProcessInterface::extendBackupCursor(
    OperationContext* opCtx, const UUID& backupId, const Timestamp& extendTo) {
    auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
    if (backupCursorHooks->enabled()) {
        return backupCursorHooks->extendBackupCursor(opCtx, backupId, extendTo);
    } else {
        uasserted(51010, "Backup cursors are an enterprise only feature.");
    }
}

std::vector<BSONObj> CommonMongodProcessInterface::getMatchingPlanCacheEntryStats(
    OperationContext* opCtx, const NamespaceString& nss, const MatchExpression* matchExp) const {
    const auto serializer = [](const auto& entry) {
        BSONObjBuilder out;
        Explain::planCacheEntryToBSON(entry, &out);
        return out.obj();
    };

    const auto predicate = [&matchExp](const BSONObj& obj) {
        return !matchExp ? true : matchExp->matchesBSON(obj);
    };

    AutoGetCollection collection(opCtx, nss, MODE_IS);
    uassert(
        50933, str::stream() << "collection '" << nss.toString() << "' does not exist", collection);

    const auto& collQueryInfo = CollectionQueryInfo::get(collection.getCollection());
    const auto planCache = collQueryInfo.getPlanCache();
    invariant(planCache);

    auto planCacheEntries =
        planCache->getMatchingStats({} /* cacheKeyFilterFunc */, serializer, predicate);

    if (feature_flags::gFeatureFlagSbePlanCache.isEnabledAndIgnoreFCV()) {
        // Retrieve plan cache entries from the SBE plan cache.
        const auto cacheKeyFilter = [uuid = collection->uuid(),
                                     collVersion = collQueryInfo.getPlanCacheInvalidatorVersion()](
                                        const sbe::PlanCacheKey& key) {
            // Only fetch plan cache entries with keys matching given UUID and collectionVersion.
            return uuid == key.getMainCollectionState().uuid &&
                collVersion == key.getMainCollectionState().version;
        };

        auto planCacheEntriesSBE =
            sbe::getPlanCache(opCtx).getMatchingStats(cacheKeyFilter, serializer, predicate);

        planCacheEntries.insert(
            planCacheEntries.end(), planCacheEntriesSBE.begin(), planCacheEntriesSBE.end());
    }

    return planCacheEntries;
}

bool CommonMongodProcessInterface::fieldsHaveSupportingUniqueIndex(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const std::set<FieldPath>& fieldPaths) const {
    auto* opCtx = expCtx->opCtx;

    // We purposefully avoid a helper like AutoGetCollection here because we don't want to check the
    // db version or do anything else. We simply want to protect against concurrent modifications to
    // the catalog.
    Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IS);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->getDb(opCtx, nss.dbName());
    auto collection =
        db ? CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss) : nullptr;
    if (!collection) {
        return fieldPaths == std::set<FieldPath>{"_id"};
    }

    auto indexIterator = collection->getIndexCatalog()->getIndexIterator(
        opCtx, IndexCatalog::InclusionPolicy::kReady);
    while (indexIterator->more()) {
        const IndexCatalogEntry* entry = indexIterator->next();
        if (supportsUniqueKey(expCtx, entry, fieldPaths)) {
            return true;
        }
    }
    return false;
}

BSONObj CommonMongodProcessInterface::_reportCurrentOpForClient(
    OperationContext* opCtx,
    Client* client,
    CurrentOpTruncateMode truncateOps,
    CurrentOpBacktraceMode backtraceMode) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(opCtx,
                                    client,
                                    (truncateOps == CurrentOpTruncateMode::kTruncateOps),
                                    (backtraceMode == CurrentOpBacktraceMode::kIncludeBacktrace),
                                    &builder);

    OperationContext* clientOpCtx = client->getOperationContext();

    if (clientOpCtx) {
        if (auto txnParticipant = TransactionParticipant::get(clientOpCtx)) {
            txnParticipant.reportUnstashedState(clientOpCtx, &builder);
        }

        // Append lock stats before returning.
        if (auto lockerInfo = clientOpCtx->lockState()->getLockerInfo(
                CurOp::get(*clientOpCtx)->getLockStatsBase())) {
            fillLockerInfo(*lockerInfo, builder);
        }

        if (auto tcWorkerRepo = getTransactionCoordinatorWorkerCurOpRepository()) {
            tcWorkerRepo->reportState(clientOpCtx, &builder);
        }

        auto flowControlStats = clientOpCtx->lockState()->getFlowControlStats();
        flowControlStats.writeToBuilder(builder);
    }

    return builder.obj();
}

void CommonMongodProcessInterface::_reportCurrentOpsForTransactionCoordinators(
    OperationContext* opCtx, bool includeIdle, std::vector<BSONObj>* ops) const {
    reportCurrentOpsForTransactionCoordinators(opCtx, includeIdle, ops);
}

void CommonMongodProcessInterface::_reportCurrentOpsForPrimaryOnlyServices(
    OperationContext* opCtx,
    CurrentOpConnectionsMode connMode,
    CurrentOpSessionsMode sessionMode,
    std::vector<BSONObj>* ops) const {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    invariant(registry);

    registry->reportServiceInfoForCurrentOp(connMode, sessionMode, ops);
}

void CommonMongodProcessInterface::_reportCurrentOpsForIdleSessions(
    OperationContext* opCtx, CurrentOpUserMode userMode, std::vector<BSONObj>* ops) const {
    auto sessionCatalog = SessionCatalog::get(opCtx);

    const bool authEnabled =
        AuthorizationSession::get(opCtx->getClient())->getAuthorizationManager().isAuthEnabled();

    // If the user is listing only their own ops, we use makeSessionFilterForAuthenticatedUsers to
    // create a pattern that will match against all authenticated usernames for the current client.
    // If the user is listing ops for all users, we create an empty pattern; constructing an
    // instance of SessionKiller::Matcher with this empty pattern will return all sessions.
    auto sessionFilter = (authEnabled && userMode == CurrentOpUserMode::kExcludeOthers
                              ? makeSessionFilterForAuthenticatedUsers(opCtx)
                              : KillAllSessionsByPatternSet{{}});

    sessionCatalog->scanSessions({std::move(sessionFilter)}, [&](const ObservableSession& session) {
        auto op = TransactionParticipant::get(session).reportStashedState(opCtx);
        if (!op.isEmpty()) {
            ops->emplace_back(op);
        }
    });
}

std::unique_ptr<CollatorInterface> CommonMongodProcessInterface::_getCollectionDefaultCollator(
    OperationContext* opCtx, StringData dbName, UUID collectionUUID) {
    auto it = _collatorCache.find(collectionUUID);
    if (it == _collatorCache.end()) {
        auto collator = [&]() -> std::unique_ptr<CollatorInterface> {
            AutoGetCollection autoColl(opCtx, {dbName.toString(), collectionUUID}, MODE_IS);
            if (!autoColl.getCollection()) {
                // This collection doesn't exist, so assume a nullptr default collation
                return nullptr;
            } else {
                auto defaultCollator = autoColl.getCollection()->getDefaultCollator();
                // Clone the collator so that we can safely use the pointer if the collection
                // disappears right after we release the lock.
                return defaultCollator ? defaultCollator->clone() : nullptr;
            }
        }();

        it = _collatorCache.emplace(collectionUUID, std::move(collator)).first;
    }

    auto& collator = it->second;
    return collator ? collator->clone() : nullptr;
}

std::unique_ptr<ResourceYielder> CommonMongodProcessInterface::getResourceYielder(
    StringData cmdName) const {
    return TransactionParticipantResourceYielder::make(cmdName);
}


std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>>
CommonMongodProcessInterface::ensureFieldsUniqueOrResolveDocumentKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<std::set<FieldPath>> fieldPaths,
    boost::optional<ChunkVersion> targetCollectionVersion,
    const NamespaceString& outputNs) const {
    uassert(51123,
            "Unexpected target chunk version specified",
            !targetCollectionVersion || expCtx->fromMongos);

    if (!fieldPaths) {
        uassert(51124, "Expected fields to be provided from mongos", !expCtx->fromMongos);
        return {std::set<FieldPath>{"_id"}, targetCollectionVersion};
    }

    // Make sure the 'fields' array has a supporting index. Skip this check if the command is sent
    // from mongos since the 'fields' check would've happened already.
    if (!expCtx->fromMongos) {
        uassert(51183,
                "Cannot find index to verify that join fields will be unique",
                fieldsHaveSupportingUniqueIndex(expCtx, outputNs, *fieldPaths));
    }
    return {*fieldPaths, targetCollectionVersion};
}

write_ops::InsertCommandRequest CommonMongodProcessInterface::buildInsertOp(
    const NamespaceString& nss, std::vector<BSONObj>&& objs, bool bypassDocValidation) {
    write_ops::InsertCommandRequest insertOp(nss);
    insertOp.setDocuments(std::move(objs));
    insertOp.setWriteCommandRequestBase([&] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(bypassDocValidation);
        return wcb;
    }());
    return insertOp;
}

write_ops::UpdateCommandRequest CommonMongodProcessInterface::buildUpdateOp(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    BatchedObjects&& batch,
    UpsertType upsert,
    bool multi) {
    write_ops::UpdateCommandRequest updateOp(nss);
    updateOp.setUpdates([&] {
        std::vector<write_ops::UpdateOpEntry> updateEntries;
        for (auto&& obj : batch) {
            updateEntries.push_back([&] {
                write_ops::UpdateOpEntry entry;
                auto&& [q, u, c] = obj;
                entry.setQ(std::move(q));
                entry.setU(std::move(u));
                entry.setC(std::move(c));
                entry.setUpsert(upsert != UpsertType::kNone);
                entry.setUpsertSupplied(
                    {{entry.getUpsert(), upsert == UpsertType::kInsertSuppliedDoc}});
                entry.setMulti(multi);
                return entry;
            }());
        }
        return updateEntries;
    }());
    updateOp.setWriteCommandRequestBase([&] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(expCtx->bypassDocumentValidation);
        return wcb;
    }());
    auto [constants, letParams] =
        expCtx->variablesParseState.transitionalCompatibilitySerialize(expCtx->variables);
    updateOp.setLegacyRuntimeConstants(std::move(constants));
    if (!letParams.isEmpty()) {
        updateOp.setLet(std::move(letParams));
    }
    return updateOp;
}

BSONObj CommonMongodProcessInterface::_convertRenameToInternalRename(
    OperationContext* opCtx,
    const BSONObj& renameCommandObj,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {

    BSONObjBuilder newCmd;
    newCmd.append("internalRenameIfOptionsAndIndexesMatch", 1);
    newCmd.append("from", renameCommandObj["renameCollection"].String());
    newCmd.append("to", renameCommandObj["to"].String());
    newCmd.append("collectionOptions", originalCollectionOptions);
    BSONArrayBuilder indexArrayBuilder(newCmd.subarrayStart("indexes"));
    for (auto&& index : originalIndexes) {
        indexArrayBuilder.append(index);
    }
    indexArrayBuilder.done();
    return newCmd.obj();
}

void CommonMongodProcessInterface::writeRecordsToRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    RecordStore* rs,
    std::vector<Record>* records,
    const std::vector<Timestamp>& ts) const {
    tassert(5643012, "Attempted to write to record store with nullptr", records);
    assertIgnorePrepareConflictsBehavior(expCtx);
    writeConflictRetry(expCtx->opCtx, "MPI::writeRecordsToRecordStore", expCtx->ns.ns(), [&] {
        Lock::GlobalLock lk(expCtx->opCtx, MODE_IS);
        WriteUnitOfWork wuow(expCtx->opCtx);
        auto writeResult = rs->insertRecords(expCtx->opCtx, records, ts);
        tassert(5643002,
                str::stream() << "Failed to write to disk because " << writeResult.reason(),
                writeResult.isOK());
        wuow.commit();
    });
}

std::unique_ptr<TemporaryRecordStore> CommonMongodProcessInterface::createTemporaryRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, KeyFormat keyFormat) const {
    assertIgnorePrepareConflictsBehavior(expCtx);
    return expCtx->opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(
        expCtx->opCtx, keyFormat);
}

Document CommonMongodProcessInterface::readRecordFromRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, RecordStore* rs, RecordId rID) const {
    RecordData possibleRecord;
    Lock::GlobalLock lk(expCtx->opCtx, MODE_IS);
    auto foundDoc = rs->findRecord(expCtx->opCtx, RecordId(rID), &possibleRecord);
    tassert(775101, str::stream() << "Could not find document id " << rID, foundDoc);
    return Document(possibleRecord.toBson());
}

void CommonMongodProcessInterface::deleteRecordFromRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, RecordStore* rs, RecordId rID) const {
    assertIgnorePrepareConflictsBehavior(expCtx);
    writeConflictRetry(expCtx->opCtx, "MPI::deleteFromRecordStore", expCtx->ns.ns(), [&] {
        Lock::GlobalLock lk(expCtx->opCtx, MODE_IS);
        WriteUnitOfWork wuow(expCtx->opCtx);
        rs->deleteRecord(expCtx->opCtx, rID);
        wuow.commit();
    });
}

void CommonMongodProcessInterface::truncateRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, RecordStore* rs) const {
    assertIgnorePrepareConflictsBehavior(expCtx);
    writeConflictRetry(expCtx->opCtx, "MPI::truncateRecordStore", expCtx->ns.ns(), [&] {
        Lock::GlobalLock lk(expCtx->opCtx, MODE_IS);
        WriteUnitOfWork wuow(expCtx->opCtx);
        auto status = rs->truncate(expCtx->opCtx);
        tassert(5643000, "Unable to clear record store", status.isOK());
        wuow.commit();
    });
}

boost::optional<Document> CommonMongodProcessInterface::lookupSingleDocumentLocally(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const Document& documentKey) {
    AutoGetCollectionForRead autoColl(expCtx->opCtx, nss);
    BSONObj document;
    if (!Helpers::findById(expCtx->opCtx, nss.ns(), documentKey.toBson(), document)) {
        return boost::none;
    }
    return Document(document).getOwned();
}

}  // namespace mongo
