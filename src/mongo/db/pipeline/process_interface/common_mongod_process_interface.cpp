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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/flow_control_ticketholder.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/fill_locker_info.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/initialize_auto_get_helper.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache/plan_cache.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/s/transaction_coordinator_curop.h"
#include "mongo/db/s/transaction_coordinator_worker_curop_repository.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/feature_document_util.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/db/views/view.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/query_analysis_sample_tracker.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/future.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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

MongoProcessInterface::SupportingUniqueIndex supportsUniqueKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const IndexCatalogEntry* index,
    const std::set<FieldPath>& uniqueKeyPaths) {
    bool supports =
        (index->descriptor()->unique() && !index->descriptor()->isPartial() &&
         keyPatternNamesExactPaths(index->descriptor()->keyPattern(), uniqueKeyPaths) &&
         CollatorInterface::collatorsMatch(index->getCollator(), expCtx->getCollator()));
    if (!supports) {
        return MongoProcessInterface::SupportingUniqueIndex::None;
    }
    return index->descriptor()->isSparse()
        ? MongoProcessInterface::SupportingUniqueIndex::NotNullish
        : MongoProcessInterface::SupportingUniqueIndex::Full;
}

// Proactively assert that this operation can safely write before hitting an assertion in the
// storage engine. We can safely write if we are enforcing prepare conflicts by blocking or if we
// are ignoring prepare conflicts and explicitly allowing writes. Ignoring prepare conflicts
// without allowing writes will cause this operation to fail in the storage engine.
void assertIgnorePrepareConflictsBehavior(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    tassert(5996900,
            "Expected operation to either be blocking on prepare conflicts or ignoring prepare "
            "conflicts and allowing writes",
            shard_role_details::getRecoveryUnit(expCtx->getOperationContext())
                    ->getPrepareConflictBehavior() != PrepareConflictBehavior::kIgnoreConflicts);
}

/**
 * Create a BSONObjBuilder with all fields common to collections, views and timeseries.
 */
BSONObjBuilder createCommonNsFields(const VersionContext& vCtx,
                                    StringData shardName,
                                    const NamespaceString& ns,
                                    const BSONObj& extraElements,
                                    StringData type) {
    BSONObjBuilder builder;
    builder.append("db",
                   DatabaseNameUtil::serialize(ns.dbName(), SerializationContext::stateDefault()));
    builder.append("name", ns.coll());
    builder.append("type", type);
    if (!shardName.empty()) {
        builder.append("shard", shardName);
    }
    if (const auto configDebugDump = catalog::getConfigDebugDump(vCtx, ns);
        configDebugDump.has_value()) {
        builder.append("configDebugDump", *configDebugDump);
    }
    builder.appendElements(extraElements);
    return builder;
}

BSONObj createListCatalogEntryForCollection(const VersionContext& vCtx,
                                            StringData shardName,
                                            const NamespaceString& ns,
                                            const BSONObj& catalogEntry) {
    auto type = [&]() {
        // TODO SERVER-101594 remove `isTimeseriesBucketsCollection()` after 9.0 becomes lastLTS
        // By then we will not have system buckets timeseries anymore,
        // thus we can always return "timeseries" if the collection has timeseries options
        if (catalogEntry["md"]["options"]["timeseries"].ok() &&
            !ns.isTimeseriesBucketsCollection()) {
            return "timeseries"_sd;
        }

        return "collection"_sd;
    }();

    return createCommonNsFields(vCtx, shardName, ns, catalogEntry, type).obj();
}

/**
 * Returns all documents from _mdb_catalog along with a sorted list of all
 * <db>.system.views namespaces found.
 */
