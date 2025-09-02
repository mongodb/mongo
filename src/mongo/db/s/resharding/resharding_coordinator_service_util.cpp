/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/routing_information_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/commit_reshard_collection_gen.h"
#include "mongo/db/global_catalog/ddl/drop_collection_if_uuid_not_matching_gen.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/shard_role_catalog/flush_routing_table_cache_updates_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/balancer/balance_stats.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/request_types/abort_reshard_collection_gen.h"
#include "mongo/s/request_types/flush_resharding_state_change_gen.h"
#include "mongo/s/request_types/update_zone_key_range_gen.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_coordinator_service_conflicting_op_in_progress_info.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/future_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {

namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

void assertNumDocsMatchedEqualsExpected(const BatchedCommandRequest& request,
                                        const BSONObj& response,
                                        int expected) {
    auto numDocsMatched = response.getIntField("n");
    uassert(5030401,
            str::stream() << "Expected to match " << expected << " docs, but only matched "
                          << numDocsMatched << " for write request " << request.toString(),
            expected == numDocsMatched);
}

void appendDonorShardEntriesToSetBuilder(const ReshardingCoordinatorDocument& coordinatorDoc,
                                         BSONObjBuilder& setBuilder) {
    BSONArrayBuilder donorShards(
        setBuilder.subarrayStart(ReshardingCoordinatorDocument::kDonorShardsFieldName));
    for (const auto& donorShard : coordinatorDoc.getDonorShards()) {
        donorShards.append(donorShard.toBSON());
    }
    donorShards.doneFast();
}

void appendRecipientShardEntriesToSetBuilder(const ReshardingCoordinatorDocument& coordinatorDoc,
                                             BSONObjBuilder& setBuilder) {
    BSONArrayBuilder recipientShards(
        setBuilder.subarrayStart(ReshardingCoordinatorDocument::kRecipientShardsFieldName));
    for (const auto& recipientShard : coordinatorDoc.getRecipientShards()) {
        recipientShards.append(recipientShard.toBSON());
    }
    recipientShards.doneFast();
}

void appendShardEntriesToSetBuilder(const ReshardingCoordinatorDocument& coordinatorDoc,
                                    BSONObjBuilder& setBuilder) {
    appendDonorShardEntriesToSetBuilder(coordinatorDoc, setBuilder);
    appendRecipientShardEntriesToSetBuilder(coordinatorDoc, setBuilder);
}

void unsetInitializingFields(BSONObjBuilder& updateBuilder) {
    BSONObjBuilder unsetBuilder(updateBuilder.subobjStart("$unset"));
    unsetBuilder.append(ReshardingCoordinatorDocument::kPresetReshardedChunksFieldName, "");
    unsetBuilder.append(ReshardingCoordinatorDocument::kZonesFieldName, "");
    unsetBuilder.doneFast();
}

using resharding_metrics::getIntervalEndFieldName;
using resharding_metrics::getIntervalStartFieldName;
using DocT = ReshardingCoordinatorDocument;
const auto metricsPrefix = resharding_metrics::getMetricsPrefix<DocT>();


void buildStateDocumentCloneMetricsForUpdate(BSONObjBuilder& bob, Date_t timestamp) {
    bob.append(getIntervalStartFieldName<DocT>(ReshardingRecipientMetrics::kDocumentCopyFieldName),
               timestamp);
}

void buildStateDocumentApplyMetricsForUpdate(BSONObjBuilder& bob, Date_t timestamp) {
    bob.append(getIntervalEndFieldName<DocT>(ReshardingRecipientMetrics::kDocumentCopyFieldName),
               timestamp);

    bob.append(
        getIntervalStartFieldName<DocT>(ReshardingRecipientMetrics::kOplogApplicationFieldName),
        timestamp);
}

void buildStateDocumentBlockingWritesMetricsForUpdate(BSONObjBuilder& bob, Date_t timestamp) {
    bob.append(
        getIntervalEndFieldName<DocT>(ReshardingRecipientMetrics::kOplogApplicationFieldName),
        timestamp);
}

// TODO SERVER-102424: Remove once we can delete writeToCoordinatorStateNss().
void buildStateDocumentMetricsForUpdate(BSONObjBuilder& bob,
                                        CoordinatorStateEnum newState,
                                        Date_t timestamp) {
    switch (newState) {
        case CoordinatorStateEnum::kCloning:
            buildStateDocumentCloneMetricsForUpdate(bob, timestamp);
            return;
        case CoordinatorStateEnum::kApplying:
            buildStateDocumentApplyMetricsForUpdate(bob, timestamp);
            return;
        case CoordinatorStateEnum::kBlockingWrites:
            buildStateDocumentBlockingWritesMetricsForUpdate(bob, timestamp);
            return;
        default:
            return;
    }
}

void setMeticsAfterWrite(ReshardingMetrics* metrics,
                         CoordinatorStateEnum newState,
                         Date_t timestamp) {
    switch (newState) {
        case CoordinatorStateEnum::kCloning:
            metrics->setStartFor(ReshardingMetrics::TimedPhase::kCloning, timestamp);
            return;
        case CoordinatorStateEnum::kApplying:
            metrics->setEndFor(ReshardingMetrics::TimedPhase::kCloning, timestamp);
            metrics->setStartFor(ReshardingMetrics::TimedPhase::kApplying, timestamp);
            return;
        case CoordinatorStateEnum::kBlockingWrites:
            metrics->setEndFor(ReshardingMetrics::TimedPhase::kApplying, timestamp);
            return;
        default:
            return;
    }
}

