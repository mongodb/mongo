/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_coordinator_service.h"

#include <absl/container/node_hash_map.h>
#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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
#include "mongo/db/commands/notify_sharding_event_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/balancer/balance_stats.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_commit_monitor.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics_helpers.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/async_rpc_util.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection_gen.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/abort_reshard_collection_gen.h"
#include "mongo/s/request_types/commit_reshard_collection_gen.h"
#include "mongo/s/request_types/drop_collection_if_uuid_not_matching_gen.h"
#include "mongo/s/request_types/flush_resharding_state_change_gen.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"
#include "mongo/s/resharding/resharding_coordinator_service_conflicting_op_in_progress_info.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_index_catalog_cache.h"
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorAfterPreparingToDonate);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeInitializing);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeCloning);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeBlockingWrites);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeDecisionPersisted);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeRemovingStateDoc);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeCompletion);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeStartingErrorFlow);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforePersistingStateTransition);
MONGO_FAIL_POINT_DEFINE(pauseBeforeTellDonorToRefresh);
MONGO_FAIL_POINT_DEFINE(pauseAfterInsertCoordinatorDoc);
MONGO_FAIL_POINT_DEFINE(pauseBeforeCTHolderInitialization);

const std::string kReshardingCoordinatorActiveIndexName = "ReshardingCoordinatorActiveIndex";
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());
const WriteConcernOptions kMajorityWriteConcern{
    WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};

bool shouldStopAttemptingToCreateIndex(Status status, const CancellationToken& token) {
    return status.isOK() || token.isCanceled();
}

Date_t getCurrentTime() {
    const auto svcCtx = cc().getServiceContext();
    return svcCtx->getFastClockSource()->now();
}

void assertNumDocsMatchedEqualsExpected(const BatchedCommandRequest& request,
                                        const BSONObj& response,
                                        int expected) {
    auto numDocsMatched = response.getIntField("n");
    uassert(5030401,
            str::stream() << "Expected to match " << expected << " docs, but only matched "
                          << numDocsMatched << " for write request " << request.toString(),
            expected == numDocsMatched);
}

void appendShardEntriesToSetBuilder(const ReshardingCoordinatorDocument& coordinatorDoc,
                                    BSONObjBuilder& setBuilder) {
    BSONArrayBuilder donorShards(
        setBuilder.subarrayStart(ReshardingCoordinatorDocument::kDonorShardsFieldName));
    for (const auto& donorShard : coordinatorDoc.getDonorShards()) {
        donorShards.append(donorShard.toBSON());
    }
    donorShards.doneFast();

    BSONArrayBuilder recipientShards(
        setBuilder.subarrayStart(ReshardingCoordinatorDocument::kRecipientShardsFieldName));
    for (const auto& recipientShard : coordinatorDoc.getRecipientShards()) {
        recipientShards.append(recipientShard.toBSON());
    }
    recipientShards.doneFast();
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
        getIntervalEndFieldName<DocT>(ReshardingRecipientMetrics::kOplogApplicationFieldName),
        timestamp);
}

void buildStateDocumentBlockingWritesMetricsForUpdate(BSONObjBuilder& bob, Date_t timestamp) {
    bob.append(
        getIntervalEndFieldName<DocT>(ReshardingRecipientMetrics::kOplogApplicationFieldName),
        timestamp);
}

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

void writeToCoordinatorStateNss(OperationContext* opCtx,
                                ReshardingMetrics* metrics,
                                const ReshardingCoordinatorDocument& coordinatorDoc,
                                TxnNumber txnNumber) {
    Date_t timestamp = getCurrentTime();
    auto nextState = coordinatorDoc.getState();
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

                    buildStateDocumentMetricsForUpdate(setBuilder, nextState, timestamp);

                    if (nextState == CoordinatorStateEnum::kPreparingToDonate) {
                        appendShardEntriesToSetBuilder(coordinatorDoc, setBuilder);
                        setBuilder.doneFast();
                        unsetInitializingFields(updateBuilder);
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

    auto expectedNumMatched = (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
        ? boost::none
        : boost::make_optional(1);

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, NamespaceString::kConfigReshardingOperationsNamespace, request, txnNumber);

    if (expectedNumMatched) {
        assertNumDocsMatchedEqualsExpected(request, res, *expectedNumMatched);
    }

    // When moving from quiescing to done, we don't have metrics available.
    invariant(metrics || nextState == CoordinatorStateEnum::kDone);
    if (metrics) {
        setMeticsAfterWrite(metrics, nextState, timestamp);
    }
}

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
        resharding::gReshardingMinimumOperationDurationMillis.load());

    resharding::emplaceCloneTimestampIfExists(recipientFields, coordinatorDoc.getCloneTimestamp());
    resharding::emplaceApproxBytesToCopyIfExists(
        recipientFields, coordinatorDoc.getReshardingApproxCopySizeStruct());

    return recipientFields;
}

BSONObj createReshardingFieldsUpdateForOriginalNss(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<OID> newCollectionEpoch,
    boost::optional<Timestamp> newCollectionTimestamp,
    boost::optional<CollectionIndexes> newCollectionIndexVersion) {
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
            auto recipientFields = constructRecipientFields(coordinatorDoc);
            BSONObj setFields =
                BSON("uuid" << coordinatorDoc.getReshardingUUID() << "key"
                            << coordinatorDoc.getReshardingKey().toBSON() << "lastmodEpoch"
                            << newCollectionEpoch.value() << "lastmod"
                            << opCtx->getServiceContext()->getPreciseClockSource()->now()
                            << "reshardingFields.state"
                            << CoordinatorState_serializer(coordinatorDoc.getState()).toString()
                            << "reshardingFields.recipientFields" << recipientFields.toBSON());
            if (newCollectionTimestamp.has_value()) {
                setFields =
                    setFields.addFields(BSON("timestamp" << newCollectionTimestamp.value()));
            }
            if (newCollectionIndexVersion.has_value()) {
                setFields = setFields.addFields(
                    BSON("indexVersion" << newCollectionIndexVersion->indexVersion()));
            }

            auto provenance = coordinatorDoc.getCommonReshardingMetadata().getProvenance();
            if (provenance && provenance.get() == ProvenanceEnum::kUnshardCollection) {
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
                                  CoordinatorState_serializer(nextState).toString());
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
                                           boost::optional<CollectionIndexes> newCollectionIndexes,
                                           TxnNumber txnNumber) {
    auto writeOp = createReshardingFieldsUpdateForOriginalNss(
        opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp, newCollectionIndexes);

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

void writeToConfigCollectionsForTempNss(OperationContext* opCtx,
                                        const ReshardingCoordinatorDocument& coordinatorDoc,
                                        boost::optional<ChunkVersion> chunkVersion,
                                        boost::optional<const BSONObj&> collation,
                                        boost::optional<CollectionIndexes> indexVersion,
                                        boost::optional<bool> isUnsplittable,
                                        TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        auto nextState = coordinatorDoc.getState();
        switch (nextState) {
            case CoordinatorStateEnum::kPreparingToDonate: {
                // Insert new entry for the temporary nss into config.collections
                auto collType = resharding::createTempReshardingCollectionType(opCtx,
                                                                               coordinatorDoc,
                                                                               chunkVersion.value(),
                                                                               collation.value(),
                                                                               indexVersion,
                                                                               isUnsplittable);
                return BatchedCommandRequest::buildInsertOp(
                    CollectionType::ConfigNS, std::vector<BSONObj>{collType.toBSON()});
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
                    BSON("$set" << BSON(
                             "reshardingFields.state"
                             << CoordinatorState_serializer(nextState).toString()
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
                                      CoordinatorState_serializer(nextState).toString());
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
    }());

    auto expectedNumMatched = (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
        ? boost::none
        : boost::make_optional(1);

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, request, txnNumber);

    if (expectedNumMatched) {
        assertNumDocsMatchedEqualsExpected(request, res, *expectedNumMatched);
    }
}

void writeToConfigIndexesForTempNss(OperationContext* opCtx,
                                    const ReshardingCoordinatorDocument& coordinatorDoc,
                                    TxnNumber txnNumber) {
    if (!feature_flags::gGlobalIndexesShardingCatalog.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return;
    }
    auto nextState = coordinatorDoc.getState();

    switch (nextState) {
        case CoordinatorStateEnum::kPreparingToDonate: {
            auto [_, optSii] =
                uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
                    opCtx, coordinatorDoc.getSourceNss()));
            if (optSii) {
                std::vector<BSONObj> indexes;
                optSii->forEachIndex([&](const auto index) {
                    IndexCatalogType copyIdx(index);
                    copyIdx.setCollectionUUID(coordinatorDoc.getReshardingUUID());
                    // TODO SERVER-73304: add the new index collection UUID here if neccessary.
                    indexes.push_back(copyIdx.toBSON());
                    return true;
                });

                BatchedCommandRequest request([&] {
                    return BatchedCommandRequest::buildInsertOp(
                        NamespaceString::kConfigsvrIndexCatalogNamespace, indexes);
                }());
                ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
                    opCtx, NamespaceString::kConfigsvrIndexCatalogNamespace, request, txnNumber);
            }
        } break;
        case CoordinatorStateEnum::kCommitting: {
            BatchedCommandRequest request([&] {
                return BatchedCommandRequest::buildDeleteOp(
                    NamespaceString::kConfigsvrIndexCatalogNamespace,
                    BSON(IndexCatalogType::kCollectionUUIDFieldName
                         << coordinatorDoc.getSourceUUID()),
                    true  // multi
                );
            }());
            ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
                opCtx, NamespaceString::kConfigsvrIndexCatalogNamespace, request, txnNumber);
        } break;
        case CoordinatorStateEnum::kAborting: {
            BatchedCommandRequest request([&] {
                return BatchedCommandRequest::buildDeleteOp(
                    NamespaceString::kConfigsvrIndexCatalogNamespace,
                    BSON(IndexCatalogType::kCollectionUUIDFieldName
                         << coordinatorDoc.getReshardingUUID()),
                    true  // multi
                );
            }());
            ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
                opCtx, NamespaceString::kConfigsvrIndexCatalogNamespace, request, txnNumber);
        } break;
        default:
            break;
    }
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

void insertChunkAndTagDocsForTempNss(OperationContext* opCtx,
                                     const std::vector<ChunkType>& initialChunks,
                                     std::vector<BSONObj> newZones) {
    // Insert new initial chunk documents for temp nss
    std::vector<BSONObj> initialChunksBSON(initialChunks.size());
    std::transform(initialChunks.begin(),
                   initialChunks.end(),
                   initialChunksBSON.begin(),
                   [](ChunkType chunk) { return chunk.toConfigBSON(); });

    ShardingCatalogManager::get(opCtx)->insertConfigDocuments(
        opCtx, ChunkType::ConfigNS, std::move(initialChunksBSON));


    ShardingCatalogManager::get(opCtx)->insertConfigDocuments(opCtx, TagsType::ConfigNS, newZones);
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
    uassertStatusOK(catalogClient->removeConfigDocuments(
        opCtx, TagsType::ConfigNS, tagsQuery, kMajorityWriteConcern, tagDeleteOperationHint));
}