void listDurableCatalog(OperationContext* opCtx,
                        StringData shardName,
                        std::deque<BSONObj>* docs,
                        std::vector<NamespaceStringOrUUID>* systemViewsNamespaces) {
    auto cursor = MDBCatalog::get(opCtx)->getCursor(opCtx);
    if (!cursor) {
        return;
    }

    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();

        // For backwards compatibility where older version have a written feature document.
        // See SERVER-57125.
        if (feature_document_util::isFeatureDocument(obj)) {
            continue;
        }

        NamespaceString ns(NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(
            obj.getStringField("ns")));
        if (ns.isSystemDotViews()) {
            systemViewsNamespaces->push_back(ns);
        }

        docs->push_back(createListCatalogEntryForCollection(
            VersionContext::getDecoration(opCtx), shardName, ns, obj));
    }
}

bool isQEColl(const CollectionAcquisition& acquisition) {
    return (acquisition.exists() &&
            acquisition.getCollectionPtr()->getCollectionOptions().encryptedFieldConfig) ||
        acquisition.nss().isFLE2StateCollection();
}

bool isQEColl(const CollectionOrViewAcquisition& acquisition) {
    return acquisition.isCollection() && isQEColl(acquisition.getCollection());
}

[[nodiscard]] Lock::GlobalLock acquireLockForSpillTable(OperationContext* opCtx) {
    return Lock::GlobalLock(
        opCtx,
        LockMode::MODE_IS,
        Date_t::max(),
        Lock::InterruptBehavior::kThrow,
        Lock::GlobalLockOptions{.skipFlowControlTicket = true, .skipRSTLLock = true});
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
    // Using kPretendUnsharded as
    // 1. the function is called in a stage where the shard version has already been checked.
    // 2. The function only access index stats and not actual data.
    // An ideal design would allow us to access an already acquired acquisition.
    const auto acquisition = acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest(ns,
                                     PlacementConcern::kPretendUnsharded,
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead));
    std::vector<Document> indexStats;
    if (!acquisition.exists()) {
        LOGV2_DEBUG(
            23881, 2, "Collection not found on index stats retrieval: {ns_ns}", "ns_ns"_attr = ns);
        return indexStats;
    }

    const auto& collPtr = acquisition.getCollectionPtr();
    const auto& indexStatsMap = CollectionIndexUsageTrackerDecoration::getUsageStats(collPtr.get());
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
        auto idxCatalog = collPtr->getIndexCatalog();
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
        auto primaryNss = systemViewsNamespaces.front().nss();
        AutoStatsTracker tracker{opCtx,
                                 primaryNss,
                                 Top::LockType::ReadLocked,
                                 AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                 DatabaseProfileSettings::get(opCtx->getServiceContext())
                                     .getDatabaseProfileLevel(primaryNss.dbName()),
                                 Date_t::max(),
                                 systemViewsNamespaces.cbegin(),
                                 systemViewsNamespaces.cend()};

        CollectionAcquisitionRequests requests;
        requests.reserve(systemViewsNamespaces.size());
        std::transform(systemViewsNamespaces.begin(),
                       systemViewsNamespaces.end(),
                       std::back_inserter(requests),
                       [opCtx](const NamespaceStringOrUUID& nsOrUuid) {
                           return CollectionAcquisitionRequest(
                               nsOrUuid,
                               PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                               repl::ReadConcernArgs::get(opCtx),
                               AcquisitionPrerequisites::kRead);
                       });
        auto acquisitions = acquireCollectionsMaybeLockFree(opCtx, requests);


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

        for (const auto& acq : acquisitions) {
            if (!acq.exists())
                continue;
            const auto& collPtr = acq.getCollectionPtr();
            auto cursor = collPtr->getCursor(opCtx);
            while (auto record = cursor->next()) {
                BSONObj obj = record->data.releaseToBson();

                NamespaceString ns(
                    NamespaceStringUtil::deserialize(acq.nss().tenantId(),
                                                     obj.getStringField("_id"),
                                                     SerializationContext::stateDefault()));
                NamespaceString viewOnNs(
                    NamespaceStringUtil::deserialize(ns.dbName(), obj.getStringField("viewOn")));

                auto builder = createCommonNsFields(
                    VersionContext::getDecoration(opCtx),
                    getShardName(opCtx),
                    ns,
                    obj,
                    // TODO SERVER-101594 stop passing type once 9.0 becomes lastLTS.
                    // By then we will not have anymore views for timeseries collection.
                    viewOnNs.isTimeseriesBucketsCollection() ? "timeseries" : "view");
                builder.appendAs(obj["_id"], "ns");
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
    const auto& collPtr = acquisition.getCollectionPtr();
    auto obj = MDBCatalog::get(opCtx)->getRawCatalogEntry(opCtx, collPtr->getCatalogId());

    return createListCatalogEntryForCollection(
        VersionContext::getDecoration(opCtx), getShardName(opCtx), ns, obj);
}

void CommonMongodProcessInterface::appendLatencyStats(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      bool includeHistograms,
                                                      BSONObjBuilder* builder) const {
    // Using kPretendUnsharded as the helper accesses only indexStats and not user data.
    auto acquisition = acquireCollectionOrViewMaybeLockFree(
        opCtx,
        CollectionOrViewAcquisitionRequest(nss,
                                           PlacementConcern::kPretendUnsharded,
                                           repl::ReadConcernArgs::get(opCtx),
                                           AcquisitionPrerequisites::kRead));
    if (!isQEColl(acquisition)) {
        Top::getDecoration(opCtx).appendLatencyStats(nss, includeHistograms, builder);
    }
}

Status CommonMongodProcessInterface::appendStorageStats(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const StorageStatsSpec& spec,
    BSONObjBuilder* builder,
    const boost::optional<BSONObj>& filterObj) const {
    return appendCollectionStorageStats(expCtx->getOperationContext(),
                                        nss,
                                        spec,
                                        expCtx->getSerializationContext(),
                                        builder,
                                        filterObj);
}

Status CommonMongodProcessInterface::appendRecordCount(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       BSONObjBuilder* builder) const {
    return appendCollectionRecordCount(opCtx, nss, builder);
}

void CommonMongodProcessInterface::appendOperationStats(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        BSONObjBuilder* builder) const {
    // Using kPretendUnsharded as the helper accesses only indexStats and not user data.
    auto acquisition = acquireCollectionOrViewMaybeLockFree(
        opCtx,
        CollectionOrViewAcquisitionRequest(nss,
                                           PlacementConcern::kPretendUnsharded,
                                           repl::ReadConcernArgs::get(opCtx),
                                           AcquisitionPrerequisites::kRead));
    if (!isQEColl(acquisition)) {
        Top::getDecoration(opCtx).appendOperationStats(nss, builder);
    }
}

Status CommonMongodProcessInterface::appendQueryExecStats(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          BSONObjBuilder* builder) const {
    // Using kPretendUnsharded as the helper accesses only query execution stats and not user data.
    auto acquisition = acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern::kPretendUnsharded,
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead));
    if (!acquisition.exists()) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection [" << nss.toStringForErrorMsg() << "] not found."};
    }

    if (!isQEColl(acquisition)) {
        const auto& collPtr = acquisition.getCollectionPtr();
        auto collectionScanStats =
            CollectionIndexUsageTrackerDecoration::getCollectionScanStats(collPtr.get());

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
    auto acquisition =
        acquireCollectionOrViewMaybeLockFree(opCtx,
                                             CollectionOrViewAcquisitionRequest::fromOpCtx(
                                                 opCtx,
                                                 nss,
                                                 AcquisitionPrerequisites::OperationType::kRead,
                                                 AcquisitionPrerequisites::ViewMode::kCanBeView));

    if (auto& collectionPtr = acquisition.getCollectionPtr(); collectionPtr) {
        return collectionPtr->getCollectionOptions().toBSON();
    }

    if (acquisition.isView()) {
        const auto view = acquisition.getView().getViewDefinition();
        if (view.timeseries()) {
            return getCollectionOptionsLocally(opCtx, nss.makeTimeseriesBucketsNamespace());
        }
        BSONObjBuilder bob;
        bob.append("viewOn", view.viewOn().coll());
        bob.append("pipeline", view.pipeline());
        if (view.defaultCollator()) {
            bob.append("collation", view.defaultCollator()->getSpec().toBSON());
        }
        return bob.obj();
    }

    return {};
}

