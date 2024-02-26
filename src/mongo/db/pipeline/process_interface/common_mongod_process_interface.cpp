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

#include <absl/container/flat_hash_map.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <limits>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/fill_locker_info.h"
#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/initialize_auto_get_helper.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/sbe_plan_cache.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/s/transaction_coordinator_curop.h"
#include "mongo/db/s/transaction_coordinator_worker_curop_repository.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/views/view.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/query_analysis_sample_tracker.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/future.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

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
            shard_role_details::getRecoveryUnit(expCtx->opCtx)->getPrepareConflictBehavior() !=
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

        NamespaceString ns(NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
            obj.getStringField("ns")));
        if (ns.isSystemDotViews()) {
            systemViewsNamespaces->push_back(ns);
        }

        BSONObjBuilder builder;
        builder.append(
            "db", DatabaseNameUtil::serialize(ns.dbName(), SerializationContext::stateDefault()));
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
    AutoGetCollectionForReadMaybeLockFree collection(opCtx, ns);

    std::vector<Document> indexStats;
    if (!collection) {
        LOGV2_DEBUG(
            23881, 2, "Collection not found on index stats retrieval: {ns_ns}", "ns_ns"_attr = ns);
        return indexStats;
    }

    const auto& indexStatsMap =
        CollectionIndexUsageTrackerDecoration::get(collection.getCollection().get())
            .getUsageStats();
    for (auto&& indexStatsMapIter : indexStatsMap) {
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

        if (!entry->isReady()) {
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
        AutoGetCollectionForReadCommandMaybeLockFree collLock(
            opCtx,
            systemViewsNamespaces.front(),
            AutoGetCollection::Options{}.secondaryNssOrUUIDs(++systemViewsNamespaces.cbegin(),
                                                             systemViewsNamespaces.cend()),
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp);

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
        if (!std::equal(systemViewsNamespaces.cbegin(),
                        systemViewsNamespaces.cend(),
                        systemViewsNamespacesFromSecondCatalogRead.cbegin(),
                        [](const auto& lhs, const auto& rhs) { return lhs.nss() == rhs.nss(); })) {
            continue;
        }

        for (const auto& svns : systemViewsNamespaces) {
            // Hold reference to the catalog for collection lookup without locks to be safe.
            auto catalog = CollectionCatalog::get(opCtx);
            auto collection = catalog->lookupCollectionByNamespace(opCtx, svns.nss());
            if (!collection) {
                continue;
            }

            auto cursor = collection->getCursor(opCtx);
            while (auto record = cursor->next()) {
                BSONObj obj = record->data.releaseToBson();

                NamespaceString ns(
                    NamespaceStringUtil::deserialize(svns.nss().tenantId(),
                                                     obj.getStringField("_id"),
                                                     SerializationContext::stateDefault()));
                NamespaceString viewOnNs(
                    NamespaceStringUtil::deserialize(ns.dbName(), obj.getStringField("viewOn")));

                BSONObjBuilder builder;
                builder.append(
                    "db",
                    DatabaseNameUtil::serialize(ns.dbName(), SerializationContext::stateDefault()));
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
    OperationContext* opCtx,
    const NamespaceString& ns,
    const boost::optional<UUID>& collUUID) const {

    // Perform an aquisition. This will verify that the collection still exists at the given
    // read concern. If it doesn't and the aggregation has specified a UUID then this acquisition
    // will fail.
    auto acquisition =
        acquireCollectionMaybeLockFree(opCtx,
                                       CollectionAcquisitionRequest::fromOpCtx(
                                           opCtx, ns, AcquisitionPrerequisites::kRead, collUUID));

    if (!acquisition.exists()) {
        return boost::none;
    }

    auto obj = DurableCatalog::get(opCtx)->getCatalogEntry(
        opCtx, acquisition.getCollectionPtr()->getCatalogId());

    BSONObjBuilder builder;
    builder.append("db",
                   DatabaseNameUtil::serialize(ns.dbName(), SerializationContext::stateDefault()));
    builder.append("name", ns.coll());
    builder.append("type", "collection");
    if (auto shardName = getShardName(opCtx); !shardName.empty()) {
        builder.append("shard", shardName);
    }
    builder.appendElements(obj);

    return builder.obj();
}

void CommonMongodProcessInterface::appendLatencyStats(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      bool includeHistograms,
                                                      BSONObjBuilder* builder) const {
    auto catalog = CollectionCatalog::get(opCtx);
    auto view = catalog->lookupView(opCtx, nss);
    if (!view) {
        AutoGetCollectionForRead collection(opCtx, nss);
        bool redactForQE =
            (collection && collection->getCollectionOptions().encryptedFieldConfig) ||
            nss.isFLE2StateCollection();
        if (!redactForQE) {
            Top::get(opCtx->getServiceContext())
                .appendLatencyStats(nss, includeHistograms, builder);
        }
    } else {
        Top::get(opCtx->getServiceContext()).appendLatencyStats(nss, includeHistograms, builder);
    }
}

Status CommonMongodProcessInterface::appendStorageStats(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const StorageStatsSpec& spec,
    BSONObjBuilder* builder,
    const boost::optional<BSONObj>& filterObj) const {
    return appendCollectionStorageStats(
        expCtx->opCtx, nss, spec, expCtx->serializationCtxt, builder, filterObj);
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
                str::stream() << "Collection [" << nss.toStringForErrorMsg() << "] not found."};
    }

    bool redactForQE =
        collection->getCollectionOptions().encryptedFieldConfig || nss.isFLE2StateCollection();
    if (!redactForQE) {
        auto collectionScanStats =
            CollectionIndexUsageTrackerDecoration::get(collection.getCollection().get())
                .getCollectionScanStats();

        dassert(collectionScanStats.collectionScans <=
                static_cast<unsigned long long>(std::numeric_limits<long long>::max()));
        dassert(collectionScanStats.collectionScansNonTailable <=
                static_cast<unsigned long long>(std::numeric_limits<long long>::max()));
        builder->append(
            "queryExecStats",
            BSON("collectionScans"
                 << BSON("total" << static_cast<long long>(collectionScanStats.collectionScans)
                                 << "nonTailable"
                                 << static_cast<long long>(
                                        collectionScanStats.collectionScansNonTailable))));
    }
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
CommonMongodProcessInterface::attachCursorSourceToPipelineForLocalRead(
    Pipeline* ownedPipeline, boost::optional<const AggregateCommandRequest&> aggRequest) {
    auto expCtx = ownedPipeline->getContext();
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(ownedPipeline,
                                                        PipelineDeleter(expCtx->opCtx));

    Pipeline::SourceContainer& sources = pipeline->getSources();
    boost::optional<DocumentSource*> firstStage =
        sources.empty() ? boost::optional<DocumentSource*>{} : sources.front().get();
    invariant(!firstStage || !dynamic_cast<DocumentSourceCursor*>(*firstStage));

    bool skipRequiresInputDocSourceCheck =
        PipelineD::isSearchPresentAndEligibleForSbe(pipeline.get());

    if (!skipRequiresInputDocSourceCheck && firstStage &&
        !(*firstStage)->constraints().requiresInputDocSource) {
        // There's no need to attach a cursor here.
        search_helpers::prepareSearchForNestedPipeline(pipeline.get());
        return pipeline;
    }

    if (expCtx->eligibleForSampling()) {
        if (auto sampleId = analyze_shard_key::tryGenerateSampleId(
                expCtx->opCtx, expCtx->ns, analyze_shard_key::SampledCommandNameEnum::kAggregate)) {
            auto [_, letParameters] =
                expCtx->variablesParseState.transitionalCompatibilitySerialize(expCtx->variables);
            analyze_shard_key::QueryAnalysisWriter::get(expCtx->opCtx)
                ->addAggregateQuery(*sampleId,
                                    expCtx->ns,
                                    pipeline->getInitialQuery(),
                                    expCtx->getCollatorBSON(),
                                    letParameters)
                .getAsync([](auto) {});
        }
    }

    // Reparse 'pipeline' to discover whether there are secondary namespaces that we need to lock
    // when constructing our query executor.
    auto lpp = LiteParsedPipeline(expCtx->ns, pipeline->serializeToBson());
    std::vector<NamespaceStringOrUUID> secondaryNamespaces = lpp.getForeignExecutionNamespaces();
    auto* opCtx = expCtx->opCtx;

    boost::optional<AutoGetCollectionForReadCommandMaybeLockFree> autoColl = boost::none;
    auto initAutoGetCallback = [&]() {
        autoColl.emplace(opCtx,
                         expCtx->ns,
                         AutoGetCollection::Options{}.secondaryNssOrUUIDs(
                             secondaryNamespaces.cbegin(), secondaryNamespaces.cend()),
                         AutoStatsTracker::LogMode::kUpdateTop);
    };

    bool isAnySecondaryCollectionNotLocal =
        intializeAutoGet(opCtx, expCtx->ns, secondaryNamespaces, initAutoGetCallback);

    tassert(8322002,
            "Should have initialized AutoGet* after calling 'initializeAutoGet'",
            autoColl.has_value());
    uassert(ErrorCodes::NamespaceNotFound,
            fmt::format("collection '{}' does not match the expected uuid",
                        expCtx->ns.toStringForErrorMsg()),
            !expCtx->uuid ||
                (autoColl->getCollection() && autoColl->getCollection()->uuid() == expCtx->uuid));

    MultipleCollectionAccessor holder{expCtx->opCtx,
                                      &autoColl->getCollection(),
                                      autoColl->getNss(),
                                      autoColl->isAnySecondaryNamespaceAView() ||
                                          isAnySecondaryCollectionNotLocal,
                                      secondaryNamespaces};
    auto resolvedAggRequest = aggRequest ? &aggRequest.get() : nullptr;
    PipelineD::buildAndAttachInnerQueryExecutorToPipeline(
        holder, expCtx->ns, resolvedAggRequest, pipeline.get());

    return pipeline;
}

std::string CommonMongodProcessInterface::getShardName(OperationContext* opCtx) const {
    if (auto shardId = getShardId(opCtx)) {
        return shardId->toString();
    }

    return std::string();
}

boost::optional<ShardId> CommonMongodProcessInterface::getShardId(OperationContext* opCtx) const {
    if (ShardingState::get(opCtx)->enabled()) {
        return ShardingState::get(opCtx)->shardId();
    }

    return {};
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
            _getCollectionDefaultCollator(expCtx->opCtx, nss.dbName(), collectionUUID));

        // If we are here, we are either executing the pipeline normally or running in one of the
        // execution stat explain verbosities. In either case, we disable explain on the foreign
        // context so that we actually retrieve the document.
        foreignExpCtx->explain = boost::none;
        pipeline = Pipeline::makePipeline({BSON("$match" << documentKey)}, foreignExpCtx, opts);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
        LOGV2_DEBUG(6726700, 1, "Namespace not found while looking up document", "error"_attr = ex);
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
    const auto serializer = [](const auto& key, const auto& entry) {
        BSONObjBuilder out;
        Explain::planCacheEntryToBSON(entry, &out);
        if (auto querySettings = key.querySettings().toBSON(); !querySettings.isEmpty()) {
            out.append("querySettings"_sd, querySettings);
        }
        return out.obj();
    };

    const auto predicate = [&matchExp](const BSONObj& obj) {
        if (obj.hasField("securityLevel")) {
            return false;
        }
        return !matchExp ? true : matchExp->matchesBSON(obj);
    };

    AutoGetCollection collection(opCtx, nss, MODE_IS);
    uassert(50933,
            str::stream() << "collection '" << nss.toStringForErrorMsg() << "' does not exist",
            collection);

    const auto& collQueryInfo = CollectionQueryInfo::get(collection.getCollection());
    const auto planCache = collQueryInfo.getPlanCache();
    invariant(planCache);

    auto planCacheEntries =
        planCache->getMatchingStats({} /* cacheKeyFilterFunc */, serializer, predicate);

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
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Client* client,
    CurrentOpTruncateMode truncateOps,
    CurrentOpBacktraceMode backtraceMode) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(expCtx,
                                    client,
                                    (truncateOps == CurrentOpTruncateMode::kTruncateOps),
                                    (backtraceMode == CurrentOpBacktraceMode::kIncludeBacktrace),
                                    &builder);

    OperationContext* clientOpCtx = client->getOperationContext();

    if (clientOpCtx) {
        if (CurOp::get(clientOpCtx)->getShouldOmitDiagnosticInformation()) {
            return builder.obj();
        }

        if (auto txnParticipant = TransactionParticipant::get(clientOpCtx)) {
            txnParticipant.reportUnstashedState(clientOpCtx, &builder);
        }

        // Append lock stats before returning.
        auto lockerInfo = shard_role_details::getLocker(clientOpCtx)
                              ->getLockerInfo(CurOp::get(*clientOpCtx)->getLockStatsBase());
        fillLockerInfo(lockerInfo, builder);


        if (auto tcWorkerRepo = getTransactionCoordinatorWorkerCurOpRepository()) {
            tcWorkerRepo->reportState(clientOpCtx, &builder);
        }

        auto flowControlStats = shard_role_details::getLocker(clientOpCtx)->getFlowControlStats();
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

void CommonMongodProcessInterface::_reportCurrentOpsForQueryAnalysis(
    OperationContext* opCtx, std::vector<BSONObj>* ops) const {
    if (analyze_shard_key::supportsPersistingSampledQueries(opCtx)) {
        analyze_shard_key::QueryAnalysisSampleTracker::get(opCtx).reportForCurrentOp(ops);
    }
}

std::unique_ptr<CollatorInterface> CommonMongodProcessInterface::_getCollectionDefaultCollator(
    OperationContext* opCtx, const DatabaseName& dbName, UUID collectionUUID) {
    auto it = _collatorCache.find(collectionUUID);
    if (it == _collatorCache.end()) {
        auto collator = [&]() -> std::unique_ptr<CollatorInterface> {
            AutoGetCollection autoColl(opCtx, {dbName, collectionUUID}, MODE_IS);
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

std::pair<std::set<FieldPath>, boost::optional<ChunkVersion>>
CommonMongodProcessInterface::ensureFieldsUniqueOrResolveDocumentKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<std::set<FieldPath>> fieldPaths,
    boost::optional<ChunkVersion> targetCollectionPlacementVersion,
    const NamespaceString& outputNs) const {
    uassert(51123,
            "Unexpected target chunk version specified",
            !targetCollectionPlacementVersion || expCtx->fromMongos);

    if (!fieldPaths) {
        uassert(51124, "Expected fields to be provided from mongos", !expCtx->fromMongos);
        return {std::set<FieldPath>{"_id"}, targetCollectionPlacementVersion};
    }

    // Make sure the 'fields' array has a supporting index. Skip this check if the command is sent
    // from mongos since the 'fields' check would've happened already.
    if (!expCtx->fromMongos) {
        uassert(51183,
                "Cannot find index to verify that join fields will be unique",
                fieldsHaveSupportingUniqueIndex(expCtx, outputNs, *fieldPaths));
    }
    return {*fieldPaths, targetCollectionPlacementVersion};
}

BSONObj CommonMongodProcessInterface::_convertRenameToInternalRename(
    OperationContext* opCtx,
    const NamespaceString& sourceNs,
    const NamespaceString& targetNs,
    const BSONObj& originalCollectionOptions,
    const std::list<BSONObj>& originalIndexes) {

    BSONObjBuilder newCmd;
    newCmd.append("internalRenameIfOptionsAndIndexesMatch", 1);
    newCmd.append("from",
                  NamespaceStringUtil::serialize(sourceNs, SerializationContext::stateDefault()));
    newCmd.append("to",
                  NamespaceStringUtil::serialize(targetNs, SerializationContext::stateDefault()));
    newCmd.append("collectionOptions", originalCollectionOptions);
    BSONArrayBuilder indexArrayBuilder(newCmd.subarrayStart("indexes"));
    for (auto&& index : originalIndexes) {
        indexArrayBuilder.append(index);
    }
    indexArrayBuilder.done();
    return newCmd.obj();
}

void CommonMongodProcessInterface::_handleTimeseriesCreateError(const DBException& ex,
                                                                OperationContext* opCtx,
                                                                const NamespaceString& ns,
                                                                TimeseriesOptions userOpts) {
    // If we receive a NamespaceExists error for a time-series view that has the same
    // specification as the time-series view we wanted to create, we should not throw an
    // error. The user is allowed to overwrite an existing time-series collection when
    // entering this function.

    // Confirming the error is NamespaceExists
    if (ex.code() != ErrorCodes::NamespaceExists) {
        throw;
    }
    auto timeseriesOpts = _getTimeseriesOptions(opCtx, ns);
    // Confirming there is a time-series view in that namespace and the time-series options of the
    // existing view are the same as expected.
    if (!timeseriesOpts || !mongo::timeseries::optionsAreEqual(timeseriesOpts.value(), userOpts)) {
        throw;
    }
}

boost::optional<TimeseriesOptions> CommonMongodProcessInterface::_getTimeseriesOptions(
    OperationContext* opCtx, const NamespaceString& ns) {
    auto view = CollectionCatalog::get(opCtx)->lookupView(opCtx, ns);
    if (!view || !view->timeseries()) {
        return boost::none;
    }
    return mongo::timeseries::getTimeseriesOptions(opCtx, ns, true /*convertToBucketsNamespace*/);
}

void CommonMongodProcessInterface::writeRecordsToRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    RecordStore* rs,
    std::vector<Record>* records,
    const std::vector<Timestamp>& ts) const {
    tassert(5643012, "Attempted to write to record store with nullptr", records);
    assertIgnorePrepareConflictsBehavior(expCtx);
    writeConflictRetry(expCtx->opCtx, "MPI::writeRecordsToRecordStore", expCtx->ns, [&] {
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
    writeConflictRetry(expCtx->opCtx, "MPI::deleteFromRecordStore", expCtx->ns, [&] {
        Lock::GlobalLock lk(expCtx->opCtx, MODE_IS);
        WriteUnitOfWork wuow(expCtx->opCtx);
        rs->deleteRecord(expCtx->opCtx, rID);
        wuow.commit();
    });
}

void CommonMongodProcessInterface::truncateRecordStore(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, RecordStore* rs) const {
    assertIgnorePrepareConflictsBehavior(expCtx);
    writeConflictRetry(expCtx->opCtx, "MPI::truncateRecordStore", expCtx->ns, [&] {
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
    AutoGetCollectionForReadMaybeLockFree autoColl(expCtx->opCtx, nss);
    BSONObj document;
    if (!Helpers::findById(expCtx->opCtx, nss, documentKey.toBson(), document)) {
        return boost::none;
    }
    return Document(document).getOwned();
}

}  // namespace mongo