/**
 * Executes metadata changes in a transaction without bumping the collection placement version.
 */
void executeMetadataChangesInTxn(
    OperationContext* opCtx,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {
    ShardingCatalogManager::withTransaction(opCtx,
                                            NamespaceString::kConfigReshardingOperationsNamespace,
                                            std::move(changeMetadataFunc),
                                            ShardingCatalogClient::kLocalWriteConcern);
}

std::shared_ptr<async_rpc::AsyncRPCOptions<FlushRoutingTableCacheUpdatesWithWriteConcern>>
makeFlushRoutingTableCacheUpdatesOptions(const NamespaceString& nss,
                                         const std::shared_ptr<executor::TaskExecutor>& exec,
                                         CancellationToken token,
                                         async_rpc::GenericArgs args) {
    auto cmd = FlushRoutingTableCacheUpdatesWithWriteConcern(nss);
    cmd.setSyncFromConfig(true);
    cmd.setDbName(nss.dbName());
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
    auto opts =
        std::make_shared<async_rpc::AsyncRPCOptions<FlushRoutingTableCacheUpdatesWithWriteConcern>>(
            exec, token, cmd, args);
    return opts;
}

}  // namespace

namespace resharding {
CollectionType createTempReshardingCollectionType(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    const ChunkVersion& chunkVersion,
    const BSONObj& collation,
    boost::optional<CollectionIndexes> indexVersion,
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

    auto recipientFields = constructRecipientFields(coordinatorDoc);
    tempEntryReshardingFields.setRecipientFields(std::move(recipientFields));
    collType.setReshardingFields(std::move(tempEntryReshardingFields));
    collType.setAllowMigrations(false);

    if (indexVersion) {
        collType.setIndexVersion(*indexVersion);
    }
    return collType;
}

void removeChunkDocs(OperationContext* opCtx, const UUID& collUUID) {
    // Remove all chunk documents for the specified collUUID. We do not know how many chunk docs
    // currently exist, so cannot pass a value for expectedNumModified
    const auto chunksQuery = BSON(ChunkType::collectionUUID() << collUUID);
    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();

    uassertStatusOK(catalogClient->removeConfigDocuments(
        opCtx, ChunkType::ConfigNS, chunksQuery, kMajorityWriteConcern));
}

void writeDecisionPersistedState(OperationContext* opCtx,
                                 ReshardingMetrics* metrics,
                                 const ReshardingCoordinatorDocument& coordinatorDoc,
                                 OID newCollectionEpoch,
                                 Timestamp newCollectionTimestamp,
                                 boost::optional<CollectionIndexes> collectionIndexes,
                                 const std::vector<ShardId>& reshardedCollectionPlacement) {

    // No need to bump originalNss version because its epoch will be changed.
    executeMetadataChangesInTxn(
        opCtx,
        [&metrics,
         &coordinatorDoc,
         &newCollectionEpoch,
         &newCollectionTimestamp,
         &collectionIndexes,
         &reshardedCollectionPlacement](OperationContext* opCtx, TxnNumber txnNumber) {
            // Update the config.reshardingOperations entry
            writeToCoordinatorStateNss(opCtx, metrics, coordinatorDoc, txnNumber);

            // Copy the original indexes to the temporary uuid.
            writeToConfigIndexesForTempNss(opCtx, coordinatorDoc, txnNumber);

            // Remove the config.collections entry for the temporary collection
            writeToConfigCollectionsForTempNss(opCtx,
                                               coordinatorDoc,
                                               boost::none,
                                               boost::none,
                                               boost::none,
                                               boost::none,
                                               txnNumber);

            // Update the config.collections entry for the original namespace to reflect the new
            // shard key, new epoch, and new UUID
            updateConfigCollectionsForOriginalNss(opCtx,
                                                  coordinatorDoc,
                                                  newCollectionEpoch,
                                                  newCollectionTimestamp,
                                                  collectionIndexes,
                                                  txnNumber);

            // Insert the list of recipient shard IDs (together with the new timestamp and UUID) as
            // the latest entry in config.placementHistory about the original namespace
            writeToConfigPlacementHistoryForOriginalNss(opCtx,
                                                        coordinatorDoc,
                                                        newCollectionTimestamp,
                                                        reshardedCollectionPlacement,
                                                        txnNumber);

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
            invariant(coordinatorDoc.getActive());
            writeToCoordinatorStateNss(opCtx, metrics, coordinatorDoc, txnNumber);

            // Update the config.collections entry for the original collection to include
            // 'reshardingFields'
            updateConfigCollectionsForOriginalNss(
                opCtx, coordinatorDoc, boost::none, boost::none, boost::none, txnNumber);
        },
        ShardingCatalogClient::kLocalWriteConcern);
}

void writeParticipantShardsAndTempCollInfo(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    const ReshardingCoordinatorDocument& updatedCoordinatorDoc,
    std::vector<ChunkType> initialChunks,
    std::vector<BSONObj> zones,
    boost::optional<CollectionIndexes> indexVersion,
    boost::optional<bool> isUnsplittable) {
    const auto tagsQuery = BSON(TagsType::ns(NamespaceStringUtil::serialize(
        updatedCoordinatorDoc.getTempReshardingNss(), SerializationContext::stateDefault())));

    removeChunkAndTagsDocs(opCtx, tagsQuery, updatedCoordinatorDoc.getReshardingUUID());
    insertChunkAndTagDocsForTempNss(opCtx, initialChunks, zones);

    ShardingCatalogManager::get(opCtx)->bumpCollectionPlacementVersionAndChangeMetadataInTxn(
        opCtx,
        updatedCoordinatorDoc.getSourceNss(),
        [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Insert the config.collections entry for the temporary resharding collection. The
            // chunks all have the same epoch, so picking the last chunk here is arbitrary.
            invariant(initialChunks.size() != 0);
            auto chunkVersion = initialChunks.back().getVersion();
            writeToConfigCollectionsForTempNss(opCtx,
                                               updatedCoordinatorDoc,
                                               chunkVersion,
                                               CollationSpec::kSimpleSpec,
                                               indexVersion,
                                               isUnsplittable,
                                               txnNumber);
            // Copy the original indexes to the temporary uuid.
            writeToConfigIndexesForTempNss(opCtx, updatedCoordinatorDoc, txnNumber);
            // Update on-disk state to reflect latest state transition.
            writeToCoordinatorStateNss(opCtx, metrics, updatedCoordinatorDoc, txnNumber);
            updateConfigCollectionsForOriginalNss(
                opCtx, updatedCoordinatorDoc, boost::none, boost::none, boost::none, txnNumber);
        },
        ShardingCatalogClient::kLocalWriteConcern);
}

void writeStateTransitionAndCatalogUpdatesThenBumpCollectionPlacementVersions(
    OperationContext* opCtx,
    ReshardingMetrics* metrics,
    const ReshardingCoordinatorDocument& coordinatorDoc) {
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
                writeToCoordinatorStateNss(opCtx, metrics, coordinatorDoc, txnNumber);

                // Update the config.collections entry for the original collection
                updateConfigCollectionsForOriginalNss(
                    opCtx, coordinatorDoc, boost::none, boost::none, boost::none, txnNumber);

                // Update the config.collections entry for the temporary resharding collection. If
                // we've already successfully committed that the operation will succeed, we've
                // removed the entry for the temporary collection and updated the entry with
                // original namespace to have the new shard key, UUID, and epoch
                if (nextState < CoordinatorStateEnum::kCommitting) {
                    writeToConfigCollectionsForTempNss(opCtx,
                                                       coordinatorDoc,
                                                       boost::none,
                                                       boost::none,
                                                       boost::none,
                                                       boost::none,
                                                       txnNumber);

                    // Copy the original indexes to the temporary uuid.
                    writeToConfigIndexesForTempNss(opCtx, coordinatorDoc, txnNumber);
                }
            },
            ShardingCatalogClient::kLocalWriteConcern);
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
            opCtx->getServiceContext()->getFastClockSource()->now() +
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
            kMajorityWriteConcern));

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
                opCtx, updatedCoordinatorDoc, boost::none, boost::none, boost::none, txnNumber);
        },
        ShardingCatalogClient::kLocalWriteConcern);

    metrics->onStateTransition(coordinatorDoc.getState(), updatedCoordinatorDoc.getState());
    return updatedCoordinatorDoc;
}
}  // namespace resharding

ChunkVersion ReshardingCoordinatorExternalState::calculateChunkVersionForInitialChunks(
    OperationContext* opCtx) {
    const auto now = VectorClock::get(opCtx)->getTime();
    const auto timestamp = now.clusterTime().asTimestamp();
    return ChunkVersion({OID::gen(), timestamp}, {1, 0});
}

boost::optional<CollectionIndexes> ReshardingCoordinatorExternalState::getCatalogIndexVersion(
    OperationContext* opCtx, const NamespaceString& nss, const UUID& uuid) {
    auto [_, optSii] =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    if (optSii) {
        VectorClock::VectorTime vt = VectorClock::get(opCtx)->getTime();
        auto time = vt.clusterTime().asTimestamp();
        return CollectionIndexes{uuid, time};
    }
    return boost::none;
}

bool ReshardingCoordinatorExternalState::getIsUnsplittable(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    auto [cm, _] =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    return cm.isUnsplittable();
}

boost::optional<CollectionIndexes>
ReshardingCoordinatorExternalState::getCatalogIndexVersionForCommit(OperationContext* opCtx,
                                                                    const NamespaceString& nss) {
    auto [_, optSii] =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
    if (optSii) {
        return optSii->getCollectionIndexes();
    }
    return boost::none;
}

std::vector<DonorShardEntry> constructDonorShardEntries(const std::set<ShardId>& donorShardIds) {
    std::vector<DonorShardEntry> donorShards;
    std::transform(donorShardIds.begin(),
                   donorShardIds.end(),
                   std::back_inserter(donorShards),
                   [](const ShardId& shardId) -> DonorShardEntry {
                       DonorShardContext donorCtx;
                       donorCtx.setState(DonorStateEnum::kUnused);
                       return DonorShardEntry{shardId, std::move(donorCtx)};
                   });
    return donorShards;
}

std::vector<RecipientShardEntry> constructRecipientShardEntries(
    const std::set<ShardId>& recipientShardIds) {
    std::vector<RecipientShardEntry> recipientShards;
    std::transform(recipientShardIds.begin(),
                   recipientShardIds.end(),
                   std::back_inserter(recipientShards),
                   [](const ShardId& shardId) -> RecipientShardEntry {
                       RecipientShardContext recipientCtx;
                       recipientCtx.setState(RecipientStateEnum::kUnused);
                       return RecipientShardEntry{shardId, std::move(recipientCtx)};
                   });
    return recipientShards;
}