BSONObj CommonMongodProcessInterface::getCollectionOptions(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    return getCollectionOptionsLocally(opCtx, nss);
}

UUID CommonMongodProcessInterface::fetchCollectionUUIDFromPrimary(OperationContext* opCtx,
                                                                  const NamespaceString& nss) {
    BSONObj options = getCollectionOptions(opCtx, nss);
    auto uuid = UUID::parse(options["uuid"_sd]);
    return uassertStatusOK(uuid);
}

query_shape::CollectionType CommonMongodProcessInterface::getCollectionTypeLocally(
    OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollectionOrViewMaybeLockFree(opCtx,
                                                CollectionOrViewAcquisitionRequest::fromOpCtx(
                                                    opCtx,
                                                    nss,
                                                    AcquisitionPrerequisites::OperationType::kRead,
                                                    AcquisitionPrerequisites::ViewMode::kCanBeView))
        .getCollectionType();
}

query_shape::CollectionType CommonMongodProcessInterface::getCollectionType(
    OperationContext* opCtx, const NamespaceString& nss) {
    return getCollectionTypeLocally(opCtx, nss);
}

std::unique_ptr<Pipeline> CommonMongodProcessInterface::attachCursorSourceToPipelineForLocalRead(
    Pipeline* ownedPipeline,
    boost::optional<const AggregateCommandRequest&> aggRequest,
    bool shouldUseCollectionDefaultCollator,
    ExecShardFilterPolicy shardFilterPolicy) {
    auto expCtx = ownedPipeline->getContext();

    // TODO: SPM-4050 Remove this.
    boost::optional<BypassCheckAllShardRoleAcquisitionsVersioned>
        bypassCheckAllShardRoleAcquisitionsAreVersioned(
            boost::in_place_init_if,
            std::holds_alternative<ProofOfUpstreamFiltering>(shardFilterPolicy),
            expCtx->getOperationContext());

    std::unique_ptr<Pipeline> pipeline(ownedPipeline);

    const auto& sources = pipeline->getSources();
    boost::optional<DocumentSource*> firstStage =
        sources.empty() ? boost::optional<DocumentSource*>{} : sources.front().get();
    tassert(10287400,
            "Pipeline must not yet have a DocumentSourceCursor as the first stage.",
            !firstStage || !dynamic_cast<DocumentSourceCursor*>(*firstStage));

    const bool isMongotPipeline = search_helpers::isMongotPipeline(pipeline.get());
    if (!isMongotPipeline && firstStage && !(*firstStage)->constraints().requiresInputDocSource) {
        // There's no need to attach a cursor or perform collection acquisition here (for stages
        // like $documents or $collStats that will not read from a user collection). Mongot
        // pipelines will not need a cursor but _do_ need to acquire the collection to check for a
        // stale shard version.
        return pipeline;
    }

    if (expCtx->eligibleForSampling()) {
        if (auto sampleId = analyze_shard_key::tryGenerateSampleId(
                expCtx->getOperationContext(),
                expCtx->getNamespaceString(),
                analyze_shard_key::SampledCommandNameEnum::kAggregate)) {
            auto [_, letParameters] =
                expCtx->variablesParseState.transitionalCompatibilitySerialize(expCtx->variables);
            analyze_shard_key::QueryAnalysisWriter::get(expCtx->getOperationContext())
                ->addAggregateQuery(*sampleId,
                                    expCtx->getNamespaceString(),
                                    pipeline->getInitialQuery(),
                                    expCtx->getCollatorBSON(),
                                    letParameters)
                .getAsync([](auto) {});
        }
    }

    // Reparse 'pipeline' to discover whether there are secondary namespaces that we need to lock
    // when constructing our query executor.
    auto lpp = LiteParsedPipeline(expCtx->getNamespaceString(), pipeline->serializeToBson());
    std::vector<NamespaceStringOrUUID> secondaryNamespaces = lpp.getForeignExecutionNamespaces();
    auto* opCtx = expCtx->getOperationContext();

    const auto& primaryNss = expCtx->getNamespaceString();
    AutoStatsTracker tracker{opCtx,
                             primaryNss,
                             Top::LockType::ReadLocked,
                             AutoStatsTracker::LogMode::kUpdateTop,
                             DatabaseProfileSettings::get(opCtx->getServiceContext())
                                 .getDatabaseProfileLevel(primaryNss.dbName()),
                             Date_t::max(),
                             secondaryNamespaces.cbegin(),
                             secondaryNamespaces.cend()};

    CollectionOrViewAcquisitionMap allAcquisitions;
    auto initAutoGetCallback = [&]() {
        CollectionOrViewAcquisitionRequests requests;
        requests.reserve(secondaryNamespaces.size() + 1);
        std::transform(secondaryNamespaces.begin(),
                       secondaryNamespaces.end(),
                       std::back_inserter(requests),
                       [opCtx](const NamespaceStringOrUUID& nsOrUuid) {
                           return CollectionOrViewAcquisitionRequest::fromOpCtx(
                               opCtx, nsOrUuid, AcquisitionPrerequisites::kRead);
                       });
        // Append acquisition for the primary nss.
        requests.emplace_back(CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx,
            primaryNss,
            AcquisitionPrerequisites::kRead,
            AcquisitionPrerequisites::kMustBeCollection));

        // Acquire all the nss at the same snapshot.
        allAcquisitions =
            makeAcquisitionMap(acquireCollectionsOrViewsMaybeLockFree(opCtx, requests));
    };

    bool isAnySecondaryCollectionNotLocal =
        initializeAutoGet(opCtx, primaryNss, secondaryNamespaces, initAutoGetCallback);

    // Extract the main acquisition.
    boost::optional<CollectionOrViewAcquisition> primaryAcquisition =
        allAcquisitions.extract(primaryNss).mapped();
    auto secondaryAcquisitions = std::move(allAcquisitions);

    tassert(10004200,
            "Expected the primary namespace to be a collection.",
            primaryAcquisition.has_value() && primaryAcquisition->isCollection());

    bool isAnySecondaryNamespaceAView =
        std::any_of(secondaryAcquisitions.begin(),
                    secondaryAcquisitions.end(),
                    [](const auto& acq) { return acq.second.isView(); });

    bool isAnySecondaryNamespaceAViewOrNotFullyLocal =
        isAnySecondaryNamespaceAView || isAnySecondaryCollectionNotLocal;

    const auto& collPtr = primaryAcquisition->getCollection().getCollectionPtr();
    if (aggRequest && aggRequest->getCollectionUUID() && collPtr) {
        checkCollectionUUIDMismatch(
            opCtx, expCtx->getNamespaceString(), collPtr, aggRequest->getCollectionUUID());
    }

    // Attach collection's default collator to the 'expCtx' if 'shouldUseCollectionDefaultCollator'
    // is specified.
    const bool canCloneCollectionDefaultCollator = collPtr && collPtr->getDefaultCollator();
    if (shouldUseCollectionDefaultCollator && canCloneCollectionDefaultCollator) {
        expCtx->setCollator(collPtr->getDefaultCollator()->clone());
    }

    MultipleCollectionAccessor holder{
        *primaryAcquisition, secondaryAcquisitions, isAnySecondaryNamespaceAViewOrNotFullyLocal};

    auto resolvedAggRequest = aggRequest ? &aggRequest.get() : nullptr;
    auto sharedStasher = make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
    auto catalogResourceHandle = make_intrusive<DSCursorCatalogResourceHandle>(sharedStasher);
    PipelineD::buildAndAttachInnerQueryExecutorToPipeline(holder,
                                                          expCtx->getNamespaceString(),
                                                          resolvedAggRequest,
                                                          pipeline.get(),
                                                          catalogResourceHandle,
                                                          shardFilterPolicy);

    if (isMongotPipeline) {
        // For mongot pipelines, we will not have a cursor attached and now must perform
        // $search-specific stage preparation. It's important that we release locks early, before
        // preparing the pipeline, so that we don't hold them during network calls to mongot. This
        // is fine for search pipelines since they are not reading any local (lock-protected) data
        // in the main pipeline. It was important that we still acquired the collection in order to
        // check for a stale shard version.
        holder.clear();
        primaryAcquisition.reset();
        secondaryAcquisitions.clear();
        search_helpers::prepareSearchForNestedPipelineLegacyExecutor(pipeline.get());
    }

    // Stash resources to free locks.
    stashTransactionResourcesFromOperationContext(opCtx, sharedStasher.get());

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
    return CursorManager::get(expCtx->getOperationContext())
        ->getIdleCursors(expCtx->getOperationContext(), userMode);
}