BSONObj createReshardingFieldsUpdateForOriginalNss(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<OID> newCollectionEpoch,
    boost::optional<Timestamp> newCollectionTimestamp) {
    auto nextState = coordinatorDoc.getState();
    switch (nextState) {
        case CoordinatorStateEnum::kInitializing: {
            // Append 'reshardingFields' to the config.collections entry for the original nss
            TypeCollectionReshardingFields originalEntryReshardingFields(
                coordinatorDoc.getReshardingUUID());
            originalEntryReshardingFields.setState(coordinatorDoc.getState());
            originalEntryReshardingFields.setStartTime(coordinatorDoc.getStartTime());
            originalEntryReshardingFields.setProvenance(
                coordinatorDoc.getCommonReshardingMetadata().getProvenance());
            originalEntryReshardingFields.setPerformVerification(
                coordinatorDoc.getCommonReshardingMetadata().getPerformVerification());

            return BSON("$set" << BSON(CollectionType::kReshardingFieldsFieldName
                                       << originalEntryReshardingFields.toBSON()
                                       << CollectionType::kUpdatedAtFieldName
                                       << opCtx->getServiceContext()->getPreciseClockSource()->now()
                                       << CollectionType::kAllowMigrationsFieldName << false));
        }
        case CoordinatorStateEnum::kPreparingToDonate: {
            TypeCollectionDonorFields donorFields(coordinatorDoc.getTempReshardingNss(),
                                                  coordinatorDoc.getReshardingKey(),
                                                  resharding::extractShardIdsFromParticipantEntries(
                                                      coordinatorDoc.getRecipientShards()));

            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));
                {
                    setBuilder.append(CollectionType::kReshardingFieldsFieldName + "." +
                                          TypeCollectionReshardingFields::kStateFieldName,
                                      CoordinatorState_serializer(nextState));

                    setBuilder.append(CollectionType::kReshardingFieldsFieldName + "." +
                                          TypeCollectionReshardingFields::kDonorFieldsFieldName,
                                      donorFields.toBSON());

                    setBuilder.append(CollectionType::kUpdatedAtFieldName,
                                      opCtx->getServiceContext()->getPreciseClockSource()->now());
                }

                setBuilder.doneFast();
            }

            return updateBuilder.obj();
        }
        case CoordinatorStateEnum::kCommitting: {
            // Update the config.collections entry for the original nss to reflect the new sharded
            // collection. Set 'uuid' to the reshardingUUID, 'key' to the new shard key,
            // 'lastmodEpoch' to newCollectionEpoch, and 'timestamp' to newCollectionTimestamp. Also
            // update the 'state' field and add the 'recipientFields' to the 'reshardingFields'
            // section.
            auto recipientFields = resharding::constructRecipientFields(coordinatorDoc);
            BSONObj setFields =
                BSON("uuid" << coordinatorDoc.getReshardingUUID() << "key"
                            << coordinatorDoc.getReshardingKey().toBSON() << "lastmodEpoch"
                            << newCollectionEpoch.value() << "lastmod"
                            << opCtx->getServiceContext()->getPreciseClockSource()->now()
                            << "reshardingFields.state"
                            << CoordinatorState_serializer(coordinatorDoc.getState())
                            << "reshardingFields.recipientFields" << recipientFields.toBSON());
            if (newCollectionTimestamp.has_value()) {
                setFields =
                    setFields.addFields(BSON("timestamp" << newCollectionTimestamp.value()));
            }
            auto provenance = coordinatorDoc.getCommonReshardingMetadata().getProvenance();
            if (provenance && provenance.get() == ReshardingProvenanceEnum::kUnshardCollection) {
                setFields = setFields.addFields(BSON("unsplittable" << true));
            }

            return BSON("$set" << setFields);
        }
        case mongo::CoordinatorStateEnum::kQuiesced:
        case mongo::CoordinatorStateEnum::kDone:
            // Remove 'reshardingFields' from the config.collections entry
            return BSON(
                "$unset" << BSON(CollectionType::kReshardingFieldsFieldName
                                 << "" << CollectionType::kAllowMigrationsFieldName << "")
                         << "$set"
                         << BSON(CollectionType::kUpdatedAtFieldName
                                 << opCtx->getServiceContext()->getPreciseClockSource()->now()));
        default: {
            // Update the 'state' field, and 'abortReason' field if it exists, in the
            // 'reshardingFields' section.
            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                setBuilder.append("reshardingFields.state",
                                  std::string{CoordinatorState_serializer(nextState)});
                setBuilder.append("lastmod",
                                  opCtx->getServiceContext()->getPreciseClockSource()->now());

                if (auto abortReason = coordinatorDoc.getAbortReason()) {
                    // If the abortReason exists, include it in the update.
                    setBuilder.append("reshardingFields.abortReason", *abortReason);

                    auto abortStatus = resharding::getStatusFromAbortReason(coordinatorDoc);
                    setBuilder.append("reshardingFields.userCanceled",
                                      abortStatus == ErrorCodes::ReshardCollectionAborted);
                }

                setBuilder.doneFast();

                if (coordinatorDoc.getAbortReason()) {
                    updateBuilder.append("$unset",
                                         BSON(CollectionType::kAllowMigrationsFieldName << ""));
                }
            }

            return updateBuilder.obj();
        }
    }
}

void updateConfigCollectionsForOriginalNss(OperationContext* opCtx,
                                           const ReshardingCoordinatorDocument& coordinatorDoc,
                                           boost::optional<OID> newCollectionEpoch,
                                           boost::optional<Timestamp> newCollectionTimestamp,
                                           TxnNumber txnNumber) {
    auto writeOp = createReshardingFieldsUpdateForOriginalNss(
        opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp);

    auto request = BatchedCommandRequest::buildUpdateOp(
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName
             << NamespaceStringUtil::serialize(coordinatorDoc.getSourceNss(),
                                               SerializationContext::stateDefault())),  // query
        writeOp,
        false,  // upsert
        false   // multi
    );

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, request, txnNumber);

    assertNumDocsMatchedEqualsExpected(request, res, 1 /* expected */);
}