ReshardingCoordinatorExternalState::ParticipantShardsAndChunks
ReshardingCoordinatorExternalStateImpl::calculateParticipantShardsAndChunks(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {

    const auto [cm, _] = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getTrackedCollectionRoutingInfoWithPlacementRefresh(
            opCtx, coordinatorDoc.getSourceNss()));

    std::set<ShardId> donorShardIds;
    cm.getAllShardIds(&donorShardIds);

    std::set<ShardId> recipientShardIds;
    std::vector<ChunkType> initialChunks;

    // The database primary must always be a recipient to ensure it ends up with consistent
    // collection metadata.
    const auto dbPrimaryShard =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(
                            opCtx, coordinatorDoc.getSourceNss().dbName()))
            ->getPrimary();

    recipientShardIds.emplace(dbPrimaryShard);

    if (const auto& chunks = coordinatorDoc.getPresetReshardedChunks()) {
        auto version = calculateChunkVersionForInitialChunks(opCtx);

        // Use the provided shardIds from presetReshardedChunks to construct the
        // recipient list.
        for (const auto& reshardedChunk : *chunks) {
            recipientShardIds.emplace(reshardedChunk.getRecipientShardId());

            initialChunks.emplace_back(coordinatorDoc.getReshardingUUID(),
                                       ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                                       version,
                                       reshardedChunk.getRecipientShardId());
            version.incMinor();
        }
    } else {
        int numInitialChunks = coordinatorDoc.getNumInitialChunks()
            ? *coordinatorDoc.getNumInitialChunks()
            : cm.numChunks();

        ShardKeyPattern shardKey(coordinatorDoc.getReshardingKey());
        const auto tempNs = coordinatorDoc.getTempReshardingNss();

        boost::optional<std::vector<mongo::TagsType>> parsedZones;
        auto rawBSONZones = coordinatorDoc.getZones();
        if (rawBSONZones && rawBSONZones->size() != 0) {
            parsedZones.emplace();
            parsedZones->reserve(rawBSONZones->size());

            for (const auto& zone : *rawBSONZones) {
                ChunkRange range(zone.getMin(), zone.getMax());
                TagsType tag(
                    coordinatorDoc.getTempReshardingNss(), zone.getZone().toString(), range);

                parsedZones->push_back(tag);
            }
        }

        InitialSplitPolicy::ShardCollectionConfig splitResult;

        // If shardDistribution is specified with min/max, use ShardDistributionSplitPolicy.
        if (const auto& shardDistribution = coordinatorDoc.getShardDistribution()) {
            uassert(ErrorCodes::InvalidOptions,
                    "Resharding improvements is not enabled, should not have "
                    "shardDistribution in coordinatorDoc",
                    resharding::gFeatureFlagReshardingImprovements.isEnabled(
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
            uassert(ErrorCodes::InvalidOptions,
                    "ShardDistribution should not be empty if provided",
                    shardDistribution->size() > 0);
            const SplitPolicyParams splitParams{coordinatorDoc.getReshardingUUID(),
                                                *donorShardIds.begin()};
            // If shardDistribution is specified with min/max, create chunks based on the shard
            // min/max. If not, do sampling based split on limited shards.
            if ((*shardDistribution)[0].getMin()) {
                auto initialSplitter = ShardDistributionSplitPolicy::make(
                    opCtx, shardKey, *shardDistribution, std::move(parsedZones));
                splitResult = initialSplitter.createFirstChunks(opCtx, shardKey, splitParams);
            } else {
                std::vector<ShardId> availableShardIds;
                for (const auto& shardDist : *shardDistribution) {
                    availableShardIds.emplace_back(shardDist.getShard());
                }
                auto initialSplitter = SamplingBasedSplitPolicy::make(opCtx,
                                                                      coordinatorDoc.getSourceNss(),
                                                                      shardKey,
                                                                      numInitialChunks,
                                                                      std::move(parsedZones),
                                                                      availableShardIds);
                splitResult = initialSplitter.createFirstChunks(opCtx, shardKey, splitParams);
            }
        } else {
            auto initialSplitter =
                SamplingBasedSplitPolicy::make(opCtx,
                                               coordinatorDoc.getSourceNss(),
                                               shardKey,
                                               numInitialChunks,
                                               std::move(parsedZones),
                                               boost::none /*availableShardIds*/);
            // Note: The resharding initial split policy doesn't care about what is the real
            // primary shard, so just pass in a random shard.
            const SplitPolicyParams splitParams{coordinatorDoc.getReshardingUUID(),
                                                *donorShardIds.begin()};
            splitResult = initialSplitter.createFirstChunks(opCtx, shardKey, splitParams);
        }

        initialChunks = std::move(splitResult.chunks);

        for (const auto& chunk : initialChunks) {
            recipientShardIds.insert(chunk.getShard());
        }
    }

    return {constructDonorShardEntries(donorShardIds),
            constructRecipientShardEntries(recipientShardIds),
            initialChunks};
}

template <typename CommandType>
void ReshardingCoordinatorExternalState::sendCommandToShards(
    OperationContext* opCtx,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> opts,
    const std::vector<ShardId>& shardIds) {
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
}

ThreadPool::Limits ReshardingCoordinatorService::getThreadPoolLimits() const {
    ThreadPool::Limits threadPoolLimit;
    threadPoolLimit.maxThreads = resharding::gReshardingCoordinatorServiceMaxThreadCount;
    return threadPoolLimit;
}

void ReshardingCoordinatorService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) {
    auto coordinatorDoc = ReshardingCoordinatorDocument::parse(
        IDLParserContext("ReshardingCoordinatorService::checkIfConflictsWithOtherInstances"),
        initialState);

    for (const auto& instance : existingInstances) {
        auto typedInstance = checked_cast<const ReshardingCoordinator*>(instance);
        // Instances which have already completed do not conflict with other instances, unless
        // their user resharding UUIDs are the same.
        const bool isUserReshardingUUIDSame =
            typedInstance->getMetadata().getUserReshardingUUID() ==
            coordinatorDoc.getUserReshardingUUID();
        if (!isUserReshardingUUIDSame && typedInstance->getCompletionFuture().isReady()) {
            LOGV2_DEBUG(7760400,
                        1,
                        "Ignoring 'conflict' with completed instance of resharding",
                        "newNss"_attr = coordinatorDoc.getSourceNss(),
                        "oldNss"_attr = typedInstance->getMetadata().getSourceNss(),
                        "newUUID"_attr = coordinatorDoc.getReshardingUUID(),
                        "oldUUID"_attr = typedInstance->getMetadata().getReshardingUUID());
            continue;
        }
        // For resharding commands with no UUID provided by the user, we will re-connect to an
        // instance with the same NS and resharding key, if that instance was originally started
        // with no user-provided UUID. If a UUID is provided by the user, we will connect only
        // to the original instance.
        const bool isNssSame =
            typedInstance->getMetadata().getSourceNss() == coordinatorDoc.getSourceNss();
        const bool isReshardingKeySame = SimpleBSONObjComparator::kInstance.evaluate(
            typedInstance->getMetadata().getReshardingKey().toBSON() ==
            coordinatorDoc.getReshardingKey().toBSON());

        const bool isProvenanceSame =
            (typedInstance->getMetadata().getProvenance() ==
             coordinatorDoc.getCommonReshardingMetadata().getProvenance());

        iassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Only one resharding operation is allowed to be active at a "
                                 "time, aborting resharding op for "
                              << coordinatorDoc.getSourceNss().toStringForErrorMsg(),
                isUserReshardingUUIDSame && isNssSame && isReshardingKeySame && isProvenanceSame);

        std::string userReshardingIdMsg;
        if (coordinatorDoc.getUserReshardingUUID()) {
            userReshardingIdMsg = str::stream()
                << " and user resharding UUID " << coordinatorDoc.getUserReshardingUUID();
        }

        iasserted(ReshardingCoordinatorServiceConflictingOperationInProgressInfo(
                      typedInstance->shared_from_this()),
                  str::stream() << "Found an active resharding operation for "
                                << coordinatorDoc.getSourceNss().toStringForErrorMsg()
                                << " with resharding key "
                                << coordinatorDoc.getReshardingKey().toString()
                                << userReshardingIdMsg);
    }
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingCoordinatorService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<ReshardingCoordinator>(
        this,
        ReshardingCoordinatorDocument::parse(IDLParserContext("ReshardingCoordinatorStateDoc"),
                                             initialState),
        std::make_shared<ReshardingCoordinatorExternalStateImpl>(),
        _serviceContext);
}

ExecutorFuture<void> ReshardingCoordinatorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {

    return AsyncTry([this] {
               auto nss = getStateDocumentsNS();

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);
               BSONObj result;
               // We don't need a unique index on "active" any more since
               // checkIfConflictsWithOtherInstances was implemented, and once we allow quiesced
               // instances it breaks them, so don't create it.
               //
               // TODO(SERVER-67712): We create the collection only to make index creation during
               // downgrade simpler, so we can remove all of this initialization when the flag is
               // removed.
               if (!resharding::gFeatureFlagReshardingImprovements.isEnabled(
                       serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                   client.runCommand(
                       nss.dbName(),
                       BSON("createIndexes"
                            << nss.coll().toString() << "indexes"
                            << BSON_ARRAY(BSON("key" << BSON("active" << 1) << "name"
                                                     << kReshardingCoordinatorActiveIndexName
                                                     << "unique" << true))),
                       result);
                   uassertStatusOK(getStatusFromCommandResult(result));
               } else {
                   client.runCommand(nss.dbName(), BSON("create" << nss.coll().toString()), result);
                   const auto& status = getStatusFromCommandResult(result);
                   if (status.code() != ErrorCodes::NamespaceExists)
                       uassertStatusOK(status);
               }
           })
        .until([token](Status status) { return shouldStopAttemptingToCreateIndex(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

void ReshardingCoordinatorService::abortAllReshardCollection(OperationContext* opCtx) {
    std::vector<SharedSemiFuture<void>> reshardingCoordinatorFutures;

    for (auto& instance : getAllInstances(opCtx)) {
        auto reshardingCoordinator = checked_pointer_cast<ReshardingCoordinator>(instance);
        reshardingCoordinatorFutures.push_back(
            reshardingCoordinator->getQuiescePeriodFinishedFuture());
        reshardingCoordinator->abort(true /* skip quiesce period */);
    }

    for (auto&& future : reshardingCoordinatorFutures) {
        future.wait(opCtx);
    }
}

ReshardingCoordinator::ReshardingCoordinator(
    ReshardingCoordinatorService* coordinatorService,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    std::shared_ptr<ReshardingCoordinatorExternalState> externalState,
    ServiceContext* serviceContext)
    : repl::PrimaryOnlyService::TypedInstance<ReshardingCoordinator>(),
      _id(BSON("_id" << coordinatorDoc.getReshardingUUID())),
      _coordinatorService(coordinatorService),
      _serviceContext(serviceContext),
      _metrics{ReshardingMetrics::initializeFrom(coordinatorDoc, _serviceContext)},
      _metadata(coordinatorDoc.getCommonReshardingMetadata()),
      _coordinatorDoc(coordinatorDoc),
      _markKilledExecutor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "ReshardingCoordinatorCancelableOpCtxPool";
          options.minThreads = 1;
          options.maxThreads = 1;
          return options;
      }())),
      _reshardingCoordinatorExternalState(externalState) {
    _reshardingCoordinatorObserver = std::make_shared<ReshardingCoordinatorObserver>();

    // If the coordinator is recovering from step-up, make sure to properly initialize the
    // promises to reflect the latest state of this resharding operation.
    if (coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing) {
        _reshardingCoordinatorObserver->onReshardingParticipantTransition(coordinatorDoc);
    }

    _metrics->onStateTransition(boost::none, coordinatorDoc.getState());
}

void ReshardingCoordinator::installCoordinatorDoc(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& doc) noexcept {
    invariant(doc.getReshardingUUID() == _coordinatorDoc.getReshardingUUID());

    BSONObjBuilder bob;
    bob.append("newState", CoordinatorState_serializer(doc.getState()));
    bob.append("oldState", CoordinatorState_serializer(_coordinatorDoc.getState()));
    bob.append(
        "namespace",
        NamespaceStringUtil::serialize(doc.getSourceNss(), SerializationContext::stateDefault()));
    bob.append("collectionUUID", doc.getSourceUUID().toString());
    bob.append("reshardingUUID", doc.getReshardingUUID().toString());

    LOGV2_INFO(5343001,
               "Transitioned resharding coordinator state",
               "newState"_attr = CoordinatorState_serializer(doc.getState()),
               "oldState"_attr = CoordinatorState_serializer(_coordinatorDoc.getState()),
               logAttrs(doc.getSourceNss()),
               "collectionUUID"_attr = doc.getSourceUUID(),
               "reshardingUUID"_attr = doc.getReshardingUUID());

    const auto previousState = _coordinatorDoc.getState();
    _coordinatorDoc = doc;

    _metrics->onStateTransition(previousState, _coordinatorDoc.getState());

    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           "resharding.coordinator.transition",
                                           doc.getSourceNss(),
                                           bob.obj(),
                                           kMajorityWriteConcern);
}