boost::optional<Document> CommonMongodProcessInterface::doLookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUUID,
    const Document& documentKey,
    MakePipelineOptions opts) {
    std::unique_ptr<Pipeline> pipeline;
    try {
        // Pass empty collator in order avoid inheriting the collator from 'expCtx', which may be
        // different from the collator of the corresponding collection.
        auto foreignExpCtx = makeCopyFromExpressionContext(
            expCtx, nss, collectionUUID, std::unique_ptr<CollatorInterface>());

        // If we are here, we are either executing the pipeline normally or running in one of the
        // execution stat explain verbosities. In either case, we disable explain on the foreign
        // context so that we actually retrieve the document.
        foreignExpCtx->setExplain(boost::none);

        AggregateCommandRequest aggRequest(nss, {BSON("$match" << documentKey)});
        if (collectionUUID) {
            aggRequest.setCollectionUUID(collectionUUID);
        }
        pipeline = Pipeline::makePipeline(
            aggRequest, foreignExpCtx, boost::none /* shardCursorsSortSpec */, opts);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
        LOGV2_DEBUG(
            6726700, 1, "Namespace not found while looking up document", "error"_attr = redact(ex));
        return boost::none;
    } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>& ex) {
        LOGV2_DEBUG(9597600,
                    1,
                    "Target collection UUID is different from the expected UUID",
                    "error"_attr = redact(ex));
        return boost::none;
    }

    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());
    auto lookedUpDocument = execPipeline->getNext();

    // Ensure that there are no two documents for the same 'documentKey'.
    if (auto next = execPipeline->getNext()) {
        uasserted(ErrorCodes::TooManyMatchingDocuments,
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
        return !matchExp ? true : exec::matcher::matchesBSON(matchExp, obj);
    };

    // Using kPretendUnsharded as the helper accesses only plan cache entry stats and not user data.
    auto acquisition =
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest(nss,
                                                       PlacementConcern::kPretendUnsharded,
                                                       repl::ReadConcernArgs::get(opCtx),
                                                       AcquisitionPrerequisites::kRead),
                          MODE_IS);
    uassert(50933,
            str::stream() << "collection '" << nss.toStringForErrorMsg() << "' does not exist",
            acquisition.exists());

    const auto& collPtr = acquisition.getCollectionPtr();
    const auto& collQueryInfo = CollectionQueryInfo::get(collPtr);
    const auto planCache = collQueryInfo.getPlanCache();
    invariant(planCache);

    auto planCacheEntries =
        planCache->getMatchingStats({} /* cacheKeyFilterFunc */, serializer, predicate);

    // Retrieve plan cache entries from the SBE plan cache.
    const auto cacheKeyFilter = [uuid = acquisition.uuid(),
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

MongoProcessInterface::SupportingUniqueIndex
CommonMongodProcessInterface::fieldsHaveSupportingUniqueIndex(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const std::set<FieldPath>& fieldPaths) const {
    auto* opCtx = expCtx->getOperationContext();

    // This method just checks metadata of the collection, which should be consistent across all
    // shards therefore it's safe to ignore placement concern when locking the collection for read
    // and acquiring a reference to it.
    const auto collection = acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern::kPretendUnsharded,
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead));
    if (!collection.exists()) {
        return fieldPaths == std::set<FieldPath>{"_id"} ? SupportingUniqueIndex::Full
                                                        : SupportingUniqueIndex::None;
    }
    auto indexIterator = collection.getCollectionPtr()->getIndexCatalog()->getIndexIterator(
        IndexCatalog::InclusionPolicy::kReady);
    auto result = SupportingUniqueIndex::None;
    while (indexIterator->more()) {
        const IndexCatalogEntry* entry = indexIterator->next();
        result = std::max(result, supportsUniqueKey(expCtx, entry, fieldPaths));
        if (result == SupportingUniqueIndex::Full) {
            break;
        }
    }
    return result;
}

