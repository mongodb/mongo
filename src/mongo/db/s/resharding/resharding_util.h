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

#pragma once

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/shard_key_util.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/request_types/reshard_collection_gen.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo {
namespace resharding {

constexpr auto kReshardFinalOpLogType = "reshardFinalOp"_sd;
constexpr auto kReshardProgressMarkOpLogType = "reshardProgressMark"_sd;
static const auto kReshardErrorMaxBytes = 2000;

const WriteConcernOptions kMajorityWriteConcern{
    WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};

struct ParticipantShardsAndChunks {
    std::vector<DonorShardEntry> donorShards;
    std::vector<RecipientShardEntry> recipientShards;
    std::vector<ChunkType> initialChunks;
};

/**
 * Emplaces the 'fetchTimestamp' onto the ClassWithFetchTimestamp if the timestamp has been
 * emplaced inside the boost::optional.
 */
template <typename ClassWithCloneTimestamp>
void emplaceCloneTimestampIfExists(ClassWithCloneTimestamp& c,
                                   boost::optional<Timestamp> cloneTimestamp) {
    if (!cloneTimestamp) {
        return;
    }

    invariant(!cloneTimestamp->isNull());

    if (auto alreadyExistingCloneTimestamp = c.getCloneTimestamp()) {
        invariant(cloneTimestamp == alreadyExistingCloneTimestamp);
    }

    c.setCloneTimestamp(*cloneTimestamp);
}

template <typename ClassWithRelaxed>
void emplaceRelaxedIfExists(ClassWithRelaxed& c, OptionalBool relaxed) {
    if (!relaxed.has_value()) {
        return;
    }

    if (auto alreadyExistingRelaxed = c.getRelaxed()) {
        uassert(ErrorCodes::BadValue,
                "Existing and new values for relaxed are expected to be equal.",
                relaxed == alreadyExistingRelaxed);
    }

    c.setRelaxed(relaxed);
}

template <typename ClassWithOplogBatchTaskCount>
void emplaceOplogBatchTaskCountIfExists(ClassWithOplogBatchTaskCount& c,
                                        boost::optional<std::int64_t> oplogBatchTaskCount) {
    if (!oplogBatchTaskCount) {
        return;
    }

    if (auto alreadyExistingOplogBatchTaskCount = c.getOplogBatchTaskCount()) {
        uassert(ErrorCodes::BadValue,
                "Existing and new values for oplogBatchTaskCount are expected to be equal.",
                oplogBatchTaskCount == alreadyExistingOplogBatchTaskCount);
    }

    c.setOplogBatchTaskCount(*oplogBatchTaskCount);
}

template <class ReshardingDocumentWithApproxCopySize>
void emplaceApproxBytesToCopyIfExists(ReshardingDocumentWithApproxCopySize& document,
                                      boost::optional<ReshardingApproxCopySize> approxCopySize) {
    if (!approxCopySize) {
        return;
    }

    invariant(bool(document.getApproxBytesToCopy()) == bool(document.getApproxDocumentsToCopy()),
              "Expected approxBytesToCopy and approxDocumentsToCopy to either both be set or to"
              " both be unset");

    if (auto alreadyExistingApproxBytesToCopy = document.getApproxBytesToCopy()) {
        invariant(approxCopySize->getApproxBytesToCopy() == *alreadyExistingApproxBytesToCopy,
                  "Expected the existing and the new values for approxBytesToCopy to be equal");
    }

    if (auto alreadyExistingApproxDocumentsToCopy = document.getApproxDocumentsToCopy()) {
        invariant(approxCopySize->getApproxDocumentsToCopy() ==
                      *alreadyExistingApproxDocumentsToCopy,
                  "Expected the existing and the new values for approxDocumentsToCopy to be equal");
    }

    document.setReshardingApproxCopySizeStruct(std::move(*approxCopySize));
}

/**
 * Emplaces the 'minFetchTimestamp' onto the ClassWithFetchTimestamp if the timestamp has been
 * emplaced inside the boost::optional.
 */
template <class ClassWithMinFetchTimestamp>
void emplaceMinFetchTimestampIfExists(ClassWithMinFetchTimestamp& c,
                                      boost::optional<Timestamp> minFetchTimestamp) {
    if (!minFetchTimestamp) {
        return;
    }

    invariant(!minFetchTimestamp->isNull());

    if (auto alreadyExistingMinFetchTimestamp = c.getMinFetchTimestamp()) {
        invariant(minFetchTimestamp == alreadyExistingMinFetchTimestamp);
    }

    c.setMinFetchTimestamp(std::move(minFetchTimestamp));
}

/**
 * Returns a serialized version of the originalError status. If the originalError status exceeds
 * maxErrorBytes, truncates the status and returns it in the errmsg field of a new status with code
 * ErrorCodes::ReshardingCollectionTruncatedError.
 */
BSONObj serializeAndTruncateReshardingErrorIfNeeded(Status originalError);

/**
 * Emplaces the 'abortReason' onto the ClassWithAbortReason if the reason has been emplaced inside
 * the boost::optional. If the 'abortReason' is too large, emplaces a status with
 * ErrorCodes::ReshardCollectionTruncatedError and a truncated version of the 'abortReason' for the
 * errmsg.
 */
template <class ClassWithAbortReason>
void emplaceTruncatedAbortReasonIfExists(ClassWithAbortReason& c,
                                         boost::optional<Status> abortReason) {
    if (!abortReason) {
        return;
    }

    invariant(!abortReason->isOK());

    if (auto alreadyExistingAbortReason = c.getAbortReason()) {
        // If there already is an abortReason, don't overwrite it.
        return;
    }

    auto truncatedAbortReasonObj = serializeAndTruncateReshardingErrorIfNeeded(abortReason.get());
    AbortReason abortReasonStruct;
    abortReasonStruct.setAbortReason(truncatedAbortReasonObj);
    c.setAbortReasonStruct(std::move(abortReasonStruct));
}

/**
 * Extract the abortReason BSONObj into a status.
 */
template <class ClassWithAbortReason>
Status getStatusFromAbortReason(ClassWithAbortReason& c) {
    invariant(c.getAbortReason());
    auto abortReasonObj = c.getAbortReason().get();
    BSONElement codeElement = abortReasonObj["code"];
    BSONElement errmsgElement = abortReasonObj["errmsg"];
    int code = codeElement.numberInt();
    std::string errmsg;
    if (errmsgElement.type() == BSONType::string) {
        errmsg = errmsgElement.String();
    } else if (!errmsgElement.eoo()) {
        errmsg = errmsgElement.toString();
    }
    return Status(ErrorCodes::Error(code), errmsg, abortReasonObj);
}

/**
 * Extracts the ShardId from each Donor/RecipientShardEntry in participantShardEntries.
 */
template <class T>
std::vector<ShardId> extractShardIdsFromParticipantEntries(
    const std::vector<T>& participantShardEntries) {
    std::vector<ShardId> shardIds(participantShardEntries.size());
    std::transform(participantShardEntries.begin(),
                   participantShardEntries.end(),
                   shardIds.begin(),
                   [](const auto& shardEntry) { return shardEntry.getId(); });
    return shardIds;
}

/**
 * Extracts the ShardId from each Donor/RecipientShardEntry in participantShardEntries as a set.
 */
template <class T>
std::set<ShardId> extractShardIdsFromParticipantEntriesAsSet(
    const std::vector<T>& participantShardEntries) {
    std::set<ShardId> shardIds;
    std::transform(participantShardEntries.begin(),
                   participantShardEntries.end(),
                   std::inserter(shardIds, shardIds.end()),
                   [](const auto& shardEntry) { return shardEntry.getId(); });
    return shardIds;
}

/**
 * Helper method to construct a DonorShardEntry with the fields specified.
 */
DonorShardEntry makeDonorShard(ShardId shardId,
                               DonorStateEnum donorState,
                               boost::optional<Timestamp> minFetchTimestamp = boost::none,
                               boost::optional<Status> abortReason = boost::none);

/**
 * Helper method to construct a RecipientShardEntry with the fields specified.
 */
RecipientShardEntry makeRecipientShard(ShardId shardId,
                                       RecipientStateEnum recipientState,
                                       boost::optional<Status> abortReason = boost::none);

/**
 * Assembles the namespace string for the temporary resharding collection based on the source
 * namespace components.
 *
 *      <db>.system.resharding.<existing collection's UUID>
 * or   <db>.system.buckets.resharding.<existing collection's UUID> for a timeseries source ns.
 */
NamespaceString constructTemporaryReshardingNss(const NamespaceString& nss, const UUID& sourceUuid);

/**
 * Asserts that there is not a hole or overlap in the chunks.
 */
void checkForHolesAndOverlapsInChunks(std::vector<ReshardedChunk>& chunks,
                                      const KeyPattern& keyPattern);

/**
 * Validates resharded chunks provided with a reshardCollection cmd. Parses each BSONObj to a valid
 * ReshardedChunk and asserts that each chunk's shardId is associated with an existing entry in
 * the shardRegistry. Then, asserts that there is not a hole or overlap in the chunks.
 */
void validateReshardedChunks(const std::vector<ReshardedChunk>& chunks,
                             OperationContext* opCtx,
                             const KeyPattern& keyPattern);

/**
 * Selects the highest minFetchTimestamp from the list of donors.
 *
 * Throws if not every donor has a minFetchTimestamp.
 */
Timestamp getHighestMinFetchTimestamp(const std::vector<DonorShardEntry>& donorShards);

/**
 * Asserts that there is not an overlap in the zone ranges.
 */
void checkForOverlappingZones(std::vector<ReshardingZoneType>& zones);

/**
 * Create an array of resharding zones from the existing collection. This is used for forced
 * same-key resharding.
 */
std::vector<ReshardingZoneType> getZonesFromExistingCollection(OperationContext* opCtx,
                                                               const NamespaceString& sourceNss);

/**
 * Creates a pipeline that can be serialized into a query for fetching oplog entries. `startAfter`
 * may be `Timestamp::isNull()` to fetch from the beginning of the oplog.
 */
std::unique_ptr<Pipeline> createOplogFetchingPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ReshardingDonorOplogId& startAfter,
    UUID collUUID,
    const ShardId& recipientShard);

/**
 * Returns true if this is a "reshardProgressMark" noop oplog entry on a recipient created after
 * oplog application has started.
 * {
 *   op: "n",
 *   ns: "<database>.<collection>",
 *   ui: <existingUUID>,
 *   o: {msg: "Latest oplog ts from donor's cursor response"},
 *   o2: {type: "reshardProgressMark", createdAfterOplogApplicationStarted: true},
 *   fromMigrate: true,
 * }
 */
bool isProgressMarkOplogAfterOplogApplicationStarted(const repl::OplogEntry& oplog);

/**
 * Returns true if this is a "reshardFinalOp" noop oplog entry on a donor.
 * {
 *   op: "n",
 *   ns: "<database>.<collection>",
 *   ui: <existingUUID>,
 *   destinedRecipient: <recipientShardId>,
 *   o: {msg: "Writes to <database>.<collection> is temporarily blocked for resharding"},
 *   o2: {type: "reshardFinalOp", reshardingUUID: <reshardingUUID>},
 *   fromMigrate: true,
 * }
 */
bool isFinalOplog(const repl::OplogEntry& oplog);
bool isFinalOplog(const repl::OplogEntry& oplog, UUID reshardingUUID);

NamespaceString getLocalOplogBufferNamespace(UUID existingUUID, ShardId donorShardId);

NamespaceString getLocalConflictStashNamespace(UUID existingUUID, ShardId donorShardId);

void doNoopWrite(OperationContext* opCtx, StringData opStr, const NamespaceString& nss);

boost::optional<Milliseconds> estimateRemainingRecipientTime(bool applyingBegan,
                                                             int64_t bytesCopied,
                                                             int64_t bytesToCopy,
                                                             Milliseconds timeSpentCopying,
                                                             int64_t oplogEntriesApplied,
                                                             int64_t oplogEntriesFetched,
                                                             Milliseconds timeSpentApplying);

/**
 * Looks up the StateMachine by namespace of the collection being resharded. If it does not exist,
 * returns boost::none.
 */
template <class Service, class Instance>
std::vector<std::shared_ptr<Instance>> getReshardingStateMachines(OperationContext* opCtx,
                                                                  const NamespaceString& sourceNs) {
    auto service =
        checked_cast<Service*>(repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                                   ->lookupServiceByName(Service::kServiceName));
    auto instances = service->getAllReshardingInstances(opCtx);
    std::vector<std::shared_ptr<Instance>> result;
    for (const auto& genericInstace : instances) {
        auto instance = checked_pointer_cast<Instance>(genericInstace);
        auto metadata = instance->getMetadata();
        if (metadata.getSourceNss() != sourceNs) {
            continue;
        }
        result.emplace_back(std::move(instance));
    }
    return result;
}

/**
 * Validate the shardDistribution parameter in reshardCollection cmd, which should satisfy the
 * following properties:
 * - The shardKeyRanges should be continuous and cover the full data range.
 * - Every shardKeyRange should be on the same key.
 * - A shardKeyRange should either have no min/max or have a min/max pair.
 * - All shardKeyRanges in the array should have the same min/max pattern.
 * Not satisfying the rules above will cause an uassert failure.
 */
void validateShardDistribution(const std::vector<ShardKeyRange>& shardDistribution,
                               OperationContext* opCtx,
                               const ShardKeyPattern& keyPattern);

/**
 * Returns true if the provenance is moveCollection or balancerMoveCollection.
 */
bool isMoveCollection(const boost::optional<ReshardingProvenanceEnum>& provenance);

/**
 * Returns true if the provenance is unshardCollection.
 */
bool isUnshardCollection(const boost::optional<ReshardingProvenanceEnum>& provenance);

/**
 * Helper function to create a thread pool for _markKilledExecutor member of resharding POS.
 */
std::shared_ptr<ThreadPool> makeThreadPoolForMarkKilledExecutor(const std::string& poolName);

boost::optional<Status> coordinatorAbortedError();

/**
 * If 'performVerification' is true, asserts that featureFlagReshardingVerification is enabled.
 */
void validatePerformVerification(const VersionContext& vCtx,
                                 boost::optional<bool> performVerification);
void validatePerformVerification(const VersionContext& vCtx, bool performVerification);

/**
 * Verifies that for each index spec in sourceIndexSpecs, there is an identical spec in
 * localIndexSpecs. Field order does not matter.
 */
template <typename InputIterator1, typename InputIterator2>
void verifyIndexSpecsMatch(InputIterator1 sourceIndexSpecsBegin,
                           InputIterator1 sourceIndexSpecsEnd,
                           InputIterator2 localIndexSpecsBegin,
                           InputIterator2 localIndexSpecsEnd) {
    stdx::unordered_map<std::string, BSONObj> localIndexSpecMap;
    std::transform(
        localIndexSpecsBegin,
        localIndexSpecsEnd,
        std::inserter(localIndexSpecMap, localIndexSpecMap.end()),
        [](const auto& spec) { return std::pair(std::string{spec.getStringField("name")}, spec); });

    UnorderedFieldsBSONObjComparator bsonCmp;
    for (auto it = sourceIndexSpecsBegin; it != sourceIndexSpecsEnd; ++it) {
        auto spec = *it;
        auto specName = std::string{spec.getStringField("name")};
        uassert(9365601,
                str::stream() << "Resharded collection missing source collection index: "
                              << specName,
                localIndexSpecMap.find(specName) != localIndexSpecMap.end());
        uassert(9365602,
                str::stream() << "Resharded collection created non-matching index. Source spec: "
                              << spec << " Resharded collection spec: "
                              << localIndexSpecMap.find(specName)->second,
                bsonCmp.evaluate(spec == localIndexSpecMap.find(specName)->second));
    }
}

ReshardingCoordinatorDocument createReshardingCoordinatorDoc(
    OperationContext* opCtx,
    const ConfigsvrReshardCollection& request,
    const CollectionType& collEntry,
    const NamespaceString& nss,
    const bool& setProvenance);

inline Status validateReshardBlockingWritesO2FieldType(const std::string& value) {
    if (value != kReshardFinalOpLogType) {
        return {ErrorCodes::BadValue,
                str::stream() << "Expected the oplog type to be '" << kReshardFinalOpLogType
                              << "'"};
    }
    return Status::OK();
}

inline Status validateReshardProgressMarkO2FieldType(const std::string& value) {
    if (value != kReshardProgressMarkOpLogType) {
        return {ErrorCodes::BadValue,
                str::stream() << "Expected the oplog type to be '" << kReshardProgressMarkOpLogType
                              << "'"};
    }
    return Status::OK();
}

Date_t getCurrentTime();

boost::optional<ReshardingCoordinatorDocument> tryGetCoordinatorDoc(OperationContext* opCtx,
                                                                    const UUID& reshardingUUID);

ReshardingCoordinatorDocument getCoordinatorDoc(OperationContext* opCtx,
                                                const UUID& reshardingUUID);

// Waits for majority replication of the latest opTime unless token is cancelled.
SemiFuture<void> waitForMajority(const CancellationToken& token,
                                 const CancelableOperationContextFactory& factory);

/**
 * Waits for the replication lag across all voting members to be below the given threshold.
 */
ExecutorFuture<void> waitForReplicationOnVotingMembers(
    std::shared_ptr<executor::TaskExecutor> executor,
    const CancellationToken& cancelToken,
    const CancelableOperationContextFactory& factory,
    std::function<unsigned()> getMaxLagSecs);

/**
 * To be called on a primary only. Returns the amount of time between the last applied optime on the
 * primary and the last majority committed optime.
 */
Milliseconds getMajorityReplicationLag(OperationContext* opCtx);

// Returns the number of indexes on the given namespace or boost::none if the collection does not
// exist.
boost::optional<int> getIndexCount(OperationContext* opCtx,
                                   const CollectionAcquisition& acquisition);


/**
 * Re-calculates the exponential moving average based on the previous average and the current value.
 * Please refer to https://en.wikipedia.org/wiki/Exponential_smoothing for the formula. Throws
 * an error if the smoothing factor is not greater than 0 and less than 1.
 */
double calculateExponentialMovingAverage(double prevAvg, double currVal, double smoothingFactor);

}  // namespace resharding
}  // namespace mongo