/*
 * Updates the collection UUID of the QueryAnalyzerDocument for a collection being resharded if
 * query sampling is enabled.
 */
void updateQueryAnalyzerMetadata(OperationContext* opCtx,
                                 const ReshardingCoordinatorDocument& coordinatorDoc,
                                 TxnNumber txnNumber) {

    auto writeOp =
        BSON("$set" << BSON(analyze_shard_key::QueryAnalyzerDocument::kCollectionUuidFieldName
                            << coordinatorDoc.getReshardingUUID()));

    auto request = BatchedCommandRequest::buildUpdateOp(
        NamespaceString::kConfigQueryAnalyzersNamespace,
        BSON(analyze_shard_key::QueryAnalyzerDocument::kCollectionUuidFieldName
             << coordinatorDoc.getSourceUUID()),  // query
        writeOp,
        false,  // upsert
        false   // multi
    );

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, NamespaceString::kConfigQueryAnalyzersNamespace, request, txnNumber);
}

void writeToConfigPlacementHistoryForOriginalNss(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    const Timestamp& newCollectionTimestamp,
    const std::vector<ShardId>& reshardedCollectionPlacement,
    TxnNumber txnNumber) {
    invariant(coordinatorDoc.getState() == CoordinatorStateEnum::kCommitting,
              "New placement data on the collection being resharded can only be persisted at "
              "commit time");

    NamespacePlacementType placementInfo(
        coordinatorDoc.getSourceNss(), newCollectionTimestamp, reshardedCollectionPlacement);
    placementInfo.setUuid(coordinatorDoc.getReshardingUUID());

    auto request = BatchedCommandRequest::buildInsertOp(
        NamespaceString::kConfigsvrPlacementHistoryNamespace, {placementInfo.toBSON()});

    auto response = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, request, txnNumber);

    assertNumDocsMatchedEqualsExpected(request, response, 1);
}

void insertChunksForTempNss(OperationContext* opCtx, const std::vector<ChunkType>& initialChunks) {
    // Insert new initial chunk documents for temp nss
    std::vector<BSONObj> initialChunksBSON(initialChunks.size());
    std::transform(initialChunks.begin(),
                   initialChunks.end(),
                   initialChunksBSON.begin(),
                   [](ChunkType chunk) { return chunk.toConfigBSON(); });

    ShardingCatalogManager::get(opCtx)->insertConfigDocuments(
        opCtx, NamespaceString::kConfigsvrChunksNamespace, std::move(initialChunksBSON));
}

void setupZonesForTempNss(OperationContext* opCtx,
                          const NamespaceString& nss,
                          std::vector<ReshardingZoneType> newZones) {
    for (const auto& zone : newZones) {
        BSONObjBuilder cmdBuilder;
        ConfigsvrUpdateZoneKeyRange cmd(
            nss, zone.getMin(), zone.getMax(), std::string{zone.getZone()});
        cmd.serialize(&cmdBuilder);
        cmdBuilder.append("writeConcern", resharding::kMajorityWriteConcern.toBSON());

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
        auto cmdResponseStatus =
            uassertStatusOK(configShard->runCommand(opCtx,
                                                    kPrimaryOnlyReadPreference,
                                                    DatabaseName::kAdmin,
                                                    cmdBuilder.obj(),
                                                    Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(cmdResponseStatus.commandStatus);
    }
}

void removeTagsDocs(OperationContext* opCtx, const BSONObj& tagsQuery, TxnNumber txnNumber) {
    // Remove tag documents with the specified tagsQuery.
    const auto tagDeleteOperationHint = BSON(TagsType::ns() << 1 << TagsType::min() << 1);
    ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx,
        TagsType::ConfigNS,
        BatchedCommandRequest::buildDeleteOp(TagsType::ConfigNS,
                                             tagsQuery,              // query
                                             true,                   // multi
                                             tagDeleteOperationHint  // hint
                                             ),
        txnNumber);
}

// Requires that there be no session information on the opCtx.
void removeChunkAndTagsDocs(OperationContext* opCtx,
                            const BSONObj& tagsQuery,
                            const UUID& collUUID) {
    // Remove all chunk documents and specified tag documents.
    resharding::removeChunkDocs(opCtx, collUUID);

    const auto tagDeleteOperationHint = BSON(TagsType::ns() << 1 << TagsType::min() << 1);
    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    uassertStatusOK(catalogClient->removeConfigDocuments(opCtx,
                                                         TagsType::ConfigNS,
                                                         tagsQuery,
                                                         resharding::kMajorityWriteConcern,
                                                         tagDeleteOperationHint));
}

std::shared_ptr<async_rpc::AsyncRPCOptions<_flushReshardingStateChange>>
createFlushReshardingStateChangeOptions(const NamespaceString& nss,
                                        const UUID& reshardingUUID,
                                        const std::shared_ptr<executor::TaskExecutor>& exec,
                                        CancellationToken token) {
    _flushReshardingStateChange cmd(nss);
    cmd.setDbName(DatabaseName::kAdmin);
    cmd.setReshardingUUID(reshardingUUID);
    auto opts =
        std::make_shared<async_rpc::AsyncRPCOptions<_flushReshardingStateChange>>(exec, token, cmd);
    return opts;
}

std::shared_ptr<async_rpc::AsyncRPCOptions<FlushRoutingTableCacheUpdatesWithWriteConcern>>
makeFlushRoutingTableCacheUpdatesOptions(const NamespaceString& nss,
                                         const std::shared_ptr<executor::TaskExecutor>& exec,
                                         CancellationToken token) {
    auto cmd = FlushRoutingTableCacheUpdatesWithWriteConcern(nss);
    cmd.setSyncFromConfig(true);
    cmd.setDbName(nss.dbName());
    generic_argument_util::setMajorityWriteConcern(cmd, &resharding::kMajorityWriteConcern);
    auto opts =
        std::make_shared<async_rpc::AsyncRPCOptions<FlushRoutingTableCacheUpdatesWithWriteConcern>>(
            exec, token, cmd);
    return opts;
}

}  // namespace

