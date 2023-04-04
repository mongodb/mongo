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

#include "mongo/s/write_ops/batch_write_op.h"

#include <numeric>

#include "mongo/base/error_codes.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/s/client/num_hosts_targeted_metrics.h"
#include "mongo/s/collection_uuid_mismatch.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

namespace mongo {
namespace {

struct WriteErrorComp {
    bool operator()(const write_ops::WriteError& errorA,
                    const write_ops::WriteError& errorB) const {
        return errorA.getIndex() < errorB.getIndex();
    }
};

/**
 * Returns a new write concern that has the copy of every field from the original
 * document but with a w set to 1. This is intended for upgrading { w: 0 } write
 * concern to { w: 1 }.
 */
BSONObj upgradeWriteConcern(const BSONObj& origWriteConcern) {
    BSONObjIterator iter(origWriteConcern);
    BSONObjBuilder newWriteConcern;

    while (iter.more()) {
        BSONElement elem(iter.next());

        if (strncmp(elem.fieldName(), "w", 2) == 0) {
            newWriteConcern.append("w", 1);
        } else {
            newWriteConcern.append(elem);
        }
    }

    return newWriteConcern.obj();
}

/**
 * Helper to determine whether a number of targeted writes require a new targeted batch.
 */
bool isNewBatchRequiredOrdered(const std::vector<std::unique_ptr<TargetedWrite>>& writes,
                               const TargetedBatchMap& batchMap) {
    for (auto&& write : writes) {
        if (batchMap.find(write->endpoint.shardName) == batchMap.end()) {
            return true;
        }
    }

    return false;
}

/**
 * Helper to determine whether a shard is already targeted with a different shardVersion, which
 * necessitates a new batch. This happens when a batch write includes a multi target write and
 * a single target write.
 */
bool isNewBatchRequiredUnordered(
    const NamespaceString& nss,
    const std::vector<std::unique_ptr<TargetedWrite>>& writes,
    const std::map<NamespaceString, std::set<ShardId>>& nsShardIdMap,
    const std::map<NamespaceString, std::set<const ShardEndpoint*, EndpointComp>>& nsEndpointMap) {
    auto endpointSetIt = nsEndpointMap.find(nss);
    if (endpointSetIt == nsEndpointMap.end()) {
        // We haven't targeted this namespace yet.
        return false;
    }

    for (auto&& write : writes) {
        if (endpointSetIt->second.find(&write->endpoint) == endpointSetIt->second.end()) {
            // This is a new endpoint for this namespace.
            auto shardIdSetIt = nsShardIdMap.find(nss);
            invariant(shardIdSetIt != nsShardIdMap.end());
            if (shardIdSetIt->second.find(write->endpoint.shardName) !=
                shardIdSetIt->second.end()) {
                // And because we have targeted this shardId for this namespace before, this implies
                // a shard is already targeted under a different endpoint/shardVersion, necessitates
                // a new batch.
                return true;
            }
        }
    }
    return false;
}

/**
 * Helper to determine whether a number of targeted writes require a new targeted batch.
 */
bool wouldMakeBatchesTooBig(const std::vector<std::unique_ptr<TargetedWrite>>& writes,
                            int writeSizeBytes,
                            const TargetedBatchMap& batchMap) {
    for (auto&& write : writes) {
        TargetedBatchMap::const_iterator it = batchMap.find(write->endpoint.shardName);
        if (it == batchMap.end()) {
            // If this is the first item in the batch, it can't be too big
            continue;
        }

        if (it->second->getNumOps() >= write_ops::kMaxWriteBatchSize) {
            // Too many items in batch
            return true;
        }

        if (it->second->getEstimatedSizeBytes() + writeSizeBytes > BSONObjMaxUserSize) {
            // Batch would be too big
            return true;
        }
    }

    return false;
}

/**
 * Gets an estimated size of how much the particular write operation would add to the size of the
 * batch.
 */
int getWriteSizeBytes(const WriteOp& writeOp) {
    const BatchItemRef& item = writeOp.getWriteItem();
    const BatchedCommandRequest::BatchType batchType = item.getOpType();

    using UpdateOpEntry = write_ops::UpdateOpEntry;
    using DeleteOpEntry = write_ops::DeleteOpEntry;

    if (batchType == BatchedCommandRequest::BatchType_Insert) {
        return item.getDocument().objsize();
    } else if (batchType == BatchedCommandRequest::BatchType_Update) {
        // Note: Be conservative here - it's okay if we send slightly too many batches.
        const auto& update = item.getUpdate();
        auto estSize = write_ops::getUpdateSizeEstimate(
            update.getQ(),
            update.getU(),
            update.getC(),
            update.getUpsertSupplied().has_value(),
            update.getCollation(),
            update.getArrayFilters(),
            update.getHint(),
            update.getSampleId(),
            update.getAllowShardKeyUpdatesWithoutFullShardKeyInQuery());

        // When running a debug build, verify that estSize is at least the BSON serialization size.
        dassert(estSize >= update.toBSON().objsize());
        return estSize;
    } else if (batchType == BatchedCommandRequest::BatchType_Delete) {
        const auto& deleteOp = item.getDelete();
        auto estSize = write_ops::getDeleteSizeEstimate(
            deleteOp.getQ(), deleteOp.getCollation(), deleteOp.getHint(), deleteOp.getSampleId());

        // When running a debug build, verify that estSize is at least the BSON serialization size.
        dassert(estSize >= deleteOp.toBSON().objsize());
        return estSize;
    }

    MONGO_UNREACHABLE;
}

/**
 * Given *either* a batch error or an array of per-item errors, copies errors we're interested in
 * into a TrackedErrorMap
 */
void trackErrors(const ShardEndpoint& endpoint,
                 const std::vector<write_ops::WriteError>& itemErrors,
                 TrackedErrors* trackedErrors) {
    for (auto&& error : itemErrors) {
        if (trackedErrors->isTracking(error.getStatus().code())) {
            trackedErrors->addError(ShardError(endpoint, error));
        }
    }
}

/**
 * Attempts to populate the actualCollection field of a CollectionUUIDMismatch error if it is not
 * populated already, contacting the primary shard if necessary.
 */
void populateCollectionUUIDMismatch(OperationContext* opCtx,
                                    write_ops::WriteError* error,
                                    boost::optional<std::string>* actualCollection,
                                    bool* hasContactedPrimaryShard) {
    if (error->getStatus() != ErrorCodes::CollectionUUIDMismatch) {
        return;
    }

    auto info = error->getStatus().extraInfo<CollectionUUIDMismatchInfo>();
    if (info->actualCollection()) {
        return;
    }

    if (*actualCollection) {
        error->setStatus({CollectionUUIDMismatchInfo{info->dbName(),
                                                     info->collectionUUID(),
                                                     info->expectedCollection(),
                                                     **actualCollection},
                          error->getStatus().reason()});
        return;
    }

    if (*hasContactedPrimaryShard) {
        return;
    }

    error->setStatus(populateCollectionUUIDMismatch(opCtx, error->getStatus()));
    if (error->getStatus() == ErrorCodes::CollectionUUIDMismatch) {
        *hasContactedPrimaryShard = true;
        if (auto& populatedActualCollection =
                error->getStatus().extraInfo<CollectionUUIDMismatchInfo>()->actualCollection()) {
            *actualCollection = populatedActualCollection;
        }
    }
}

int getEncryptionInformationSize(const BatchedCommandRequest& req) {
    if (!req.getWriteCommandRequestBase().getEncryptionInformation()) {
        return 0;
    }
    return req.getWriteCommandRequestBase().getEncryptionInformation().value().toBSON().objsize();
}

}  // namespace

StatusWith<bool> targetWriteOps(OperationContext* opCtx,
                                std::vector<WriteOp>& writeOps,
                                bool ordered,
                                bool recordTargetErrors,
                                GetTargeterFn getTargeterFn,
                                GetWriteSizeFn getWriteSizeFn,
                                TargetedBatchMap& batchMap) {
    //
    // Targeting of unordered batches is fairly simple - each remaining write op is targeted,
    // and each of those targeted writes are grouped into a batch for a particular shard
    // endpoint.
    //
    // Targeting of ordered batches is a bit more complex - to respect the ordering of the
    // batch, we can only send:
    // A) a single targeted batch to one shard endpoint
    // B) multiple targeted batches, but only containing targeted writes for a single write op
    //
    // This means that any multi-shard write operation must be targeted and sent one-by-one.
    // Subsequent single-shard write operations can be batched together if they go to the same
    // place.
    //
    // Ex: ShardA : { skey : a->k }, ShardB : { skey : k->z }
    //
    // Ordered insert batch of: [{ skey : a }, { skey : b }, { skey : x }]
    // broken into:
    //  [{ skey : a }, { skey : b }],
    //  [{ skey : x }]
    //
    // Ordered update Batch of :
    //  [{ skey : a }{ $push },
    //   { skey : b }{ $push },
    //   { skey : [c, x] }{ $push },
    //   { skey : y }{ $push },
    //   { skey : z }{ $push }]
    // broken into:
    //  [{ skey : a }, { skey : b }],
    //  [{ skey : [c,x] }],
    //  [{ skey : y }, { skey : z }]
    //

    bool isWriteWithoutShardKeyOrId = false;

    std::map<NamespaceString, std::set<const ShardEndpoint*, EndpointComp>> nsEndpointMap;
    std::map<NamespaceString, std::set<ShardId>> nsShardIdMap;

    for (auto& writeOp : writeOps) {
        // Only target Ready op.
        if (writeOp.getWriteState() != WriteOpState_Ready)
            continue;

        // If we got a write without shard key in the previous iteration, it should be sent in its
        // own batch.
        if (isWriteWithoutShardKeyOrId) {
            break;
        }

        const auto& targeter = getTargeterFn(writeOp);
        std::vector<std::unique_ptr<TargetedWrite>> writes;
        auto targetStatus = [&] {
            try {
                writeOp.targetWrites(opCtx, targeter, &writes);
                return Status::OK();
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        }();

        if (!targetStatus.isOK()) {
            write_ops::WriteError targetError(0, targetStatus);

            auto cancelBatches = [&](const write_ops::WriteError& why) {
                for (TargetedBatchMap::iterator it = batchMap.begin(); it != batchMap.end();) {
                    for (auto&& write : it->second->getWrites()) {
                        // NOTE: We may repeatedly cancel a write op here, but that's fast and we
                        // want to cancel before erasing the TargetedWrite* (which owns the
                        // cancelled targeting info) for reporting reasons.
                        writeOps[write->writeOpRef.first].cancelWrites(&why);
                    }

                    it = batchMap.erase(it);
                }
                dassert(batchMap.empty());
            };

            if (TransactionRouter::get(opCtx)) {
                writeOp.setOpError(targetError);

                // Cleanup all the writes we have targetted in this call so far since we are going
                // to abort the entire transaction.
                cancelBatches(targetError);

                return targetStatus;
            } else if (!recordTargetErrors) {
                // Cancel current batch state with an error
                cancelBatches(targetError);
                return targetStatus;
            } else if (!ordered || batchMap.empty()) {
                // Record an error for this batch

                writeOp.setOpError(targetError);

                if (ordered)
                    return isWriteWithoutShardKeyOrId;

                continue;
            } else {
                dassert(ordered && !batchMap.empty());

                // Send out what we have, but don't record an error yet, since there may be an error
                // in the writes before this point.
                writeOp.cancelWrites(&targetError);
                break;
            }
        }

        // If writes are ordered and we have a targeted endpoint, make sure we don't need to send
        // these targeted writes to any other endpoints.
        if (ordered && !batchMap.empty()) {
            dassert(batchMap.size() == 1u);
            if (isNewBatchRequiredOrdered(writes, batchMap)) {
                writeOp.cancelWrites(nullptr);
                break;
            }
        }

        const auto estWriteSizeBytes = getWriteSizeFn(writeOp);

        if (wouldMakeBatchesTooBig(writes, estWriteSizeBytes, batchMap)) {
            invariant(!batchMap.empty());
            writeOp.cancelWrites(nullptr);
            break;
        }

        // If writes are unordered and we already have targeted endpoints, make sure we don't target
        // the same shard with a different shardVersion.
        if (!ordered &&
            isNewBatchRequiredUnordered(targeter.getNS(), writes, nsShardIdMap, nsEndpointMap)) {
            writeOp.cancelWrites(nullptr);
            break;
        }

        // Check if an updateOne or deleteOne necessitates using the two phase write in the case
        // where the query does not contain a shard key or _id to target by.
        if (auto writeItem = writeOp.getWriteItem();
            writeItem.getOpType() == BatchedCommandRequest::BatchType_Update ||
            writeItem.getOpType() == BatchedCommandRequest::BatchType_Delete) {

            bool isMultiWrite = false;
            BSONObj query;
            BSONObj collation;
            bool isUpsert = false;

            if (writeItem.getOpType() == BatchedCommandRequest::BatchType_Update) {
                auto updateReq = writeItem.getUpdate();
                isMultiWrite = updateReq.getMulti();
                query = updateReq.getQ();
                collation = updateReq.getCollation().value_or(BSONObj());
                isUpsert = updateReq.getUpsert();
            } else {
                auto deleteReq = writeItem.getDelete();
                isMultiWrite = deleteReq.getMulti();
                query = deleteReq.getQ();
                collation = deleteReq.getCollation().value_or(BSONObj());
            }

            if (!isMultiWrite &&
                write_without_shard_key::useTwoPhaseProtocol(opCtx,
                                                             targeter.getNS(),
                                                             true /* isUpdateOrDelete */,
                                                             isUpsert,
                                                             query,
                                                             collation)) {

                // Writes without shard key should be in their own batch.
                if (!batchMap.empty()) {
                    writeOp.cancelWrites(nullptr);
                    break;
                } else {
                    isWriteWithoutShardKeyOrId = true;
                }
            };
        }

        // Targeting went ok, add to appropriate TargetedBatch
        for (auto&& write : writes) {
            const auto& shardId = write->endpoint.shardName;
            TargetedBatchMap::iterator batchIt = batchMap.find(shardId);
            if (batchIt == batchMap.end()) {
                auto newBatch = std::make_unique<TargetedWriteBatch>(shardId);
                batchIt = batchMap.emplace(shardId, std::move(newBatch)).first;
            }

            nsEndpointMap[targeter.getNS()].insert(&write->endpoint);
            nsShardIdMap[targeter.getNS()].insert(shardId);

            batchIt->second->addWrite(std::move(write), estWriteSizeBytes);
        }

        // Relinquish ownership of TargetedWrites, now the TargetedBatches own them
        writes.clear();

        // Break if we're ordered and we have more than one endpoint - later writes cannot be
        // enforced as ordered across multiple shard endpoints.
        if (ordered && batchMap.size() > 1u)
            break;
    }

    return isWriteWithoutShardKeyOrId;
}

BatchWriteOp::BatchWriteOp(OperationContext* opCtx, const BatchedCommandRequest& clientRequest)
    : _opCtx(opCtx),
      _clientRequest(clientRequest),
      _batchTxnNum(_opCtx->getTxnNumber()),
      _inTransaction(bool(TransactionRouter::get(opCtx))),
      _isRetryableWrite(opCtx->isRetryableWrite()) {
    _writeOps.reserve(_clientRequest.sizeWriteOps());

    for (size_t i = 0; i < _clientRequest.sizeWriteOps(); ++i) {
        _writeOps.emplace_back(BatchItemRef(&_clientRequest, i), _inTransaction);
    }
}

StatusWith<bool> BatchWriteOp::targetBatch(const NSTargeter& targeter,
                                           bool recordTargetErrors,
                                           TargetedBatchMap* targetedBatches) {
    const bool ordered = _clientRequest.getWriteCommandRequestBase().getOrdered();

    auto targetStatus = targetWriteOps(
        _opCtx,
        _writeOps,
        ordered,
        recordTargetErrors,
        // getTargeterFn:
        [&](const WriteOp& writeOp) -> const NSTargeter& { return targeter; },
        // getWriteSizeFn:
        [&](const WriteOp& writeOp) {
            // If retryable writes are used, MongoS needs to send an additional array of stmtId(s)
            // corresponding to the statements that got routed to each individual shard, so they
            // need to be accounted in the potential request size so it does not exceed the max BSON
            // size.
            //
            // The constant 4 is chosen as the size of the BSON representation of the stmtId.
            const int writeSizeBytes = getWriteSizeBytes(writeOp) +
                getEncryptionInformationSize(_clientRequest) +
                write_ops::kWriteCommandBSONArrayPerElementOverheadBytes +
                (_batchTxnNum ? write_ops::kWriteCommandBSONArrayPerElementOverheadBytes + 4 : 0);

            // For unordered writes, the router must return an entry for each failed write. This
            // constant is a pessimistic attempt to ensure that if a request to a shard hits
            // "retargeting needed" error and has to return number of errors equivalent to the
            // number of writes in the batch, the response size will not exceed the max BSON size.
            //
            // The constant of 272 is chosen as an approximation of the size of the BSON
            // representation of the StaleConfigInfo (which contains the shard id) and the adjacent
            // error message.
            const int errorResponsePotentialSizeBytes =
                ordered ? 0 : write_ops::kWriteCommandBSONArrayPerElementOverheadBytes + 272;
            return std::max(writeSizeBytes, errorResponsePotentialSizeBytes);
        },
        *targetedBatches);

    if (!targetStatus.isOK()) {
        return targetStatus;
    }

    _nShardsOwningChunks = targeter.getNShardsOwningChunks();

    return targetStatus;
}

BatchedCommandRequest BatchWriteOp::buildBatchRequest(
    const TargetedWriteBatch& targetedBatch,
    const NSTargeter& targeter,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery) const {
    const auto batchType = _clientRequest.getBatchType();

    boost::optional<std::vector<int32_t>> stmtIdsForOp;
    if (_isRetryableWrite) {
        stmtIdsForOp.emplace();
    }

    boost::optional<std::vector<BSONObj>> insertDocs;
    boost::optional<std::vector<write_ops::UpdateOpEntry>> updates;
    boost::optional<std::vector<write_ops::DeleteOpEntry>> deletes;

    for (auto&& targetedWrite : targetedBatch.getWrites()) {
        const WriteOpRef& writeOpRef = targetedWrite->writeOpRef;

        switch (batchType) {
            case BatchedCommandRequest::BatchType_Insert:
                if (!insertDocs)
                    insertDocs.emplace();
                insertDocs->emplace_back(
                    _clientRequest.getInsertRequest().getDocuments().at(writeOpRef.first));
                break;
            case BatchedCommandRequest::BatchType_Update:
                if (!updates)
                    updates.emplace();
                updates->emplace_back(
                    _clientRequest.getUpdateRequest().getUpdates().at(writeOpRef.first));
                updates->back().setSampleId(targetedWrite->sampleId);

                // If we are using the two phase write protocol introduced in PM-1632, we allow
                // shard key updates without specifying the full shard key in the query if we
                // execute the update in a retryable write/transaction.
                if (allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
                    uassert(
                        ErrorCodes::InvalidOptions,
                        "$_allowShardKeyUpdatesWithoutFullShardKeyInQuery is an internal parameter",
                        !updates->back().getAllowShardKeyUpdatesWithoutFullShardKeyInQuery());
                    updates->back().setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
                        allowShardKeyUpdatesWithoutFullShardKeyInQuery);
                }
                break;
            case BatchedCommandRequest::BatchType_Delete:
                if (!deletes)
                    deletes.emplace();
                deletes->emplace_back(
                    _clientRequest.getDeleteRequest().getDeletes().at(writeOpRef.first));
                deletes->back().setSampleId(targetedWrite->sampleId);
                break;
            default:
                MONGO_UNREACHABLE;
        }

        if (stmtIdsForOp) {
            stmtIdsForOp->push_back(write_ops::getStmtIdForWriteAt(
                _clientRequest.getWriteCommandRequestBase(), writeOpRef.first));
        }
    }

    BatchedCommandRequest request([&] {
        switch (batchType) {
            case BatchedCommandRequest::BatchType_Insert:
                return BatchedCommandRequest([&] {
                    write_ops::InsertCommandRequest insertOp(targeter.getNS());
                    insertOp.setDocuments(std::move(*insertDocs));
                    return insertOp;
                }());
            case BatchedCommandRequest::BatchType_Update: {
                return BatchedCommandRequest([&] {
                    write_ops::UpdateCommandRequest updateOp(targeter.getNS());
                    updateOp.setUpdates(std::move(*updates));
                    // Each child batch inherits its let params/runtime constants from the parent
                    // batch.
                    updateOp.setLegacyRuntimeConstants(_clientRequest.getLegacyRuntimeConstants());
                    updateOp.setLet(_clientRequest.getLet());
                    return updateOp;
                }());
            }
            case BatchedCommandRequest::BatchType_Delete:
                return BatchedCommandRequest([&] {
                    write_ops::DeleteCommandRequest deleteOp(targeter.getNS());
                    deleteOp.setDeletes(std::move(*deletes));
                    // Each child batch inherits its let params from the parent batch.
                    deleteOp.setLet(_clientRequest.getLet());
                    deleteOp.setLegacyRuntimeConstants(_clientRequest.getLegacyRuntimeConstants());
                    return deleteOp;
                }());
        }
        MONGO_UNREACHABLE;
    }());

    request.setWriteCommandRequestBase([&] {
        write_ops::WriteCommandRequestBase wcb;

        wcb.setBypassDocumentValidation(
            _clientRequest.getWriteCommandRequestBase().getBypassDocumentValidation());
        wcb.setOrdered(_clientRequest.getWriteCommandRequestBase().getOrdered());
        wcb.setCollectionUUID(_clientRequest.getWriteCommandRequestBase().getCollectionUUID());

        wcb.setEncryptionInformation(
            _clientRequest.getWriteCommandRequestBase().getEncryptionInformation());

        if (targeter.isShardedTimeSeriesBucketsNamespace() &&
            !_clientRequest.getNS().isTimeseriesBucketsCollection()) {
            wcb.setIsTimeseriesNamespace(true);
        }

        if (_isRetryableWrite) {
            wcb.setStmtIds(std::move(stmtIdsForOp));
        }

        return wcb;
    }());

    // For BatchWriteOp, all writes in the batch should share the same endpoint since they target
    // the same shard and namespace. So we just use the endpoint from the first write.
    const auto& endpoint = targetedBatch.getWrites()[0]->endpoint;

    auto shardVersion = endpoint.shardVersion;
    if (shardVersion)
        request.setShardVersion(*shardVersion);

    auto dbVersion = endpoint.databaseVersion;
    if (dbVersion)
        request.setDbVersion(*dbVersion);

    if (_clientRequest.hasWriteConcern()) {
        if (_clientRequest.isVerboseWC()) {
            request.setWriteConcern(_clientRequest.getWriteConcern());
        } else {
            // Mongos needs to send to the shard with w > 0 so it will be able to see the
            // writeErrors
            request.setWriteConcern(upgradeWriteConcern(_clientRequest.getWriteConcern()));
        }
    } else if (!TransactionRouter::get(_opCtx)) {
        // Apply the WC from the opCtx (except if in a transaction).
        request.setWriteConcern(_opCtx->getWriteConcern().toBSON());
    }

    return request;
}

void BatchWriteOp::noteBatchResponse(const TargetedWriteBatch& targetedBatch,
                                     const BatchedCommandResponse& response,
                                     TrackedErrors* trackedErrors) {
    if (!response.getOk()) {
        write_ops::WriteError error(0, response.getTopLevelStatus());

        // Treat command errors exactly like other failures of the batch.
        //
        // Note that no errors will be tracked from these failures - as-designed.
        noteBatchError(targetedBatch, error);
        return;
    }

    // Stop tracking targeted batch
    _targeted.erase(&targetedBatch);

    // Increment stats for this batch
    _incBatchStats(response);

    //
    // Assign errors to particular items.
    // Write Concern errors are stored and handled later.
    //

    // Special handling for write concern errors, save for later
    if (response.isWriteConcernErrorSet()) {
        // For BatchWriteOp, all writes in the batch should share the same endpoint since they
        // target the same shard and namespace. So we just use the endpoint from the first write.
        _wcErrors.emplace_back(targetedBatch.getWrites()[0]->endpoint,
                               *response.getWriteConcernError());
    }

    std::vector<write_ops::WriteError> itemErrors;

    // Handle batch and per-item errors
    if (response.isErrDetailsSet()) {
        // Per-item errors were set
        itemErrors.insert(
            itemErrors.begin(), response.getErrDetails().begin(), response.getErrDetails().end());

        // Sort per-item errors by index
        std::sort(itemErrors.begin(), itemErrors.end(), WriteErrorComp());
    }

    //
    // Go through all pending responses of the op and sorted remote responses, populate errors
    // This will either set all errors to the batch error or apply per-item errors as-needed
    //
    // If the batch is ordered, cancel all writes after the first error for retargeting.
    //

    const bool ordered = _clientRequest.getWriteCommandRequestBase().getOrdered();

    auto itemErrorIt = itemErrors.begin();
    int index = 0;
    write_ops::WriteError* lastError = nullptr;
    for (auto&& write : targetedBatch.getWrites()) {
        WriteOp& writeOp = _writeOps[write->writeOpRef.first];
        invariant(writeOp.getWriteState() == WriteOpState_Pending);

        // See if we have an error for the write
        write_ops::WriteError* writeError = nullptr;

        if (itemErrorIt != itemErrors.end() && itemErrorIt->getIndex() == index) {
            // We have an per-item error for this write op's index
            writeError = &(*itemErrorIt);
            ++itemErrorIt;
        }

        // Finish the response (with error, if needed)
        if (!writeError) {
            if (!ordered || !lastError) {
                writeOp.noteWriteComplete(*write);
            } else {
                // We didn't actually apply this write - cancel so we can retarget
                dassert(writeOp.getNumTargeted() == 1u);
                writeOp.cancelWrites(lastError);
            }
        } else {
            writeOp.noteWriteError(*write, *writeError);
            lastError = writeError;
        }
        ++index;
    }

    // Track errors we care about, whether batch or individual errors
    if (nullptr != trackedErrors) {
        // For BatchWriteOp, all writes in the batch should share the same endpoint since they
        // target the same shard and namespace. So we just use the endpoint from the first write.
        trackErrors(targetedBatch.getWrites()[0]->endpoint, itemErrors, trackedErrors);
    }

    // Track upserted ids if we need to
    if (response.isUpsertDetailsSet()) {
        const std::vector<BatchedUpsertDetail*>& upsertedIds = response.getUpsertDetails();
        for (std::vector<BatchedUpsertDetail*>::const_iterator it = upsertedIds.begin();
             it != upsertedIds.end();
             ++it) {
            // The child upserted details don't have the correct index for the full batch
            const BatchedUpsertDetail* childUpsertedId = *it;

            // Work backward from the child batch item index to the batch item index
            int childBatchIndex = childUpsertedId->getIndex();
            int batchIndex = targetedBatch.getWrites()[childBatchIndex]->writeOpRef.first;

            // Push the upserted id with the correct index into the batch upserted ids
            auto upsertedId = std::make_unique<BatchedUpsertDetail>();
            upsertedId->setIndex(batchIndex);
            upsertedId->setUpsertedID(childUpsertedId->getUpsertedID());
            _upsertedIds.push_back(std::move(upsertedId));
        }
    }
}

void BatchWriteOp::noteBatchError(const TargetedWriteBatch& targetedBatch,
                                  const write_ops::WriteError& error) {
    // Treat errors to get a batch response as failures of the contained writes
    BatchedCommandResponse emulatedResponse;
    emulatedResponse.setStatus(Status::OK());
    emulatedResponse.setN(0);

    const int numErrors = _clientRequest.getWriteCommandRequestBase().getOrdered()
        ? 1
        : targetedBatch.getWrites().size();

    for (int i = 0; i < numErrors; i++) {
        write_ops::WriteError errorClone = error;
        errorClone.setIndex(i);
        emulatedResponse.addToErrDetails(std::move(errorClone));
    }

    noteBatchResponse(targetedBatch, emulatedResponse, nullptr);
}

void BatchWriteOp::abortBatch(const write_ops::WriteError& error) {
    dassert(!isFinished());
    dassert(numWriteOpsIn(WriteOpState_Pending) == 0);

    const size_t numWriteOps = _clientRequest.sizeWriteOps();
    const bool orderedOps = _clientRequest.getWriteCommandRequestBase().getOrdered();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];
        // Can only be called with no outstanding batches
        dassert(writeOp.getWriteState() != WriteOpState_Pending);