BSONObj CommonMongodProcessInterface::_reportCurrentOpForClient(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Client* client,
    CurrentOpTruncateMode truncateOps) const {
    BSONObjBuilder builder;

    CurOp::reportCurrentOpForClient(
        expCtx, client, (truncateOps == CurrentOpTruncateMode::kTruncateOps), &builder);

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

    const bool authEnabled = AuthorizationManager::get(opCtx->getService())->isAuthEnabled();

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

CommonMongodProcessInterface::DocumentKeyResolutionMetadata
CommonMongodProcessInterface::ensureFieldsUniqueOrResolveDocumentKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<std::set<FieldPath>> fieldPaths,
    boost::optional<ChunkVersion> targetCollectionPlacementVersion,
    const NamespaceString& outputNs) const {
    uassert(51123,
            "Unexpected target chunk version specified",
            !targetCollectionPlacementVersion || expCtx->getFromRouter());

    if (!fieldPaths) {
        uassert(51124, "Expected fields to be provided from router", !expCtx->getFromRouter());
        return {std::set<FieldPath>{"_id"},
                targetCollectionPlacementVersion,
                SupportingUniqueIndex::Full};
    }

    // Make sure the 'fields' array has a supporting index. Skip this check if the command is sent
    // from router since the 'fields' check would've happened already.
    auto supportingUniqueIndex = fieldsHaveSupportingUniqueIndex(expCtx, outputNs, *fieldPaths);
    if (!expCtx->getFromRouter()) {
        uassert(51183,
                "Cannot find index to verify that join fields will be unique",
                supportingUniqueIndex != SupportingUniqueIndex::None);
    }
    return {*fieldPaths, targetCollectionPlacementVersion, supportingUniqueIndex};
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

namespace {
// TODO SERVER-106716 Remove this
template <typename SpillTableWriteOperation>
void withWriteUnitOfWorkIfNeeded(OperationContext* opCtx, SpillTableWriteOperation operation) {
    if (feature_flags::gFeatureFlagCreateSpillKVEngine.isEnabled()) {
        operation();
    } else {
        WriteUnitOfWork wuow(opCtx);
        operation();
        wuow.commit();
    }
}
}  // namespace

void CommonMongodProcessInterface::writeRecordsToSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    SpillTable& spillTable,
    std::vector<Record>* records) const {
    tassert(5643012, "Attempted to write to record store with nullptr", records);
    assertIgnorePrepareConflictsBehavior(expCtx);
    writeConflictRetry(
        expCtx->getOperationContext(),
        "MPI::writeRecordsToSpillTable",
        expCtx->getNamespaceString(),
        [&] {
            Lock::GlobalLock lk = acquireLockForSpillTable(expCtx->getOperationContext());
            withWriteUnitOfWorkIfNeeded(expCtx->getOperationContext(), [&]() {
                auto writeResult = spillTable.insertRecords(expCtx->getOperationContext(), records);
                uassert(ErrorCodes::OutOfDiskSpace,
                        str::stream() << "Failed to write to disk because " << writeResult.reason(),
                        writeResult.isOK());
            });
        }

    );
}

