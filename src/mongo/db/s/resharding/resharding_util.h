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

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/str.h"

namespace mongo {
namespace resharding {

constexpr auto kReshardFinalOpLogType = "reshardFinalOp"_sd;
constexpr auto kReshardProgressMark = "reshardProgressMark"_sd;
static const auto kReshardErrorMaxBytes = 2000;

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
    if (errmsgElement.type() == String) {
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
 */
NamespaceString constructTemporaryReshardingNss(StringData db, const UUID& sourceUuid);

/**
 * Gets the recipient shards for a resharding operation.
 */
std::set<ShardId> getRecipientShards(OperationContext* opCtx,
                                     const NamespaceString& reshardNss,
                                     const UUID& reshardingUUID);

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
 * Builds documents to insert into config.tags from zones provided to reshardCollection cmd.
 */
std::vector<BSONObj> buildTagsDocsFromZones(const NamespaceString& tempNss,
                                            const std::vector<ReshardingZoneType>& zones);

/**
 * Creates a pipeline that can be serialized into a query for fetching oplog entries. `startAfter`
 * may be `Timestamp::isNull()` to fetch from the beginning of the oplog.
 */
std::unique_ptr<Pipeline, PipelineDeleter> createOplogFetchingPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ReshardingDonorOplogId& startAfter,
    UUID collUUID,
    const ShardId& recipientShard);

/**
 * Sentinel oplog format:
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

}  // namespace resharding

}  // namespace mongo