namespace resharding {

/**
 * Creates reshardingFields.recipientFields for the resharding operation. Note: these should not
 * change once the operation has begun.
 */
TypeCollectionRecipientFields constructRecipientFields(
    const ReshardingCoordinatorDocument& coordinatorDoc) {
    std::vector<DonorShardFetchTimestamp> donorShards;

    for (const auto& donor : coordinatorDoc.getDonorShards()) {
        DonorShardFetchTimestamp donorFetchTimestamp(donor.getId());
        donorFetchTimestamp.setMinFetchTimestamp(donor.getMutableState().getMinFetchTimestamp());
        donorShards.push_back(std::move(donorFetchTimestamp));
    }

    TypeCollectionRecipientFields recipientFields(
        std::move(donorShards),
        coordinatorDoc.getSourceUUID(),
        coordinatorDoc.getSourceNss(),
        coordinatorDoc.getDemoMode()
            ? 0
            : resharding::gReshardingMinimumOperationDurationMillis.load());

    resharding::emplaceCloneTimestampIfExists(recipientFields, coordinatorDoc.getCloneTimestamp());
    resharding::emplaceApproxBytesToCopyIfExists(
        recipientFields, coordinatorDoc.getReshardingApproxCopySizeStruct());
    resharding::emplaceOplogBatchTaskCountIfExists(
        recipientFields, coordinatorDoc.getRecipientOplogBatchTaskCount());
    resharding::emplaceRelaxedIfExists(recipientFields, coordinatorDoc.getRelaxed());
    return recipientFields;
}

CollectionType createTempReshardingCollectionType(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    const ChunkVersion& chunkVersion,
    const BSONObj& collation,
    boost::optional<bool> isUnsplittable) {
    CollectionType collType(coordinatorDoc.getTempReshardingNss(),
                            chunkVersion.epoch(),
                            chunkVersion.getTimestamp(),
                            opCtx->getServiceContext()->getPreciseClockSource()->now(),
                            coordinatorDoc.getReshardingUUID(),
                            coordinatorDoc.getReshardingKey());
    collType.setDefaultCollation(collation);

    if (isUnsplittable.has_value() && isUnsplittable.get()) {
        collType.setUnsplittable(isUnsplittable.get());
    }

    TypeCollectionReshardingFields tempEntryReshardingFields(coordinatorDoc.getReshardingUUID());
    tempEntryReshardingFields.setState(coordinatorDoc.getState());
    tempEntryReshardingFields.setStartTime(coordinatorDoc.getStartTime());
    tempEntryReshardingFields.setProvenance(
        coordinatorDoc.getCommonReshardingMetadata().getProvenance());
    tempEntryReshardingFields.setPerformVerification(
        coordinatorDoc.getCommonReshardingMetadata().getPerformVerification());

    auto recipientFields = constructRecipientFields(coordinatorDoc);
    tempEntryReshardingFields.setRecipientFields(std::move(recipientFields));
    collType.setReshardingFields(std::move(tempEntryReshardingFields));
    collType.setAllowMigrations(false);

    return collType;
}

void removeChunkDocs(OperationContext* opCtx, const UUID& collUUID) {
    // Remove all chunk documents for the specified collUUID. We do not know how many chunk docs
    // currently exist, so cannot pass a value for expectedNumModified
    const auto chunksQuery = BSON(ChunkType::collectionUUID() << collUUID);
    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();

    uassertStatusOK(catalogClient->removeConfigDocuments(opCtx,
                                                         NamespaceString::kConfigsvrChunksNamespace,
                                                         chunksQuery,
                                                         resharding::kMajorityWriteConcern));
}

void writeDecisionPersistedState(OperationContext* opCtx,
                                 ReshardingMetrics* metrics,
                                 const ReshardingCoordinatorDocument& coordinatorDoc,
                                 OID newCollectionEpoch,
                                 Timestamp newCollectionTimestamp,
                                 const std::vector<ShardId>& reshardedCollectionPlacement) {

    // No need to bump originalNss version because its epoch will be changed.
    executeMetadataChangesInTxn(
        opCtx,
        [&metrics,
         &coordinatorDoc,
         &newCollectionEpoch,
         &newCollectionTimestamp,
         &reshardedCollectionPlacement](OperationContext* opCtx, TxnNumber txnNumber) {
            // Update the config.reshardingOperations entry
            writeToCoordinatorStateNss(opCtx, metrics, coordinatorDoc, txnNumber);

            // Remove the config.collections entry for the temporary collection
            writeToConfigCollectionsForTempNss(
                opCtx, coordinatorDoc, boost::none, boost::none, boost::none, txnNumber);

            // Update the config.collections entry for the original namespace to reflect the new
            // shard key, new epoch, and new UUID
            updateConfigCollectionsForOriginalNss(
                opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp, txnNumber);

            // Insert the list of recipient shard IDs (together with the new timestamp and UUID) as
            // the latest entry in config.placementHistory about the original namespace
            writeToConfigPlacementHistoryForOriginalNss(opCtx,
                                                        coordinatorDoc,
                                                        newCollectionTimestamp,
                                                        reshardedCollectionPlacement,
                                                        txnNumber);

            // Update the QueryAnalyzerDocument for the resharded collection with the new collection
            // UUID.
            updateQueryAnalyzerMetadata(opCtx, coordinatorDoc, txnNumber);

            // Delete all of the config.tags entries for the user collection namespace.
            const auto removeTagsQuery = BSON(TagsType::ns(NamespaceStringUtil::serialize(
                coordinatorDoc.getSourceNss(), SerializationContext::stateDefault())));
            removeTagsDocs(opCtx, removeTagsQuery, txnNumber);

            // Update all of the config.tags entries for the temporary resharding namespace
            // to refer to the user collection namespace.
            updateTagsDocsForTempNss(opCtx, coordinatorDoc, txnNumber);
        });
}

void updateTagsDocsForTempNss(OperationContext* opCtx,
                              const ReshardingCoordinatorDocument& coordinatorDoc,
                              TxnNumber txnNumber) {
    auto hint = BSON("ns" << 1 << "min" << 1);
    auto tagsRequest = BatchedCommandRequest::buildUpdateOp(
        TagsType::ConfigNS,
        BSON(TagsType::ns(
            NamespaceStringUtil::serialize(coordinatorDoc.getTempReshardingNss(),
                                           SerializationContext::stateDefault()))),  // query
        BSON("$set" << BSON("ns" << NamespaceStringUtil::serialize(
                                coordinatorDoc.getSourceNss(),
                                SerializationContext::stateDefault()))),  // update
        false,                                                            // upsert
        true,                                                             // multi
        hint                                                              // hint
    );

    // Update the 'ns' field to be the original collection namespace for all tags documents that
    // currently have 'ns' as the temporary collection namespace.
    auto tagsRes = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, TagsType::ConfigNS, tagsRequest, txnNumber);
}