void markCompleted(const Status& status, ReshardingMetrics* metrics) {
    if (status.isOK()) {
        metrics->onSuccess();
    } else if (status == ErrorCodes::ReshardCollectionAborted) {
        metrics->onCanceled();
    } else {
        metrics->onFailure();
    }
}

std::shared_ptr<async_rpc::AsyncRPCOptions<_flushReshardingStateChange>>
createFlushReshardingStateChangeOptions(const NamespaceString& nss,
                                        const UUID& reshardingUUID,
                                        const std::shared_ptr<executor::TaskExecutor>& exec,
                                        CancellationToken token,
                                        async_rpc::GenericArgs args) {
    _flushReshardingStateChange cmd(nss);
    cmd.setDbName(DatabaseName::kAdmin);
    cmd.setReshardingUUID(reshardingUUID);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<_flushReshardingStateChange>>(
        exec, token, cmd, args);
    return opts;
}

std::shared_ptr<async_rpc::AsyncRPCOptions<ShardsvrCommitReshardCollection>>
createShardsvrCommitReshardCollectionOptions(const NamespaceString& nss,
                                             const UUID& reshardingUUID,
                                             const std::shared_ptr<executor::TaskExecutor>& exec,
                                             CancellationToken token,
                                             async_rpc::GenericArgs args) {
    ShardsvrCommitReshardCollection cmd(nss);
    cmd.setDbName(DatabaseName::kAdmin);
    cmd.setReshardingUUID(reshardingUUID);
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrCommitReshardCollection>>(
        exec, token, cmd, args);
    return opts;
}

ExecutorFuture<void> ReshardingCoordinator::_tellAllParticipantsReshardingStarted(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kPreparingToDonate) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this] {
                       // Ensure the flushes to create participant state machines don't get
                       // interrupted upon abort.
                       _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(),
                                                       _markKilledExecutor);
                   })
                   .then([this] { return _waitForMajority(_ctHolder->getStepdownToken()); })
                   .then([this, executor]() {
                       pauseBeforeTellDonorToRefresh.pauseWhileSet();
                       _establishAllDonorsAsParticipants(executor);
                   })
                   .then([this, executor] { _establishAllRecipientsAsParticipants(executor); })
                   .onCompletion([this](Status status) {
                       // Swap back to using operation contexts canceled upon abort until ready to
                       // persist the decision or unrecoverable error.
                       _cancelableOpCtxFactory.emplace(_ctHolder->getAbortToken(),
                                                       _markKilledExecutor);

                       return status;
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5093702,
                  "Resharding coordinator encountered transient error while telling participants "
                  "to refresh",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken());
}

ExecutorFuture<void> ReshardingCoordinator::_initializeCoordinator(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this] { _insertCoordDocAndChangeOrigCollEntry(); })
                   .then([this] { _calculateParticipantsAndChunksThenWriteToDisk(); });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5093703,
                  "Resharding coordinator encountered transient error while initializing",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onCompletion([this, executor](Status status) {
            if (_ctHolder->isSteppingOrShuttingDown()) {
                return ExecutorFuture<void>(**executor, status);
            }

            if (_coordinatorDoc.getState() != CoordinatorStateEnum::kPreparingToDonate) {
                return ExecutorFuture<void>(**executor, status);
            }

            // Regardless of error or non-error, guarantee that once the coordinator
            // completes its transition to kPreparingToDonate, participants are aware of
            // the resharding operation and their state machines are created.
            return _tellAllParticipantsReshardingStarted(executor);
        })
        .onError([this, executor](Status status) {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeStartingErrorFlow.pauseWhileSet(opCtx.get());
            }

            if (_ctHolder->isSteppingOrShuttingDown()) {
                return ExecutorFuture<void>(**executor, status);
            }

            if (_ctHolder->isAborted()) {
                // If the abort cancellation token was triggered, implying that a user ran the abort
                // command, override status with a resharding abort error.
                //
                // Note for debugging purposes: Ensure the original error status is recorded in the
                // logs before replacing it.
                status = {ErrorCodes::ReshardCollectionAborted, "aborted"};
            }

            auto nss = _coordinatorDoc.getSourceNss();
            LOGV2(4956903,
                  "Resharding failed",
                  logAttrs(nss),
                  "newShardKeyPattern"_attr = _coordinatorDoc.getReshardingKey(),
                  "error"_attr = status);

            // Allow abort to continue except when stepped down.
            _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(), _markKilledExecutor);

            // If we're already quiesced here it means we failed over and need to preserve the
            // original abort reason.
            if (_coordinatorDoc.getState() == CoordinatorStateEnum::kQuiesced) {
                _originalReshardingStatus.emplace(Status::OK());
                auto originalAbortReason = _coordinatorDoc.getAbortReason();
                if (originalAbortReason) {
                    _originalReshardingStatus.emplace(
                        sharding_ddl_util_deserializeErrorStatusFromBSON(
                            BSON("status" << *originalAbortReason).firstElement()));
                }
                markCompleted(*_originalReshardingStatus, _metrics.get());
                // We must return status here, not _originalReshardingStatus, because the latter
                // may be Status::OK() and not abort the future flow.
                return ExecutorFuture<void>(**executor, status);
            } else if (_coordinatorDoc.getState() < CoordinatorStateEnum::kPreparingToDonate) {
                return _onAbortCoordinatorOnly(executor, status);
            } else {
                return _onAbortCoordinatorAndParticipants(executor, status);
            }
        });
}