        if (writeOp.getWriteState() < WriteOpState_Completed) {
            writeOp.setOpError(error);

            // Only one error if we're ordered
            if (orderedOps)
                break;
        }
    }

    dassert(isFinished());
}

void BatchWriteOp::forgetTargetedBatchesOnTransactionAbortingError() {
    _targeted.clear();
}

bool BatchWriteOp::isFinished() {
    const size_t numWriteOps = _clientRequest.sizeWriteOps();
    const bool orderedOps = _clientRequest.getWriteCommandRequestBase().getOrdered();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];
        if (writeOp.getWriteState() < WriteOpState_Completed)
            return false;
        else if (orderedOps && writeOp.getWriteState() == WriteOpState_Error)
            return true;
    }

    return true;
}

void BatchWriteOp::buildClientResponse(BatchedCommandResponse* batchResp) {
    // Note: we aggressively abandon the batch when encountering errors during transactions, so
    // it can be in a state that is not "finished" even for unordered batches.
    dassert(_inTransaction || isFinished());

    // Result is OK
    batchResp->setStatus(Status::OK());

    // For non-verbose, it's all we need.
    if (!_clientRequest.isVerboseWC()) {
        return;
    }

    //
    // Find all the errors in the batch
    //

    std::vector<WriteOp*> errOps;

    const size_t numWriteOps = _clientRequest.sizeWriteOps();
    for (size_t i = 0; i < numWriteOps; ++i) {
        WriteOp& writeOp = _writeOps[i];

        if (writeOp.getWriteState() == WriteOpState_Error) {
            errOps.push_back(&writeOp);
        }
    }

    //
    // Build the per-item errors.
    //

    if (!errOps.empty()) {
        boost::optional<std::string> collectionUUIDMismatchActualCollection;

        for (std::vector<WriteOp*>::iterator it = errOps.begin(); it != errOps.end(); ++it) {
            WriteOp& writeOp = **it;
            write_ops::WriteError error = writeOp.getOpError();
            auto status = error.getStatus();

            // For CollectionUUIDMismatch error, check if there is a response from a shard that
            // aleady has the actualCollection information. If there is none, make an additional
            // call to the primary shard to fetch this info in case the collection is unsharded or
            // the targeted shard does not own any chunk of the collection with the requested uuid.
            if (!collectionUUIDMismatchActualCollection &&
                status.code() == ErrorCodes::CollectionUUIDMismatch) {
                collectionUUIDMismatchActualCollection =
                    status.extraInfo<CollectionUUIDMismatchInfo>()->actualCollection();
            }

            batchResp->addToErrDetails(std::move(error));
        }

        bool hasContactedPrimaryShard = false;
        for (auto& error : batchResp->getErrDetails()) {
            populateCollectionUUIDMismatch(
                _opCtx, &error, &collectionUUIDMismatchActualCollection, &hasContactedPrimaryShard);
        }
    }

    // Only return a write concern error if everything succeeded (unordered or ordered)
    // OR if something succeeded and we're unordered
    const bool orderedOps = _clientRequest.getWriteCommandRequestBase().getOrdered();
    const bool reportWCError =
        errOps.empty() || (!orderedOps && errOps.size() < _clientRequest.sizeWriteOps());
    if (!_wcErrors.empty() && reportWCError) {
        WriteConcernErrorDetail* error = new WriteConcernErrorDetail;

        // Generate the multi-error message below
        if (_wcErrors.size() == 1) {
            auto status = _wcErrors.front().error.toStatus();
            error->setStatus(status.withReason(str::stream()
                                               << status.reason() << " at "
                                               << _wcErrors.front().endpoint.shardName));
        } else {
            StringBuilder msg;
            msg << "multiple errors reported : ";

            for (auto it = _wcErrors.begin(); it != _wcErrors.end(); ++it) {
                const auto& wcError = *it;
                if (it != _wcErrors.begin()) {
                    msg << " :: and :: ";
                }
                msg << wcError.error.toStatus().toString() << " at " << wcError.endpoint.shardName;
            }

            error->setStatus({ErrorCodes::WriteConcernFailed, msg.str()});
        }
        batchResp->setWriteConcernError(error);
    }

    //
    // Append the upserted ids, if required
    //

    if (_upsertedIds.size() != 0) {
        batchResp->setUpsertDetails(transitional_tools_do_not_use::unspool_vector(_upsertedIds));
    }

    // Stats
    const int nValue = _numInserted + _numUpserted + _numMatched + _numDeleted;
    batchResp->setN(nValue);
    if (_clientRequest.getBatchType() == BatchedCommandRequest::BatchType_Update &&
        _numModified >= 0) {
        batchResp->setNModified(_numModified);
    }
    if (!_retriedStmtIds.empty()) {
        batchResp->setRetriedStmtIds(_retriedStmtIds);
    }
}