void insertCoordDocAndChangeOrigCollEntry(OperationContext* opCtx,
                                          ReshardingMetrics* metrics,
                                          const ReshardingCoordinatorDocument& coordinatorDoc) {
    ShardingCatalogManager::get(opCtx)->bumpCollectionPlacementVersionAndChangeMetadataInTxn(
        opCtx,
        coordinatorDoc.getSourceNss(),
        [&](OperationContext* opCtx, TxnNumber txnNumber) {
            auto doc = ShardingCatalogManager::get(opCtx)->findOneConfigDocumentInTxn(
                opCtx,
                CollectionType::ConfigNS,
                txnNumber,
                BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                         coordinatorDoc.getSourceNss(), SerializationContext::stateDefault())));

            uassert(5808200,
                    str::stream() << "config.collection entry not found for "
                                  << coordinatorDoc.getSourceNss().toStringForErrorMsg(),
                    doc);

            CollectionType configCollDoc(*doc);
            uassert(5808201,
                    str::stream() << "collection " << CollectionType::kAllowMigrationsFieldName
                                  << " setting is already set to false",
                    configCollDoc.getAllowMigrations());

            // Insert the coordinator document to config.reshardingOperations.
            writeToCoordinatorStateNss(opCtx, metrics, coordinatorDoc, txnNumber);

            // Update the config.collections entry for the original collection to include
            // 'reshardingFields'
            updateConfigCollectionsForOriginalNss(
                opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);
        },
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
}