ExecutorFuture<ReshardingCoordinatorDocument> ReshardingCoordinator::_runUntilReadyToCommit(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) noexcept {
    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this, executor] { return _awaitAllDonorsReadyToDonate(executor); })
                   .then([this, executor] {
                       if (_coordinatorDoc.getState() == CoordinatorStateEnum::kCloning) {
                           _tellAllRecipientsToRefresh(executor);
                       }
                   })
                   .then([this, executor] { return _awaitAllRecipientsFinishedCloning(executor); })
                   .then([this, executor] {
                       if (_coordinatorDoc.getState() == CoordinatorStateEnum::kApplying) {
                           _tellAllDonorsToRefresh(executor);
                       }
                   })
                   .then([this, executor] { return _awaitAllRecipientsFinishedApplying(executor); })
                   .then([this, executor] {
                       if (_coordinatorDoc.getState() == CoordinatorStateEnum::kBlockingWrites) {
                           _tellAllDonorsToRefresh(executor);
                       }
                   })
                   .then([this, executor] {
                       return _awaitAllRecipientsInStrictConsistency(executor);
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5093704,
                  "Resharding coordinator encountered transient error",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until<StatusWith<ReshardingCoordinatorDocument>>(
            [](const StatusWith<ReshardingCoordinatorDocument>& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onCompletion([this](auto passthroughFuture) {
            _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(), _markKilledExecutor);
            return passthroughFuture;
        })
        .onError([this, executor](Status status) -> ExecutorFuture<ReshardingCoordinatorDocument> {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeStartingErrorFlow.pauseWhileSet(opCtx.get());
            }

            if (_ctHolder->isSteppingOrShuttingDown()) {
                return ExecutorFuture<ReshardingCoordinatorDocument>(**executor, status);
            }

            if (_ctHolder->isAborted()) {
                // If the abort cancellation token was triggered, implying that a user ran the abort
                // command, override status with a resharding abort error.
                status = {ErrorCodes::ReshardCollectionAborted, "aborted"};
            }

            auto nss = _coordinatorDoc.getSourceNss();
            LOGV2(4956902,
                  "Resharding failed",
                  logAttrs(nss),
                  "newShardKeyPattern"_attr = _coordinatorDoc.getReshardingKey(),
                  "error"_attr = status);

            invariant(_coordinatorDoc.getState() >= CoordinatorStateEnum::kPreparingToDonate);

            return _onAbortCoordinatorAndParticipants(executor, status)
                .onCompletion([](Status status) {
                    return StatusWith<ReshardingCoordinatorDocument>(status);
                });
        });
}

ExecutorFuture<void> ReshardingCoordinator::_commitAndFinishReshardOperation(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const ReshardingCoordinatorDocument& updatedCoordinatorDoc) noexcept {
    return resharding::WithAutomaticRetry([this, executor, updatedCoordinatorDoc] {
               return ExecutorFuture<void>(**executor)
                   .then(
                       [this, executor, updatedCoordinatorDoc] { _commit(updatedCoordinatorDoc); });
           })
        .onTransientError([](const Status& status) {
            LOGV2(7698801,
                  "Resharding coordinator encountered transient error while committing",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken())
        .onError([this, executor](Status status) {
            if (status == ErrorCodes::TransactionTooLargeForCache) {
                return _onAbortCoordinatorAndParticipants(executor, status);
            }
            return ExecutorFuture<void>(**executor, status);
        })
        .then([this, executor, updatedCoordinatorDoc] {
            return resharding::WithAutomaticRetry([this, executor, updatedCoordinatorDoc] {
                       return ExecutorFuture<void>(**executor)
                           .then([this] { return _waitForMajority(_ctHolder->getStepdownToken()); })
                           .thenRunOn(**executor)
                           .then(
                               [this, executor] { _generateOpEventOnCoordinatingShard(executor); })
                           .then([this, executor] {
                               _tellAllParticipantsToCommit(_coordinatorDoc.getSourceNss(),
                                                            executor);
                           })
                           .then([this] {
                               _updateChunkImbalanceMetrics(_coordinatorDoc.getSourceNss());
                           })
                           .then([this, updatedCoordinatorDoc] {
                               auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                               resharding::removeChunkDocs(opCtx.get(),
                                                           updatedCoordinatorDoc.getSourceUUID());
                               return Status::OK();
                           })
                           .then([this, executor] {
                               return _awaitAllParticipantShardsDone(executor);
                           })
                           .then([this, executor] {
                               _metrics->setEndFor(ReshardingMetrics::TimedPhase::kCriticalSection,
                                                   getCurrentTime());

                               // Best-effort attempt to trigger a refresh on the participant shards
                               // so they see the collection metadata without reshardingFields and
                               // no longer throw ReshardCollectionInProgress. There is no guarantee
                               // this logic ever runs if the config server primary steps down after
                               // having removed the coordinator state document.
                               return _tellAllRecipientsToRefresh(executor);
                           });
                   })
                .onTransientError([](const Status& status) {
                    LOGV2(5093705,
                          "Resharding coordinator encountered transient error while committing",
                          "error"_attr = status);
                })
                .onUnrecoverableError([](const Status& status) {})
                .until<Status>([](const Status& status) { return status.isOK(); })
                .on(**executor, _ctHolder->getStepdownToken())
                .onError([this, executor](Status status) {
                    {
                        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                        reshardingPauseCoordinatorBeforeStartingErrorFlow.pauseWhileSet(
                            opCtx.get());
                    }

                    if (_ctHolder->isSteppingOrShuttingDown()) {
                        return status;
                    }

                    LOGV2_FATAL(
                        5277000,
                        "Unrecoverable error past the point resharding was guaranteed to succeed",
                        "error"_attr = redact(status));
                });
        });
}

SemiFuture<void> ReshardingCoordinator::run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                            const CancellationToken& stepdownToken) noexcept {
    pauseBeforeCTHolderInitialization.pauseWhileSet();

    auto abortCalled = [&] {
        stdx::lock_guard<Latch> lk(_abortCalledMutex);
        _ctHolder = std::make_unique<CoordinatorCancellationTokenHolder>(stepdownToken);
        return _abortCalled;
    }();

    if (abortCalled) {
        if (abortCalled == AbortType::kAbortSkipQuiesce) {
            _ctHolder->cancelQuiescePeriod();
        }
        _ctHolder->abort();
    }

    _markKilledExecutor->startup();
    _cancelableOpCtxFactory.emplace(_ctHolder->getAbortToken(), _markKilledExecutor);

    return _isReshardingOpRedundant(executor)
        .thenRunOn(_coordinatorService->getInstanceCleanupExecutor())
        .onCompletion([this, self = shared_from_this(), executor](
                          StatusWith<bool> isOpRedundantSW) -> ExecutorFuture<void> {
            if (isOpRedundantSW.isOK() && isOpRedundantSW.getValue()) {
                this->_coordinatorService->releaseInstance(this->_id, isOpRedundantSW.getStatus());
                _coordinatorDocWrittenPromise.emplaceValue();
                _completionPromise.emplaceValue();
                _reshardingCoordinatorObserver->fulfillPromisesBeforePersistingStateDoc();
                return ExecutorFuture<void>(**executor, isOpRedundantSW.getStatus());
            } else if (!isOpRedundantSW.isOK()) {
                this->_coordinatorService->releaseInstance(this->_id, isOpRedundantSW.getStatus());
                _coordinatorDocWrittenPromise.setError(isOpRedundantSW.getStatus());
                _completionPromise.setError(isOpRedundantSW.getStatus());
                _reshardingCoordinatorObserver->interrupt(isOpRedundantSW.getStatus());
                return ExecutorFuture<void>(**executor, isOpRedundantSW.getStatus());
            }
            return _runReshardingOp(executor);
        })
        .onCompletion([this, self = shared_from_this(), executor](Status status) {
            _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(), _markKilledExecutor);
            return _quiesce(executor, std::move(status));
        })
        .semi();
}

ExecutorFuture<void> ReshardingCoordinator::_quiesce(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, Status status) {
    if (_coordinatorDoc.getState() == CoordinatorStateEnum::kQuiesced) {
        return (*executor)
            ->sleepUntil(*_coordinatorDoc.getQuiescePeriodEnd(), _ctHolder->getCancelQuiesceToken())
            .onCompletion([this, self = shared_from_this(), executor, status](Status sleepStatus) {
                LOGV2_DEBUG(7760405,
                            1,
                            "Resharding coordinator quiesce period done",
                            "reshardingUUID"_attr = _coordinatorDoc.getReshardingUUID());
                if (!_ctHolder->isSteppingOrShuttingDown()) {
                    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;
                    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kDone);
                    executeMetadataChangesInTxn(
                        opCtx.get(),
                        [&updatedCoordinatorDoc](OperationContext* opCtx, TxnNumber txnNumber) {
                            writeToCoordinatorStateNss(opCtx,
                                                       nullptr /* metrics have already been freed */
                                                       ,
                                                       updatedCoordinatorDoc,
                                                       txnNumber);
                        });
                    LOGV2_DEBUG(7760406,
                                1,
                                "Resharding coordinator removed state doc after quiesce",
                                "reshardingUUID"_attr = _coordinatorDoc.getReshardingUUID());
                }
                return status;
            })
            .thenRunOn(_coordinatorService->getInstanceCleanupExecutor())
            .onCompletion([this, self = shared_from_this(), executor, status](Status deleteStatus) {
                _quiescePeriodFinishedPromise.emplaceValue();
                return status;
            });
    }
    // No quiesce period is required.
    _quiescePeriodFinishedPromise.emplaceValue();
    return ExecutorFuture<void>(**executor, status);
}

ExecutorFuture<void> ReshardingCoordinator::_runReshardingOp(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return _initializeCoordinator(executor)
        .then([this, executor] { return _runUntilReadyToCommit(executor); })
        .then([this, executor](const ReshardingCoordinatorDocument& updatedCoordinatorDoc) {
            return _commitAndFinishReshardOperation(executor, updatedCoordinatorDoc);
        })
        .onCompletion([this, executor](Status status) {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            reshardingPauseCoordinatorBeforeCompletion.executeIf(
                [&](const BSONObj&) {
                    reshardingPauseCoordinatorBeforeCompletion.pauseWhileSetAndNotCanceled(
                        opCtx.get(), _ctHolder->getStepdownToken());
                },
                [&](const BSONObj& data) {
                    auto ns = data.getStringField("sourceNamespace");
                    return ns.empty() ? true
                                      : ns.toString() ==
                            NamespaceStringUtil::serialize(_coordinatorDoc.getSourceNss(),
                                                           SerializationContext::stateDefault());
                });

            {
                auto lg = stdx::lock_guard(_fulfillmentMutex);
                // reportStatus is the status reported back to the caller, which may be
                // different than the status if we interrupted the future chain because the
                // resharding was already completed on a previous primary.
                auto reportStatus = _originalReshardingStatus.value_or(status);
                if (reportStatus.isOK()) {
                    _completionPromise.emplaceValue();

                    if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
                        _coordinatorDocWrittenPromise.emplaceValue();
                    }
                } else {
                    _completionPromise.setError(reportStatus);

                    if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
                        _coordinatorDocWrittenPromise.setError(reportStatus);
                    }
                }
            }

            if (_criticalSectionTimeoutCbHandle) {
                (*executor)->cancel(*_criticalSectionTimeoutCbHandle);
            }

            return status;
        })
        .thenRunOn(_coordinatorService->getInstanceCleanupExecutor())
        .onCompletion([this](Status outerStatus) {
            // Wait for the commit monitor to halt. We ignore any ignores because the
            // ReshardingCoordinator instance is already exiting at this point.
            return _commitMonitorQuiesced
                .thenRunOn(_coordinatorService->getInstanceCleanupExecutor())
                .onCompletion([outerStatus](Status) { return outerStatus; });
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            _metrics->onStateTransition(_coordinatorDoc.getState(), boost::none);
            if (resharding::gFeatureFlagReshardingImprovements.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                _logStatsOnCompletion(status.isOK());
            }

            // Destroy metrics early so its lifetime will not be tied to the lifetime of this
            // state machine. This is because we have future callbacks copy shared pointers to this
            // state machine that causes it to live longer than expected and potentially overlap
            // with a newer instance when stepping up. The commit monitor also has a shared pointer
            // to the metrics, so release this as well.
            _metrics.reset();
            _commitMonitor.reset();

            if (!status.isOK()) {
                {
                    auto lg = stdx::lock_guard(_fulfillmentMutex);
                    if (!_completionPromise.getFuture().isReady()) {
                        _completionPromise.setError(status);
                    }

                    if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
                        _coordinatorDocWrittenPromise.setError(status);
                    }
                }
                _reshardingCoordinatorObserver->interrupt(status);
            }
        });
}

ExecutorFuture<void> ReshardingCoordinator::_onAbortCoordinatorOnly(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const Status& status) {
    if (_coordinatorDoc.getState() == CoordinatorStateEnum::kUnused) {
        return ExecutorFuture<void>(**executor, status);
    }

    return resharding::WithAutomaticRetry([this, executor, status] {
               auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

               // Notify metrics as the operation is now complete for external observers.
               markCompleted(status, _metrics.get());

               // The temporary collection and its corresponding entries were never created. Only
               // the coordinator document and reshardingFields require cleanup.
               _removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(opCtx.get(), status);
               return status;
           })
        .onTransientError([](const Status& retryStatus) {
            LOGV2(5093706,
                  "Resharding coordinator encountered transient error while aborting",
                  "error"_attr = retryStatus);
        })
        .onUnrecoverableError([](const Status& retryStatus) {})
        .until<Status>([](const Status& retryStatus) { return retryStatus.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken())
        // Return back original status.
        .then([status] { return status; });
}

ExecutorFuture<void> ReshardingCoordinator::_onAbortCoordinatorAndParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const Status& status) {
    // Participants should never be waited upon to complete the abort if they were never made aware
    // of the resharding operation (the coordinator flushing its state change to
    // kPreparingToDonate).
    invariant(_coordinatorDoc.getState() >= CoordinatorStateEnum::kPreparingToDonate);

    return resharding::WithAutomaticRetry([this, executor, status] {
               return ExecutorFuture<void>(**executor)
                   .then([this, executor, status] {
                       if (_coordinatorDoc.getState() != CoordinatorStateEnum::kAborting) {
                           // The coordinator only transitions into kAborting if there are
                           // participants to wait on before transitioning to kDone.
                           _updateCoordinatorDocStateAndCatalogEntries(
                               CoordinatorStateEnum::kAborting,
                               _coordinatorDoc,
                               boost::none,
                               boost::none,
                               status);
                       }
                   })
                   .then([this] { return _waitForMajority(_ctHolder->getStepdownToken()); })
                   .thenRunOn(**executor)
                   .then([this, executor, status] {
                       _tellAllParticipantsToAbort(executor,
                                                   status == ErrorCodes::ReshardCollectionAborted);

                       // Wait for all participants to acknowledge the operation reached an
                       // unrecoverable error.
                       return future_util::withCancellation(
                           _awaitAllParticipantShardsDone(executor), _ctHolder->getStepdownToken());
                   });
           })
        .onTransientError([](const Status& retryStatus) {
            LOGV2(5093707,
                  "Resharding coordinator encountered transient error while aborting all "
                  "participants",
                  "error"_attr = retryStatus);
        })
        .onUnrecoverableError([](const Status& retryStatus) {})
        .until<Status>([](const Status& retryStatus) { return retryStatus.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken())
        // Return back the original status.
        .then([status] { return status; });
}

void ReshardingCoordinator::abort(bool skipQuiescePeriod) {
    auto ctHolderInitialized = [&] {
        stdx::lock_guard<Latch> lk(_abortCalledMutex);
        skipQuiescePeriod = skipQuiescePeriod || _abortCalled == AbortType::kAbortSkipQuiesce;
        _abortCalled =
            skipQuiescePeriod ? AbortType::kAbortSkipQuiesce : AbortType::kAbortWithQuiesce;
        return !(_ctHolder == nullptr);
    }();

    if (ctHolderInitialized) {
        if (skipQuiescePeriod)
            _ctHolder->cancelQuiescePeriod();
        _ctHolder->abort();
    }
}

boost::optional<BSONObj> ReshardingCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode,
    MongoProcessInterface::CurrentOpSessionsMode) noexcept {
    return _metrics->reportForCurrentOp();
}