int BatchWriteOp::numWriteOpsIn(WriteOpState opState) const {
    // TODO: This could be faster, if we tracked this info explicitly
    return std::accumulate(
        _writeOps.begin(), _writeOps.end(), 0, [opState](int sum, const WriteOp& writeOp) {
            return sum + (writeOp.getWriteState() == opState ? 1 : 0);
        });
}

boost::optional<int> BatchWriteOp::getNShardsOwningChunks() {
    return _nShardsOwningChunks;
}

void BatchWriteOp::_incBatchStats(const BatchedCommandResponse& response) {
    const auto batchType = _clientRequest.getBatchType();

    if (batchType == BatchedCommandRequest::BatchType_Insert) {
        _numInserted += response.getN();
    } else if (batchType == BatchedCommandRequest::BatchType_Update) {
        int numUpserted = 0;
        if (response.isUpsertDetailsSet()) {
            numUpserted = response.sizeUpsertDetails();
        }
        _numMatched += (response.getN() - numUpserted);
        long long numModified = response.getNModified();

        if (numModified >= 0)
            _numModified += numModified;
        else
            _numModified = -1;  // sentinel used to indicate we omit the field downstream

        _numUpserted += numUpserted;
    } else {
        dassert(batchType == BatchedCommandRequest::BatchType_Delete);
        _numDeleted += response.getN();
    }

    if (auto retriedStmtIds = response.getRetriedStmtIds(); !retriedStmtIds.empty()) {
        _retriedStmtIds.insert(_retriedStmtIds.end(), retriedStmtIds.begin(), retriedStmtIds.end());
    }
}