void writeParticipantShardsAndTempCollInfo(OperationContext* opCtx,
                                           ReshardingMetrics* metrics,
                                           const ReshardingCoordinatorDocument& coordinatorDoc,
                                           PhaseTransitionFn phaseTransitionFn,
                                           std::vector<ChunkType> initialChunks,
                                           std::vector<ReshardingZoneType> zones,
                                           boost::optional<bool> isUnsplittable) {
    const auto tagsQuery = BSON(TagsType::ns(NamespaceStringUtil::serialize(
        coordinatorDoc.getTempReshardingNss(), SerializationContext::stateDefault())));

    removeChunkAndTagsDocs(opCtx, tagsQuery, coordinatorDoc.getReshardingUUID());
    insertChunksForTempNss(opCtx, initialChunks);

    ShardingCatalogManager::get(opCtx)->bumpCollectionPlacementVersionAndChangeMetadataInTxn(
        opCtx,
        coordinatorDoc.getSourceNss(),
        [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Update on-disk state to reflect latest state transition.
            ReshardingCoordinatorDocument updatedCoordinatorDoc =
                phaseTransitionFn(opCtx, txnNumber);

            // Insert the config.collections entry for the temporary resharding collection. The
            // chunks all have the same epoch, so picking the last chunk here is arbitrary.
            invariant(initialChunks.size() != 0);
            auto chunkVersion = initialChunks.back().getVersion();
            writeToConfigCollectionsForTempNss(opCtx,
                                               updatedCoordinatorDoc,
                                               chunkVersion,
                                               CollationSpec::kSimpleSpec,
                                               isUnsplittable,
                                               txnNumber);

            updateConfigCollectionsForOriginalNss(
                opCtx, updatedCoordinatorDoc, boost::none, boost::none, txnNumber);
        },
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    setupZonesForTempNss(opCtx, coordinatorDoc.getTempReshardingNss(), zones);
}

void writeStateTransitionAndCatalogUpdatesThenBumpCollectionPlacementVersions(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<PhaseTransitionFn> phaseTransitionFn) {
    // Run updates to config.reshardingOperations and config.collections in a transaction
    auto nextState = coordinatorDoc.getState();

    std::vector<NamespaceString> collNames = {coordinatorDoc.getSourceNss()};
    if (nextState < CoordinatorStateEnum::kCommitting) {
        collNames.emplace_back(coordinatorDoc.getTempReshardingNss());
    }

    ShardingCatalogManager::get(opCtx)
        ->bumpMultipleCollectionPlacementVersionsAndChangeMetadataInTxn(
            opCtx,
            collNames,
            [&](OperationContext* opCtx, TxnNumber txnNumber) {
                // Update the config.reshardingOperations entry

                // TODO SERVER-103243 - once this ticket is done we can remove if statement and
                // directly call the phase transition function.
                ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
                if (phaseTransitionFn) {
                    updatedCoordinatorDoc = (*phaseTransitionFn)(opCtx, txnNumber);
                } else {
                    writeToCoordinatorStateNss(opCtx, metrics, coordinatorDoc, txnNumber);
                }

                // Update the config.collections entry for the original collection
                updateConfigCollectionsForOriginalNss(
                    opCtx, updatedCoordinatorDoc, boost::none, boost::none, txnNumber);

                // Update the config.collections entry for the temporary resharding collection. If
                // we've already successfully committed that the operation will succeed, we've
                // removed the entry for the temporary collection and updated the entry with
                // original namespace to have the new shard key, UUID, and epoch
                if (nextState < CoordinatorStateEnum::kCommitting) {
                    writeToConfigCollectionsForTempNss(opCtx,
                                                       updatedCoordinatorDoc,
                                                       boost::none,
                                                       boost::none,
                                                       boost::none,
                                                       txnNumber);
                }
            },
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
}

ReshardingCoordinatorDocument removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<Status> abortReason) {
    // If the coordinator needs to abort and isn't in kInitializing, additional collections need to
    // be cleaned up in the final transaction. Otherwise, cleanup for abort and success are the
    // same.
    const bool wasDecisionPersisted =
        coordinatorDoc.getState() >= CoordinatorStateEnum::kCommitting;
    invariant((wasDecisionPersisted && !abortReason) || abortReason);

    if (coordinatorDoc.getState() > CoordinatorStateEnum::kQuiesced) {
        return coordinatorDoc;
    }

    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    // If a user resharding ID was provided, move the coordinator doc to "quiesced" rather than
    // "done".
    if (coordinatorDoc.getUserReshardingUUID()) {
        updatedCoordinatorDoc.setState(CoordinatorStateEnum::kQuiesced);
        updatedCoordinatorDoc.setQuiescePeriodEnd(
            opCtx->fastClockSource().now() +
            Milliseconds(resharding::gReshardingCoordinatorQuiescePeriodMillis));
    } else {
        updatedCoordinatorDoc.setState(CoordinatorStateEnum::kDone);
    }
    emplaceTruncatedAbortReasonIfExists(updatedCoordinatorDoc, abortReason);

    const auto tagsQuery = BSON(TagsType::ns(NamespaceStringUtil::serialize(
        coordinatorDoc.getTempReshardingNss(), SerializationContext::stateDefault())));
    // Once the decision has been persisted, the coordinator would have modified the
    // config.chunks and config.collections entry. This means that the UUID of the
    // non-temp collection is now the UUID of what was previously the UUID of the temp
    // collection. So don't try to call remove as it will end up removing the metadata
    // for the real collection.
    if (!wasDecisionPersisted) {
        const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();

        uassertStatusOK(catalogClient->removeConfigDocuments(
            opCtx,
            CollectionType::ConfigNS,
            BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                     coordinatorDoc.getTempReshardingNss(), SerializationContext::stateDefault())),
            resharding::kMajorityWriteConcern));

        removeChunkAndTagsDocs(opCtx, tagsQuery, coordinatorDoc.getReshardingUUID());
    }
    ShardingCatalogManager::get(opCtx)->bumpCollectionPlacementVersionAndChangeMetadataInTxn(
        opCtx,
        updatedCoordinatorDoc.getSourceNss(),
        [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Remove entry for this resharding operation from config.reshardingOperations
            writeToCoordinatorStateNss(opCtx, metrics, updatedCoordinatorDoc, txnNumber);

            // Remove the resharding fields from the config.collections entry
            updateConfigCollectionsForOriginalNss(
                opCtx, updatedCoordinatorDoc, boost::none, boost::none, txnNumber);
        },
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

    metrics->onStateTransition(coordinatorDoc.getState(), updatedCoordinatorDoc.getState());
    return updatedCoordinatorDoc;
}