std::shared_ptr<ReshardingCoordinatorObserver> ReshardingCoordinator::getObserver() {
    return _reshardingCoordinatorObserver;
}

void ReshardingCoordinator::onOkayToEnterCritical() {
    _fulfillOkayToEnterCritical(Status::OK());
}

void ReshardingCoordinator::_fulfillOkayToEnterCritical(Status status) {
    auto lg = stdx::lock_guard(_fulfillmentMutex);
    if (_canEnterCritical.getFuture().isReady())
        return;

    if (status.isOK()) {
        LOGV2(5391601, "Marking resharding operation okay to enter critical section");
        _canEnterCritical.emplaceValue();
    } else {
        _canEnterCritical.setError(std::move(status));
    }
}

SemiFuture<void> ReshardingCoordinator::_waitForMajority(const CancellationToken& token) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto client = opCtx->getClient();
    repl::ReplClientInfo::forClient(client).setLastOpToSystemLastOpTime(opCtx.get());
    auto opTime = repl::ReplClientInfo::forClient(client).getLastOp();
    return WaitForMajorityService::get(client->getServiceContext())
        .waitUntilMajorityForWrite(client->getServiceContext(), opTime, token);
}

ExecutorFuture<bool> ReshardingCoordinator::_isReshardingOpRedundant(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    // We only check for redundancy when the resharding op first starts as it would be unsafe to
    // skip the remainder of the cleanup for the resharding operation if there was a primary
    // failover after the CoordinatorStateEnum::kCommitting state had been reached.
    if (_coordinatorDoc.getState() != CoordinatorStateEnum::kUnused) {
        return ExecutorFuture<bool>(**executor, false);
    }

    return resharding::WithAutomaticRetry([this, executor] {
               auto cancelableOpCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
               auto opCtx = cancelableOpCtx.get();
               boost::optional<ChunkManager> cm;

               // Ensure indexes are loaded in the catalog cache, along with the collection
               // placement.
               if (feature_flags::gGlobalIndexesShardingCatalog.isEnabled(
                       serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {

                   auto cri = uassertStatusOK(
                       Grid::get(opCtx)->catalogCache()->getTrackedCollectionRoutingInfoWithRefresh(
                           opCtx, _coordinatorDoc.getSourceNss()));
                   cm.emplace(cri.cm);
               } else {

                   auto cri =
                       uassertStatusOK(Grid::get(opCtx)
                                           ->catalogCache()
                                           ->getTrackedCollectionRoutingInfoWithPlacementRefresh(
                                               opCtx, _coordinatorDoc.getSourceNss()));
                   cm.emplace(cri.cm);
               }

               if (resharding::isMoveCollection(_metadata.getProvenance())) {
                   // Verify if the moveCollection is redundant by checking if the operation is
                   // attempting to move to the same shard.
                   std::set<ShardId> shardIdsSet;
                   cm->getAllShardIds(&shardIdsSet);
                   const auto currentShard =
                       _coordinatorDoc.getShardDistribution().get().front().getShard();
                   return shardIdsSet.find(currentShard) != shardIdsSet.end();
               }

               const auto currentShardKey = cm->getShardKeyPattern().getKeyPattern();
               // Verify if there is any work to be done by the resharding operation by checking
               // if the existing shard key matches the desired new shard key.
               bool isOpRedundant = SimpleBSONObjComparator::kInstance.evaluate(
                   currentShardKey.toBSON() == _coordinatorDoc.getReshardingKey().toBSON());

               // If forceRedistribution is true, still do resharding.
               if (isOpRedundant && _coordinatorDoc.getForceRedistribution() &&
                   *_coordinatorDoc.getForceRedistribution()) {
                   return false;
               }

               // If this is not forced same-key resharding, set forceRedistribution to false so
               // we can identify forced same-key resharding by this field later.
               _coordinatorDoc.setForceRedistribution(false);
               return isOpRedundant;
           })
        .onTransientError([](const StatusWith<bool>& status) {
            LOGV2(7074600,
                  "Resharding coordinator encountered transient error refreshing routing info",
                  "error"_attr = status.getStatus());
        })
        .onUnrecoverableError([](const StatusWith<bool>& status) {})
        .until<StatusWith<bool>>([](const StatusWith<bool>& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onError(([this, executor](StatusWith<bool> status) {
            if (_ctHolder->isAborted()) {
                // If the abort cancellation token was triggered, implying that a user ran the
                // abort command, override status with a resharding abort error.
                //
                // Note for debugging purposes: Ensure the original error status is recorded in
                // the logs before replacing it.
                status = {ErrorCodes::ReshardCollectionAborted, "aborted"};
            }
            return status;
        }));
}

void ReshardingCoordinator::_insertCoordDocAndChangeOrigCollEntry() {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kUnused) {
        if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
            _coordinatorDocWrittenPromise.emplaceValue();
        }

        if (_coordinatorDoc.getState() == CoordinatorStateEnum::kAborting ||
            _coordinatorDoc.getState() == CoordinatorStateEnum::kQuiesced) {
            _ctHolder->abort();
            // Force future chain to enter onError flow
            uasserted(ErrorCodes::ReshardCollectionAborted, "aborted");
        }

        return;
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    reshardingPauseCoordinatorBeforeInitializing.pauseWhileSetAndNotCanceled(
        opCtx.get(), _ctHolder->getStepdownToken());
    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kInitializing);
    resharding::insertCoordDocAndChangeOrigCollEntry(
        opCtx.get(), _metrics.get(), updatedCoordinatorDoc);
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);

    {
        // Note: don't put blocking or interruptible code in this block.
        const bool isSameKeyResharding =
            _coordinatorDoc.getForceRedistribution() && *_coordinatorDoc.getForceRedistribution();
        _coordinatorDocWrittenPromise.emplaceValue();
        // We need to call setIsSameKeyResharding first so the metrics can count same key resharding
        // correctly.
        _metrics->setIsSameKeyResharding(isSameKeyResharding);
        _metrics->onStarted();
    }

    pauseAfterInsertCoordinatorDoc.pauseWhileSet();
}

void ReshardingCoordinator::_calculateParticipantsAndChunksThenWriteToDisk() {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing) {
        return;
    }
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;

    // If zones is not provided by the user, we should use the existing zones for
    // this resharding operation.
    if (updatedCoordinatorDoc.getForceRedistribution() &&
        *updatedCoordinatorDoc.getForceRedistribution() && !updatedCoordinatorDoc.getZones()) {
        auto zones = resharding::getZonesFromExistingCollection(
            opCtx.get(), updatedCoordinatorDoc.getSourceNss());
        updatedCoordinatorDoc.setZones(std::move(zones));
    }

    auto shardsAndChunks = _reshardingCoordinatorExternalState->calculateParticipantShardsAndChunks(
        opCtx.get(), updatedCoordinatorDoc);

    updatedCoordinatorDoc.setDonorShards(std::move(shardsAndChunks.donorShards));
    updatedCoordinatorDoc.setRecipientShards(std::move(shardsAndChunks.recipientShards));
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);

    // Remove the presetReshardedChunks and zones from the coordinator document to reduce
    // the possibility of the document reaching the BSONObj size constraint.
    ShardKeyPattern shardKey(updatedCoordinatorDoc.getReshardingKey());
    std::vector<BSONObj> zones;
    if (updatedCoordinatorDoc.getZones()) {
        zones = resharding::buildTagsDocsFromZones(updatedCoordinatorDoc.getTempReshardingNss(),
                                                   *updatedCoordinatorDoc.getZones(),
                                                   shardKey);
    }
    updatedCoordinatorDoc.setPresetReshardedChunks(boost::none);
    updatedCoordinatorDoc.setZones(boost::none);

    auto indexVersion = _reshardingCoordinatorExternalState->getCatalogIndexVersion(
        opCtx.get(),
        updatedCoordinatorDoc.getSourceNss(),
        updatedCoordinatorDoc.getReshardingUUID());

    auto provenance = updatedCoordinatorDoc.getCommonReshardingMetadata().getProvenance();
    auto isUnsplittable = _reshardingCoordinatorExternalState->getIsUnsplittable(
                              opCtx.get(), updatedCoordinatorDoc.getSourceNss()) ||
        (provenance && provenance.get() == ProvenanceEnum::kUnshardCollection);

    resharding::writeParticipantShardsAndTempCollInfo(opCtx.get(),
                                                      _metrics.get(),
                                                      updatedCoordinatorDoc,
                                                      std::move(shardsAndChunks.initialChunks),
                                                      std::move(zones),
                                                      std::move(indexVersion),
                                                      isUnsplittable);
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);

    reshardingPauseCoordinatorAfterPreparingToDonate.pauseWhileSetAndNotCanceled(
        opCtx.get(), _ctHolder->getAbortToken());
}

ReshardingApproxCopySize computeApproxCopySize(ReshardingCoordinatorDocument& coordinatorDoc) {
    const auto numRecipients = coordinatorDoc.getRecipientShards().size();
    iassert(ErrorCodes::BadValue,
            "Expected to find at least one recipient in the coordinator document",
            numRecipients > 0);

    // Compute the aggregate for the number of documents and bytes to copy.
    long aggBytesToCopy = 0, aggDocumentsToCopy = 0;
    for (auto donor : coordinatorDoc.getDonorShards()) {
        if (const auto bytesToClone = donor.getMutableState().getBytesToClone()) {
            aggBytesToCopy += *bytesToClone;
        }

        if (const auto documentsToClone = donor.getMutableState().getDocumentsToClone()) {
            aggDocumentsToCopy += *documentsToClone;
        }
    }

    // Calculate the approximate number of documents and bytes that each recipient will clone.
    ReshardingApproxCopySize approxCopySize;
    approxCopySize.setApproxBytesToCopy(aggBytesToCopy / numRecipients);
    approxCopySize.setApproxDocumentsToCopy(aggDocumentsToCopy / numRecipients);
    return approxCopySize;
}