std::unique_ptr<SpillTable> CommonMongodProcessInterface::createSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, KeyFormat keyFormat) const {
    assertIgnorePrepareConflictsBehavior(expCtx);
    Lock::GlobalLock lk = acquireLockForSpillTable(expCtx->getOperationContext());
    if (feature_flags::gFeatureFlagCreateSpillKVEngine.isEnabled()) {
        return expCtx->getOperationContext()
            ->getServiceContext()
            ->getStorageEngine()
            ->makeSpillTable(expCtx->getOperationContext(),
                             keyFormat,
                             internalQuerySpillingMinAvailableDiskSpaceBytes.load());
    }
    auto storageEngine = expCtx->getOperationContext()->getServiceContext()->getStorageEngine();
    return storageEngine->makeTemporaryRecordStore(
        expCtx->getOperationContext(), storageEngine->generateNewInternalIdent(), keyFormat);
}

Document CommonMongodProcessInterface::readRecordFromSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const SpillTable& spillTable,
    RecordId rID) const {
    RecordData possibleRecord;
    Lock::GlobalLock lk = acquireLockForSpillTable(expCtx->getOperationContext());
    auto foundDoc =
        spillTable.findRecord(expCtx->getOperationContext(), RecordId(rID), &possibleRecord);
    tassert(775101, str::stream() << "Could not find document id " << rID, foundDoc);
    return Document::fromBsonWithMetaData(possibleRecord.toBson());
}