void writeToCoordinatorStateNss(OperationContext* opCtx,
                                ReshardingMetrics* metrics,
                                const ReshardingCoordinatorDocument& coordinatorDoc,
                                TxnNumber txnNumber) {
    Date_t timestamp = resharding::getCurrentTime();
    auto nextState = coordinatorDoc.getState();

    // We only need to check one donorShardEntry for 'documentsToCopy' because it will either be set
    // on all entries or on none of them.
    auto shouldUpdateDonorShardEntriesDocumentsToCopy =
        nextState == CoordinatorStateEnum::kCloning &&
        coordinatorDoc.getDonorShards().front().getDocumentsToCopy();

    // We only need to check one donorShardEntry for 'documentsFinal' because it will either be set
    // on all entries or on none of them.
    auto shouldUpdateDonorShardEntriesDocumentsFinal =
        nextState == CoordinatorStateEnum::kBlockingWrites &&
        coordinatorDoc.getDonorShards().front().getDocumentsFinal();

    BatchedCommandRequest request([&] {
        switch (nextState) {
            case CoordinatorStateEnum::kInitializing:
                // Insert the new coordinator document.
                return BatchedCommandRequest::buildInsertOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    std::vector<BSONObj>{coordinatorDoc.toBSON()});
            case CoordinatorStateEnum::kDone:
                // Remove the coordinator document.
                return BatchedCommandRequest::buildDeleteOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    BSON("_id" << coordinatorDoc.getReshardingUUID()),  // query
                    false                                               // multi
                );
            default: {
                // Partially update the coordinator document.
                BSONObjBuilder updateBuilder;
                {
                    BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                    // Always update the state field.
                    setBuilder.append(ReshardingCoordinatorDocument::kStateFieldName,
                                      CoordinatorState_serializer(coordinatorDoc.getState()));

                    if (auto cloneTimestamp = coordinatorDoc.getCloneTimestamp()) {
                        // If the cloneTimestamp exists, include it in the update.
                        setBuilder.append(ReshardingCoordinatorDocument::kCloneTimestampFieldName,
                                          *cloneTimestamp);
                    }

                    if (auto abortReason = coordinatorDoc.getAbortReason()) {
                        // If the abortReason exists, include it in the update.
                        setBuilder.append(ReshardingCoordinatorDocument::kAbortReasonFieldName,
                                          *abortReason);
                    }

                    if (auto approxBytesToCopy = coordinatorDoc.getApproxBytesToCopy()) {
                        // If the approxBytesToCopy exists, include it in the update.
                        setBuilder.append(
                            ReshardingCoordinatorDocument::kApproxBytesToCopyFieldName,
                            *approxBytesToCopy);
                    }

                    if (auto approxDocumentsToCopy = coordinatorDoc.getApproxDocumentsToCopy()) {
                        // If the approxDocumentsToCopy exists, include it in the update.
                        setBuilder.append(
                            ReshardingCoordinatorDocument::kApproxDocumentsToCopyFieldName,
                            *approxDocumentsToCopy);
                    }

                    if (auto quiescePeriodEnd = coordinatorDoc.getQuiescePeriodEnd()) {
                        // If the quiescePeriodEnd exists, include it in the update.
                        setBuilder.append(ReshardingCoordinatorDocument::kQuiescePeriodEndFieldName,
                                          *quiescePeriodEnd);
                    }

                    if (auto criticalSectionExpiresAt =
                            coordinatorDoc.getCriticalSectionExpiresAt()) {
                        // If the criticalSectionExpiresAt exists, include it in the update.
                        setBuilder.append(
                            ReshardingCoordinatorDocument::kCriticalSectionExpiresAtFieldName,
                            *criticalSectionExpiresAt);
                    }

                    buildStateDocumentMetricsForUpdate(setBuilder, nextState, timestamp);

                    if (nextState == CoordinatorStateEnum::kPreparingToDonate) {
                        appendShardEntriesToSetBuilder(coordinatorDoc, setBuilder);
                        setBuilder.doneFast();
                        unsetInitializingFields(updateBuilder);
                    }

                    if (shouldUpdateDonorShardEntriesDocumentsToCopy ||
                        shouldUpdateDonorShardEntriesDocumentsFinal) {
                        appendDonorShardEntriesToSetBuilder(coordinatorDoc, setBuilder);
                        setBuilder.doneFast();
                    }
                }

                return BatchedCommandRequest::buildUpdateOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    BSON("_id" << coordinatorDoc.getReshardingUUID()),
                    updateBuilder.obj(),
                    false,  // upsert
                    false   // multi
                );
            }
        }
    }());

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, NamespaceString::kConfigReshardingOperationsNamespace, request, txnNumber);

    assertResultIsValidForUpdatesAndDeletes(request, res);

    // We don't have metrics when moving from quiescing to done and when we are in the cloning state
    // or blocking state and are updating the donor shard entries.
    invariant(metrics || shouldUpdateDonorShardEntriesDocumentsToCopy ||
              shouldUpdateDonorShardEntriesDocumentsFinal ||
              (nextState == CoordinatorStateEnum::kDone));
    if (metrics) {
        setMeticsAfterWrite(metrics, nextState, timestamp);
    }
}

/**
 * Executes metadata changes in a transaction without bumping the collection placement version.
 */