ExecutorFuture<void> ReshardingCoordinator::_awaitAllDonorsReadyToDonate(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kPreparingToDonate) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllDonorsReadyToDonate(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeCloning.pauseWhileSetAndNotCanceled(
                    opCtx.get(), _ctHolder->getAbortToken());
            }

            auto highestMinFetchTimestamp = resharding::getHighestMinFetchTimestamp(
                coordinatorDocChangedOnDisk.getDonorShards());

            _updateCoordinatorDocStateAndCatalogEntries(
                CoordinatorStateEnum::kCloning,
                coordinatorDocChangedOnDisk,
                highestMinFetchTimestamp,
                computeApproxCopySize(coordinatorDocChangedOnDisk));
        })
        .then([this] { return _waitForMajority(_ctHolder->getAbortToken()); });
}

ExecutorFuture<void> ReshardingCoordinator::_awaitAllRecipientsFinishedCloning(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kCloning) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllRecipientsFinishedCloning(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            this->_updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kApplying,
                                                              coordinatorDocChangedOnDisk);
        })
        .then([this] { return _waitForMajority(_ctHolder->getAbortToken()); });
}

void ReshardingCoordinator::_startCommitMonitor(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_commitMonitor) {
        return;
    }

    _commitMonitor = std::make_shared<resharding::CoordinatorCommitMonitor>(
        _metrics,
        _coordinatorDoc.getSourceNss(),
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards()),
        **executor,
        _ctHolder->getCommitMonitorToken());

    _commitMonitorQuiesced = _commitMonitor->waitUntilRecipientsAreWithinCommitThreshold()
                                 .thenRunOn(**executor)
                                 .onCompletion([this](Status status) {
                                     _fulfillOkayToEnterCritical(status);
                                     return status;
                                 })
                                 .share();
}

ExecutorFuture<void> ReshardingCoordinator::_awaitAllRecipientsFinishedApplying(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kApplying) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return ExecutorFuture<void>(**executor)
        .then([this, executor] {
            _startCommitMonitor(executor);

            LOGV2(5391602, "Resharding operation waiting for an okay to enter critical section");

            // The _reshardingCoordinatorObserver->awaitAllRecipientsInStrictConsistency() future is
            // used for reporting recipient shard errors encountered during the Applying phase and
            // in turn aborting the resharding operation.
            // For all other cases, the _canEnterCritical.getFuture() resolves first and the
            // operation can then proceed to entering the critical section depending on the status
            // returned.
            return future_util::withCancellation(
                       whenAny(
                           _canEnterCritical.getFuture().thenRunOn(**executor),
                           _reshardingCoordinatorObserver->awaitAllRecipientsInStrictConsistency()
                               .thenRunOn(**executor)
                               .ignoreValue()),
                       _ctHolder->getAbortToken())
                .thenRunOn(**executor)
                .then([](auto result) { return result.result; })
                .onCompletion([this](Status status) {
                    _ctHolder->cancelCommitMonitor();
                    if (status.isOK()) {
                        LOGV2(5391603, "Resharding operation is okay to enter critical section");
                    }
                    return status;
                });
        })
        .then([this, executor] {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeBlockingWrites.pauseWhileSetAndNotCanceled(
                    opCtx.get(), _ctHolder->getAbortToken());
            }

            this->_updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kBlockingWrites,
                                                              _coordinatorDoc);
            _metrics->setStartFor(ReshardingMetrics::TimedPhase::kCriticalSection,
                                  getCurrentTime());
        })
        .then([this] { return _waitForMajority(_ctHolder->getAbortToken()); })
        .thenRunOn(**executor)
        .then([this, executor] {
            const auto criticalSectionTimeout =
                Milliseconds(resharding::gReshardingCriticalSectionTimeoutMillis.load());
            const auto criticalSectionExpiresAt = (*executor)->now() + criticalSectionTimeout;
            LOGV2_INFO(
                5573001, "Engaging critical section", "timeoutAt"_attr = criticalSectionExpiresAt);

            auto swCbHandle = (*executor)->scheduleWorkAt(
                criticalSectionExpiresAt,
                [this](const executor::TaskExecutor::CallbackArgs& cbData) {
                    if (!cbData.status.isOK()) {
                        return;
                    }
                    _reshardingCoordinatorObserver->onCriticalSectionTimeout();
                });

            if (!swCbHandle.isOK()) {
                _reshardingCoordinatorObserver->interrupt(swCbHandle.getStatus());
            }

            _criticalSectionTimeoutCbHandle = swCbHandle.getValue();
        });
}

ExecutorFuture<ReshardingCoordinatorDocument>
ReshardingCoordinator::_awaitAllRecipientsInStrictConsistency(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kBlockingWrites) {
        // If in recovery, just return the existing _stateDoc.
        return ExecutorFuture<ReshardingCoordinatorDocument>(**executor, _coordinatorDoc);
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllRecipientsInStrictConsistency(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor);
}

void ReshardingCoordinator::_commit(const ReshardingCoordinatorDocument& coordinatorDoc) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kBlockingWrites) {
        invariant(_coordinatorDoc.getState() != CoordinatorStateEnum::kAborting);
        return;
    }

    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kCommitting);

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    reshardingPauseCoordinatorBeforeDecisionPersisted.pauseWhileSetAndNotCanceled(
        opCtx.get(), _ctHolder->getAbortToken());

    // The new epoch and timestamp to use for the resharded collection to indicate that the
    // collection is a new incarnation of the namespace
    auto newCollectionEpoch = OID::gen();
    auto newCollectionTimestamp = [&] {
        const auto now = VectorClock::get(opCtx.get())->getTime();
        return now.clusterTime().asTimestamp();
    }();

    auto indexVersion = _reshardingCoordinatorExternalState->getCatalogIndexVersionForCommit(
        opCtx.get(), updatedCoordinatorDoc.getTempReshardingNss());

    // Retrieve the exact placement of the resharded collection from the routing table.
    // The 'recipientShards' field of the coordinator doc cannot be used for this purpose as it
    // always includes the primary shard for the parent database (even when it doesn't own any chunk
    // under the new key pattern).
    auto reshardedCollectionPlacement = [&] {
        std::set<ShardId> collectionPlacement;
        std::vector<ShardId> collectionPlacementAsVector;

        const auto [cm, _] =
            uassertStatusOK(Grid::get(opCtx.get())
                                ->catalogCache()
                                ->getTrackedCollectionRoutingInfoWithPlacementRefresh(
                                    opCtx.get(), coordinatorDoc.getTempReshardingNss()));

        cm.getAllShardIds(&collectionPlacement);

        collectionPlacementAsVector.reserve(collectionPlacement.size());
        for (auto& elem : collectionPlacement) {
            collectionPlacementAsVector.emplace_back(elem);
        }
        return collectionPlacementAsVector;
    }();

    resharding::writeDecisionPersistedState(opCtx.get(),
                                            _metrics.get(),
                                            updatedCoordinatorDoc,
                                            std::move(newCollectionEpoch),
                                            std::move(newCollectionTimestamp),
                                            std::move(indexVersion),
                                            reshardedCollectionPlacement);

    // Update the in memory state
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);
}

void ReshardingCoordinator::_generateOpEventOnCoordinatingShard(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

    CollectionResharded eventNotification(_coordinatorDoc.getSourceNss(),
                                          _coordinatorDoc.getSourceUUID(),
                                          _coordinatorDoc.getReshardingUUID(),
                                          _coordinatorDoc.getReshardingKey().toBSON());
    eventNotification.setSourceKey(_coordinatorDoc.getSourceKey());
    eventNotification.setNumInitialChunks(_coordinatorDoc.getNumInitialChunks());
    eventNotification.setUnique(_coordinatorDoc.getUnique());
    eventNotification.setCollation(_coordinatorDoc.getCollation());

    ShardsvrNotifyShardingEventRequest request(notify_sharding_event::kCollectionResharded,
                                               eventNotification.toBSON());

    const auto dbPrimaryShard =
        uassertStatusOK(
            Grid::get(opCtx.get())
                ->catalogCache()
                ->getDatabaseWithRefresh(opCtx.get(), _coordinatorDoc.getSourceNss().dbName()))
            ->getPrimary();

    // In case the recipient is running a legacy binary, swallow the error.
    try {
        async_rpc::GenericArgs args;
        async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
        const auto opts =
            std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrNotifyShardingEventRequest>>(
                **executor, _ctHolder->getStepdownToken(), request, args);
        opts->cmd.setDbName(DatabaseName::kAdmin);
        _reshardingCoordinatorExternalState->sendCommandToShards(
            opCtx.get(), opts, {dbPrimaryShard});
    } catch (const ExceptionFor<ErrorCodes::UnsupportedShardingEventNotification>& e) {
        LOGV2_WARNING(7403100,
                      "Unable to generate op entry on reshardCollection commit",
                      "error"_attr = redact(e.toStatus()));
    }
}


ExecutorFuture<void> ReshardingCoordinator::_awaitAllParticipantShardsDone(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    std::vector<ExecutorFuture<ReshardingCoordinatorDocument>> futures;
    futures.emplace_back(
        _reshardingCoordinatorObserver->awaitAllRecipientsDone().thenRunOn(**executor));
    futures.emplace_back(
        _reshardingCoordinatorObserver->awaitAllDonorsDone().thenRunOn(**executor));

    // We only allow the stepdown token to cancel operations after progressing past
    // kCommitting.
    return future_util::withCancellation(whenAllSucceed(std::move(futures)),
                                         _ctHolder->getStepdownToken())
        .thenRunOn(**executor)
        .then([this, executor](const auto& coordinatorDocsChangedOnDisk) {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            auto& coordinatorDoc = coordinatorDocsChangedOnDisk[1];

            boost::optional<Status> abortReason;
            if (coordinatorDoc.getAbortReason()) {
                abortReason = resharding::getStatusFromAbortReason(coordinatorDoc);
            }

            if (!abortReason) {
                // (SERVER-54231) Ensure every catalog entry referring the source uuid is
                // cleared out on every shard.
                const auto allShardIds =
                    Grid::get(opCtx.get())->shardRegistry()->getAllShardIds(opCtx.get());
                const auto& nss = coordinatorDoc.getSourceNss();
                const auto& notMatchingThisUUID = coordinatorDoc.getReshardingUUID();
                const auto cmd = ShardsvrDropCollectionIfUUIDNotMatchingWithWriteConcernRequest(
                    nss, notMatchingThisUUID);

                async_rpc::GenericArgs args;
                async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
                auto opts = std::make_shared<async_rpc::AsyncRPCOptions<
                    ShardsvrDropCollectionIfUUIDNotMatchingWithWriteConcernRequest>>(
                    **executor, _ctHolder->getStepdownToken(), cmd, args);
                _reshardingCoordinatorExternalState->sendCommandToShards(
                    opCtx.get(), opts, allShardIds);
            }

            reshardingPauseCoordinatorBeforeRemovingStateDoc.pauseWhileSetAndNotCanceled(
                opCtx.get(), _ctHolder->getStepdownToken());

            // Notify metrics as the operation is now complete for external observers.
            markCompleted(abortReason ? *abortReason : Status::OK(), _metrics.get());

            _removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(opCtx.get(), abortReason);
        });
}