bool CommonMongodProcessInterface::checkRecordInSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const SpillTable& spillTable,
    RecordId rID) const {
    RecordData possibleRecord;
    Lock::GlobalLock lk = acquireLockForSpillTable(expCtx->getOperationContext());
    return spillTable.findRecord(expCtx->getOperationContext(), RecordId(rID), &possibleRecord);
}

void CommonMongodProcessInterface::deleteRecordFromSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    SpillTable& spillTable,
    RecordId rID) const {
    assertIgnorePrepareConflictsBehavior(expCtx);
    writeConflictRetry(expCtx->getOperationContext(),
                       "MPI::deleteRecordFromSpillTable",
                       expCtx->getNamespaceString(),
                       [&] {
                           Lock::GlobalLock lk =
                               acquireLockForSpillTable(expCtx->getOperationContext());
                           withWriteUnitOfWorkIfNeeded(expCtx->getOperationContext(), [&]() {
                               spillTable.deleteRecord(expCtx->getOperationContext(), rID);
                           });
                       });
}

void CommonMongodProcessInterface::truncateSpillTable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, SpillTable& spillTable) const {
    assertIgnorePrepareConflictsBehavior(expCtx);
    writeConflictRetry(expCtx->getOperationContext(),
                       "MPI::truncateSpillTable",
                       expCtx->getNamespaceString(),
                       [&] {
                           Lock::GlobalLock lk =
                               acquireLockForSpillTable(expCtx->getOperationContext());
                           withWriteUnitOfWorkIfNeeded(expCtx->getOperationContext(), [&]() {
                               auto status = spillTable.truncate(expCtx->getOperationContext());
                               tassert(5643000, "Unable to clear record store", status.isOK());
                           });
                       });
}

boost::optional<Document> CommonMongodProcessInterface::lookupSingleDocumentLocally(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    const Document& documentKey) {
    OperationContext* opCtx = expCtx->getOperationContext();
    // Using kPretendUnsharded (and skipping version check) as this helper is only used to access
    // the config.system.preimages which is always present and local.
    const auto acquisition = acquireCollectionMaybeLockFree(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern::kPretendUnsharded,
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead));
    BSONObj document;
    if (!Helpers::findById(expCtx->getOperationContext(), nss, documentKey.toBson(), document)) {
        return boost::none;
    }
    return Document(document).getOwned();
}

}  // namespace mongo