void BatchWriteOp::_cancelBatches(const write_ops::WriteError& why,
                                  TargetedBatchMap&& batchMapToCancel) {
    // Collect all the writeOps that are currently targeted
    for (TargetedBatchMap::iterator it = batchMapToCancel.begin(); it != batchMapToCancel.end();) {
        for (auto&& write : it->second->getWrites()) {
            // NOTE: We may repeatedly cancel a write op here, but that's fast and we want to cancel
            // before erasing the TargetedWrite* (which owns the cancelled targeting info) for
            // reporting reasons.
            _writeOps[write->writeOpRef.first].cancelWrites(&why);
        }

        it = batchMapToCancel.erase(it);
    }
}

void TrackedErrors::startTracking(int errCode) {
    dassert(!isTracking(errCode));
    _errorMap.emplace(errCode, std::vector<ShardError>());
}

bool TrackedErrors::isTracking(int errCode) const {
    return _errorMap.count(errCode) != 0;
}

void TrackedErrors::addError(ShardError error) {
    TrackedErrorMap::iterator seenIt = _errorMap.find(error.error.getStatus().code());
    if (seenIt == _errorMap.end())
        return;
    seenIt->second.emplace_back(std::move(error));
}

const std::vector<ShardError>& TrackedErrors::getErrors(int errCode) const {
    dassert(isTracking(errCode));
    return _errorMap.find(errCode)->second;
}

}  // namespace mongo