void ReshardingCoordinator::_updateCoordinatorDocStateAndCatalogEntries(
    CoordinatorStateEnum nextState,
    ReshardingCoordinatorDocument coordinatorDoc,
    boost::optional<Timestamp> cloneTimestamp,
    boost::optional<ReshardingApproxCopySize> approxCopySize,
    boost::optional<Status> abortReason) {
    // Build new state doc for coordinator state update
    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(nextState);
    resharding::emplaceApproxBytesToCopyIfExists(updatedCoordinatorDoc, std::move(approxCopySize));
    resharding::emplaceCloneTimestampIfExists(updatedCoordinatorDoc, std::move(cloneTimestamp));
    resharding::emplaceTruncatedAbortReasonIfExists(updatedCoordinatorDoc, abortReason);

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    resharding::writeStateTransitionAndCatalogUpdatesThenBumpCollectionPlacementVersions(
        opCtx.get(), _metrics.get(), updatedCoordinatorDoc);

    // Update in-memory coordinator doc
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);
}

void ReshardingCoordinator::_removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(
    OperationContext* opCtx, boost::optional<Status> abortReason) {
    auto updatedCoordinatorDoc = resharding::removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(
        opCtx, _metrics.get(), _coordinatorDoc, abortReason);

    // Update in-memory coordinator doc.
    installCoordinatorDoc(opCtx, updatedCoordinatorDoc);
}

template <typename CommandType>
void ReshardingCoordinator::_sendCommandToAllParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> opts) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto donorShardIds =
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getDonorShards());
    auto recipientShardIds =
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards());
    std::set<ShardId> participantShardIds{donorShardIds.begin(), donorShardIds.end()};
    participantShardIds.insert(recipientShardIds.begin(), recipientShardIds.end());

    _reshardingCoordinatorExternalState->sendCommandToShards(
        opCtx.get(), opts, {participantShardIds.begin(), participantShardIds.end()});
}

template <typename CommandType>
void ReshardingCoordinator::_sendCommandToAllRecipients(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> opts) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto recipientShardIds =
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards());

    _reshardingCoordinatorExternalState->sendCommandToShards(
        opCtx.get(), opts, {recipientShardIds.begin(), recipientShardIds.end()});
}

template <typename CommandType>
void ReshardingCoordinator::_sendCommandToAllDonors(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> opts) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto donorShardIds =
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getDonorShards());

    _reshardingCoordinatorExternalState->sendCommandToShards(
        opCtx.get(), opts, {donorShardIds.begin(), donorShardIds.end()});
}

void ReshardingCoordinator::_establishAllDonorsAsParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    invariant(_coordinatorDoc.getState() == CoordinatorStateEnum::kPreparingToDonate);
    async_rpc::GenericArgs args;
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
    auto opts = makeFlushRoutingTableCacheUpdatesOptions(
        _coordinatorDoc.getSourceNss(), **executor, _ctHolder->getStepdownToken(), args);
    opts->cmd.setDbName(DatabaseName::kAdmin);
    _sendCommandToAllDonors(executor, opts);
}

void ReshardingCoordinator::_establishAllRecipientsAsParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    invariant(_coordinatorDoc.getState() == CoordinatorStateEnum::kPreparingToDonate);
    async_rpc::GenericArgs args;
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
    auto opts = makeFlushRoutingTableCacheUpdatesOptions(
        _coordinatorDoc.getTempReshardingNss(), **executor, _ctHolder->getStepdownToken(), args);
    opts->cmd.setDbName(DatabaseName::kAdmin);
    _sendCommandToAllRecipients(executor, opts);
}

void ReshardingCoordinator::_tellAllRecipientsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    NamespaceString nssToRefresh;
    // Refresh the temporary namespace if the coordinator is in a state prior to 'kCommitting'.
    // A refresh of recipients while in 'kCommitting' should be accompanied by a refresh of
    // all participants for the original namespace to ensure correctness.
    if (_coordinatorDoc.getState() < CoordinatorStateEnum::kCommitting) {
        nssToRefresh = _coordinatorDoc.getTempReshardingNss();
    } else {
        nssToRefresh = _coordinatorDoc.getSourceNss();
    }

    async_rpc::GenericArgs args;
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args, kMajorityWriteConcern);
    auto opts = createFlushReshardingStateChangeOptions(nssToRefresh,
                                                        _coordinatorDoc.getReshardingUUID(),
                                                        **executor,
                                                        _ctHolder->getStepdownToken(),
                                                        args);
    opts->cmd.setDbName(DatabaseName::kAdmin);
    _sendCommandToAllRecipients(executor, opts);
}

void ReshardingCoordinator::_tellAllDonorsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    async_rpc::GenericArgs args;
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args, kMajorityWriteConcern);
    auto opts = createFlushReshardingStateChangeOptions(_coordinatorDoc.getSourceNss(),
                                                        _coordinatorDoc.getReshardingUUID(),
                                                        **executor,
                                                        _ctHolder->getStepdownToken(),
                                                        args);
    opts->cmd.setDbName(DatabaseName::kAdmin);
    _sendCommandToAllDonors(executor, opts);
}

void ReshardingCoordinator::_tellAllParticipantsToCommit(
    const NamespaceString& nss, const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opts = createShardsvrCommitReshardCollectionOptions(
        nss, _coordinatorDoc.getReshardingUUID(), **executor, _ctHolder->getStepdownToken(), {});
    opts->cmd.setDbName(DatabaseName::kAdmin);
    _sendCommandToAllParticipants(executor, opts);
}

void ReshardingCoordinator::_tellAllParticipantsToAbort(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, bool isUserAborted) {
    ShardsvrAbortReshardCollection abortCmd(_coordinatorDoc.getReshardingUUID(), isUserAborted);
    abortCmd.setDbName(DatabaseName::kAdmin);
    async_rpc::GenericArgs args;
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrAbortReshardCollection>>(
        **executor, _ctHolder->getStepdownToken(), abortCmd, args);
    _sendCommandToAllParticipants(executor, opts);
}

void ReshardingCoordinator::_updateChunkImbalanceMetrics(const NamespaceString& nss) {
    auto cancellableOpCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto opCtx = cancellableOpCtx.get();

    try {
        auto [routingInfo, _] = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getTrackedCollectionRoutingInfoWithPlacementRefresh(
                opCtx, nss));

        const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
        const auto collectionZones =
            uassertStatusOK(catalogClient->getTagsForCollection(opCtx, nss));

        const auto& keyPattern = routingInfo.getShardKeyPattern().getKeyPattern();

        ZoneInfo zoneInfo;
        for (const auto& tag : collectionZones) {
            uassertStatusOK(zoneInfo.addRangeToZone(
                ZoneRange(keyPattern.extendRangeBound(tag.getMinKey(), false),
                          keyPattern.extendRangeBound(tag.getMaxKey(), false),
                          tag.getTag())));
        }

        const auto allShardsWithOpTime = uassertStatusOK(
            catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern));

        auto imbalanceCount =
            getMaxChunkImbalanceCount(routingInfo, allShardsWithOpTime.value, zoneInfo);

        _metrics->setLastOpEndingChunkImbalance(imbalanceCount);
    } catch (const DBException& ex) {
        LOGV2_WARNING(5543000,
                      "Encountered error while trying to update resharding chunk imbalance metrics",
                      logAttrs(nss),
                      "error"_attr = redact(ex.toStatus()));
    }
}

void ReshardingCoordinator::_logStatsOnCompletion(bool success) {
    BSONObjBuilder builder;
    BSONObjBuilder statsBuilder;
    builder.append("uuid", _coordinatorDoc.getReshardingUUID().toBSON());
    builder.append("status", success ? "success" : "failed");
    statsBuilder.append("ns", toStringForLogging(_coordinatorDoc.getSourceNss()));
    statsBuilder.append("sourceUUID", _coordinatorDoc.getSourceUUID().toBSON());
    statsBuilder.append("newUUID", _coordinatorDoc.getReshardingUUID().toBSON());
    if (_coordinatorDoc.getSourceKey()) {
        statsBuilder.append("oldShardKey", *_coordinatorDoc.getSourceKey());
    }
    statsBuilder.append("newShardKey", _coordinatorDoc.getReshardingKey().toBSON());
    if (_coordinatorDoc.getStartTime()) {
        auto startTime = *_coordinatorDoc.getStartTime();
        statsBuilder.append("startTime", startTime);

        auto endTime = getCurrentTime();
        statsBuilder.append("endTime", endTime);

        auto elapsedMillis = (endTime - startTime).count();
        statsBuilder.append("operationDuration", elapsedMillis);
    } else {
        statsBuilder.append("endTime", getCurrentTime());
    }
    _metrics->reportOnCompletion(&statsBuilder);

    int64_t totalWritesDuringCriticalSection = 0;
    for (auto shard : _coordinatorDoc.getDonorShards()) {
        totalWritesDuringCriticalSection +=
            shard.getMutableState().getWritesDuringCriticalSection().value_or(0);
    }
    statsBuilder.append("writesDuringCriticalSection", totalWritesDuringCriticalSection);

    for (auto shard : _coordinatorDoc.getRecipientShards()) {
        BSONObjBuilder shardBuilder;
        shardBuilder.append("bytesCopied", shard.getMutableState().getBytesCopied().value_or(0));
        shardBuilder.append("oplogFetched", shard.getMutableState().getOplogFetched().value_or(0));
        shardBuilder.append("oplogApplied", shard.getMutableState().getOplogApplied().value_or(0));
        statsBuilder.append(shard.getId(), shardBuilder.obj());
    }

    int64_t totalDocuments = 0;
    int64_t docSize = 0;
    int64_t totalIndexes = 0;
    for (auto shard : _coordinatorDoc.getRecipientShards()) {
        totalDocuments += shard.getMutableState().getTotalNumDocuments().value_or(0);
        docSize += shard.getMutableState().getTotalDocumentSize().value_or(0);
        if (shard.getMutableState().getNumOfIndexes().value_or(0) > totalIndexes) {
            totalIndexes = shard.getMutableState().getNumOfIndexes().value_or(0);
        }
    }
    statsBuilder.append("numberOfTotalDocuments", totalDocuments);
    statsBuilder.append("averageDocSize", totalDocuments > 0 ? (docSize / totalDocuments) : 0);
    statsBuilder.append("numberOfIndexes", totalIndexes);

    statsBuilder.append("numberOfSourceShards", (int64_t)(_coordinatorDoc.getDonorShards().size()));

    auto numDestinationShards = 0;
    if (const auto& shardDistribution = _coordinatorDoc.getShardDistribution()) {
        std::set<ShardId> destinationShards;
        for (const auto& shardDist : *shardDistribution) {
            destinationShards.emplace(shardDist.getShard());
        }
        numDestinationShards = destinationShards.size();
    } else {
        numDestinationShards = _coordinatorDoc.getRecipientShards().size();
    }
    statsBuilder.append("numberOfDestinationShards", (int64_t)numDestinationShards);

    builder.append("statistics", statsBuilder.obj());
    LOGV2(7763800, "Resharding complete", "info"_attr = builder.obj());
}

}  // namespace mongo