void executeMetadataChangesInTxn(
    OperationContext* opCtx,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {
    ShardingCatalogManager::withTransaction(
        opCtx,
        NamespaceString::kConfigReshardingOperationsNamespace,
        std::move(changeMetadataFunc),
        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
}

void sendFlushReshardingStateChangeToShards(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const UUID& reshardingUUID,
                                            const std::vector<ShardId>& shardIds,
                                            const std::shared_ptr<executor::TaskExecutor>& executor,
                                            CancellationToken token) {
    auto opts = createFlushReshardingStateChangeOptions(nss, reshardingUUID, executor, token);
    generic_argument_util::setMajorityWriteConcern(opts->cmd, &resharding::kMajorityWriteConcern);
    opts->cmd.setDbName(DatabaseName::kAdmin);

    sendCommandToShards(opCtx, opts, {shardIds.begin(), shardIds.end()});
}

void sendFlushRoutingTableCacheUpdatesToShards(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token) {
    auto opts = makeFlushRoutingTableCacheUpdatesOptions(nss, executor, token);
    opts->cmd.setDbName(DatabaseName::kAdmin);
    generic_argument_util::setMajorityWriteConcern(opts->cmd, &resharding::kMajorityWriteConcern);

    sendCommandToShards(opCtx, opts, {shardIds.begin(), shardIds.end()});
}

ShardOwnership computeRecipientChunkOwnership(OperationContext* opCtx,
                                              const ReshardingCoordinatorDocument& coordinatorDoc) {
    const auto cm =
        uassertStatusOK(RoutingInformationCache::get(opCtx)->getCollectionPlacementInfoWithRefresh(
            opCtx, coordinatorDoc.getTempReshardingNss()));
    std::set<ShardId> shardsOwningChunks;
    cm.getAllShardIds(&shardsOwningChunks);

    auto recipientShardIds =
        resharding::extractShardIdsFromParticipantEntries(coordinatorDoc.getRecipientShards());

    std::set<ShardId> shardsNotOwningChunks;
    std::copy_if(recipientShardIds.begin(),
                 recipientShardIds.end(),
                 std::inserter(shardsNotOwningChunks, shardsNotOwningChunks.end()),
                 [&shardsOwningChunks](const ShardId& shard) {
                     return !shardsOwningChunks.contains(shard);
                 });

    invariant(recipientShardIds.size() == shardsOwningChunks.size() + shardsNotOwningChunks.size());

    return ShardOwnership{shardsOwningChunks, shardsNotOwningChunks};
}

void assertResultIsValidForUpdatesAndDeletes(const BatchedCommandRequest& request,
                                             const BSONObj& result) {
    if (request.getBatchType() == BatchedCommandRequest::BatchType_Delete) {
        auto numDocsMatched = result.getIntField("n");
        uassert(10541700,
                str::stream() << "Expected to match 0 or 1 docs, but matched " << numDocsMatched
                              << " for delete request " << request.toString(),
                0 == numDocsMatched || 1 == numDocsMatched);
    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Update) {
        assertNumDocsMatchedEqualsExpected(request, result, 1);
    }
}

void writeToConfigCollectionsForTempNss(OperationContext* opCtx,
                                        const ReshardingCoordinatorDocument& coordinatorDoc,
                                        boost::optional<ChunkVersion> chunkVersion,
                                        boost::optional<const BSONObj&> collation,
                                        boost::optional<bool> isUnsplittable,
                                        TxnNumber txnNumber) {
    auto request = generateBatchedCommandRequestForConfigCollectionsForTempNss(
        opCtx, coordinatorDoc, chunkVersion, collation, isUnsplittable);

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, request, txnNumber);

    assertResultIsValidForUpdatesAndDeletes(request, res);
}

BatchedCommandRequest generateBatchedCommandRequestForConfigCollectionsForTempNss(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<ChunkVersion> chunkVersion,
    boost::optional<const BSONObj&> collation,
    boost::optional<bool> isUnsplittable) {
    auto nextState = coordinatorDoc.getState();
    switch (nextState) {
        case CoordinatorStateEnum::kPreparingToDonate: {
            // Insert new entry for the temporary nss into config.collections
            auto collType = resharding::createTempReshardingCollectionType(
                opCtx, coordinatorDoc, chunkVersion.value(), collation.value(), isUnsplittable);
            return BatchedCommandRequest::buildInsertOp(CollectionType::ConfigNS,
                                                        std::vector<BSONObj>{collType.toBSON()});
        }
        case CoordinatorStateEnum::kCloning: {
            // Update the 'state', 'donorShards', 'approxCopySize', and 'cloneTimestamp' fields
            // in the 'reshardingFields.recipient' section

            BSONArrayBuilder donorShardsBuilder;
            for (const auto& donor : coordinatorDoc.getDonorShards()) {
                DonorShardFetchTimestamp donorShardFetchTimestamp(donor.getId());
                donorShardFetchTimestamp.setMinFetchTimestamp(
                    donor.getMutableState().getMinFetchTimestamp());
                donorShardsBuilder.append(donorShardFetchTimestamp.toBSON());
            }

            return BatchedCommandRequest::buildUpdateOp(
                CollectionType::ConfigNS,
                BSON(CollectionType::kNssFieldName
                     << NamespaceStringUtil::serialize(coordinatorDoc.getTempReshardingNss(),
                                                       SerializationContext::stateDefault())),
                BSON("$set" << BSON("reshardingFields.state"
                                    << CoordinatorState_serializer(nextState)
                                    << "reshardingFields.recipientFields.approxDocumentsToCopy"
                                    << coordinatorDoc.getApproxDocumentsToCopy().value()
                                    << "reshardingFields.recipientFields.approxBytesToCopy"
                                    << coordinatorDoc.getApproxBytesToCopy().value()
                                    << "reshardingFields.recipientFields.cloneTimestamp"
                                    << coordinatorDoc.getCloneTimestamp().value()
                                    << "reshardingFields.recipientFields.donorShards"
                                    << donorShardsBuilder.arr() << "lastmod"
                                    << opCtx->getServiceContext()->getPreciseClockSource()->now())),
                false,  // upsert
                false   // multi
            );
        }
        case CoordinatorStateEnum::kCommitting:
            // Remove the entry for the temporary nss
            return BatchedCommandRequest::buildDeleteOp(
                CollectionType::ConfigNS,
                BSON(CollectionType::kNssFieldName
                     << NamespaceStringUtil::serialize(coordinatorDoc.getTempReshardingNss(),
                                                       SerializationContext::stateDefault())),
                false  // multi
            );
        default: {
            // Update the 'state' field, and 'abortReason' field if it exists, in the
            // 'reshardingFields' section.
            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                setBuilder.append("reshardingFields.state",
                                  std::string{CoordinatorState_serializer(nextState)});
                setBuilder.append("lastmod",
                                  opCtx->getServiceContext()->getPreciseClockSource()->now());

                if (auto abortReason = coordinatorDoc.getAbortReason()) {
                    setBuilder.append("reshardingFields.abortReason", *abortReason);

                    auto abortStatus = resharding::getStatusFromAbortReason(coordinatorDoc);
                    setBuilder.append("reshardingFields.userCanceled",
                                      abortStatus == ErrorCodes::ReshardCollectionAborted);
                }
            }

            return BatchedCommandRequest::buildUpdateOp(
                CollectionType::ConfigNS,
                BSON(CollectionType::kNssFieldName
                     << NamespaceStringUtil::serialize(coordinatorDoc.getTempReshardingNss(),
                                                       SerializationContext::stateDefault())),
                updateBuilder.obj(),
                true,  // upsert
                false  // multi
            );
        }
    }
}

}  // namespace resharding
}  // namespace mongo
