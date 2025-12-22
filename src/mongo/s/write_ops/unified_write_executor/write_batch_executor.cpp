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

#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"

#include "mongo/db/error_labels.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/s/request_types/coordinate_multi_update_gen.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/coordinate_multi_update_util.h"
#include "mongo/s/write_ops/wc_error.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/util/exit.h"

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace unified_write_executor {

namespace {
BatchWriteCommandReply parseBatchWriteCommandReply(const BSONObj& responseData,
                                                   BatchedCommandRequest::BatchType batchType) {
    BatchedCommandResponse reply;
    std::string errMsg;

    if (!reply.parseBSON(responseData, &errMsg)) {
        uassertStatusOK(Status{ErrorCodes::FailedToParse, errMsg});
    }

    return BatchWriteCommandReply::make(reply, batchType);
}

BatchWriteCommandReply parseBatchWriteCommandReplySingleOp(
    const BSONObj& responseData, BatchedCommandRequest::BatchType batchType) {
    // If we're parsing a BatchedCommandResponse for a single op and 'responseData' is empty, that
    // means the two-phase write completed successfully without updating or deleting anything
    // (because nothing matched the filter).
    //
    // In this case, we create a BatchedCommandResponse containing a single response item with an
    // OK status and all counters set to 0.
    if (responseData.isEmpty()) {
        BatchedCommandResponse reply;
        reply.setStatus(Status::OK());
        reply.setN(0);
        if (batchType == BatchedCommandRequest::BatchType_Update) {
            reply.setNModified(0);
        }

        return BatchWriteCommandReply::make(reply, batchType);
    }

    return parseBatchWriteCommandReply(responseData, batchType);
}

BulkWriteCommandReply parseBulkWriteCommandReply(const BSONObj& responseData) {
    return BulkWriteCommandReply::parse(responseData,
                                        IDLParserContext("BulkWriteCommandReply_UnifiedWriteExec"));
}

BulkWriteCommandReply parseBulkWriteCommandReplySingleOp(const BSONObj& responseData,
                                                         const WriteOp& op) {
    // If we're parsing a BulkWriteCommandReply for a single op and 'responseData' is empty, that
    // means the two-phase write completed successfully without updating or deleting anything
    // (because nothing matched the filter).
    //
    // In this case, we create a BulkWriteCommandReply containing a single response item with an
    // OK status and with n=0 (and with nModified=0 if 'writeOp' is an update).
    if (responseData.isEmpty()) {
        BulkWriteReplyItem replyItem(0, Status::OK());
        replyItem.setN(0);
        if (getWriteOpType(op) == WriteType::kUpdate) {
            replyItem.setNModified(0);
        }

        auto cursor =
            BulkWriteCommandResponseCursor(/*cursorId*/ 0,
                                           std::vector<BulkWriteReplyItem>{std::move(replyItem)},
                                           NamespaceString::makeBulkWriteNSS(boost::none));
        return BulkWriteCommandReply(std::move(cursor), 0, 0, 0, 0, 0, 0);
    }

    return parseBulkWriteCommandReply(responseData);
}

write_ops::FindAndModifyCommandReply parseFindAndModifyCommandReply(const BSONObj& responseData) {
    // If we're parsing a FindAndModifyCommandReply and 'responseData' is empty, that means the
    // two-phase write completed successfully without updating or deleting anything (because nothing
    // matched the filter).
    //
    // In this case, we create a FindAndModifyCommandReply with an OK status and n=0.
    if (responseData.isEmpty()) {
        write_ops::FindAndModifyLastError lastError(/*n*/ 0);
        lastError.setUpdatedExisting(false);

        write_ops::FindAndModifyCommandReply reply;
        reply.setLastErrorObject(std::move(lastError));
        return reply;
    }

    return write_ops::FindAndModifyCommandReply::parse(
        responseData, IDLParserContext("FindAndModifyCommandReply_UnifiedWriteExec"));
}

void appendWriteConcern(OperationContext* opCtx, BSONObjBuilder& builder) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    auto writeConcern = getWriteConcernForShardRequest(opCtx);
    // We don't append write concern in a transaction.
    if (writeConcern && !inTransaction) {
        builder.append(WriteConcernOptions::kWriteConcernField, writeConcern->toBSON());
    }
}

bool isTransientTxnError(bool inTransaction,
                         const Status& status,
                         boost::optional<const BSONObj&> responseData = boost::none) {
    if (inTransaction && !status.isOK()) {
        if (responseData) {
            return hasTransientTransactionErrorLabel(
                ErrorReply::parse(*responseData, IDLParserContext("ErrorReply_UnifiedWriteExec")));
        } else {
            return isTransientTransactionError(
                status.code(), /*hasWriteConcernError*/ false, /*isCommitOrAbort*/ false);
        }
    }
    return false;
}

template <typename T>
void filterGenericArgumentsForEmbeddedCommand(OperationContext* opCtx, T& request) {
    // When the command is embedded, we should filter out all the generic arguments other than
    // "rawData", which is need to preserve the semantics on the timeseries bucket collection.
    coordinate_multi_update_util::filterRequestGenericArguments(request.getGenericArguments());
}

template <typename ResponseType, typename RequestType>
NoRetryWriteBatchResponse executeOpInInternalTransaction(OperationContext* opCtx,
                                                         const RequestType& request,
                                                         const WriteOp& writeOp) {
    static_assert(std::is_same_v<RequestType, BatchedCommandRequest> ||
                  std::is_same_v<RequestType, BulkWriteCommandRequest>);
    static_assert(std::is_same_v<ResponseType, BatchWriteCommandReply> ||
                  std::is_same_v<ResponseType, BulkWriteCommandReply>);

    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    txn_api::SyncTransactionWithRetries txn(
        opCtx, executor, /*resourceYielder*/ nullptr, inlineExecutor);

    ResponseType response;
    auto swRes = txn.runNoThrow(
        opCtx, [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            if constexpr (std::is_same_v<RequestType, BatchedCommandRequest>) {
                // The "stmtIds" field has already been added to 'request', so we pass in an empty
                // vector here to avoid adding a duplicate "stmtId"/"stmtIds" field.
                auto crudOpResponse = txnClient.runCRUDOpSync(request, std::vector<StmtId>{});
                // Assert that 'crudOpResponse' does not have a top-level error.
                uassertStatusOK(crudOpResponse.getTopLevelStatus());
                // Convert the BatchedCommandResponse into a BatchWriteCommandReply and store it
                // into 'response'.
                auto batchType = request.getBatchType();
                response = BatchWriteCommandReply::make(std::move(crudOpResponse), batchType);
                return SemiFuture<void>::makeReady();
            } else {
                auto crudOpResponse = txnClient.runCRUDOpSync(request);
                response = std::move(crudOpResponse);
                return SemiFuture<void>::makeReady();
            }
        });

    const bool swResIsOK = swRes.getStatus().isOK();

    auto wce = swResIsOK && !swRes.getValue().wcError.toStatus().isOK()
        ? boost::make_optional(swRes.getValue().wcError)
        : boost::none;

    auto swResponse = swResIsOK && swRes.getValue().cmdStatus.isOK()
        ? StatusWith(std::move(response))
        : StatusWith<ResponseType>(!swResIsOK ? swRes.getStatus() : swRes.getValue().cmdStatus);

    return NoRetryWriteBatchResponse::make(
        std::move(swResponse), std::move(wce), writeOp, inTransaction);
}
}  // namespace

BatchWriteCommandReply BatchWriteCommandReply::make(const BatchedCommandResponse& bcr,
                                                    BatchedCommandRequest::BatchType batchType) {
    tassert(11468103, "Expected OK top-level status", bcr.getTopLevelStatus().isOK());
    tassert(11468104, "Expected 'n' to be non-negative", bcr.getN() >= 0);
    tassert(11468105, "Expected 'nModified' to be non-negative", bcr.getNModified() >= 0);

    unsigned long long n = static_cast<unsigned long long>(bcr.getN());
    unsigned long long nUpserted = bcr.isUpsertDetailsSet() ? bcr.sizeUpsertDetails() : 0;
    unsigned long long nModified =
        bcr.getNModifiedOpt() ? static_cast<unsigned long long>(*bcr.getNModifiedOpt()) : 0;
    tassert(11468106, "Expected 'nUpserted' to be less than or equal to 'n'", nUpserted <= n);

    using BatchWriteReplyItem = std::tuple<int, Status, boost::optional<IDLAnyTypeOwned>>;

    BatchWriteCommandReply reply;
    std::vector<BatchWriteReplyItem> details;

    if (batchType == BatchedCommandRequest::BatchType_Insert) {
        reply.nInserted = n;
    } else if (batchType == BatchedCommandRequest::BatchType_Update) {
        reply.nMatched = n - nUpserted;
        reply.nModified = nModified;
        reply.nUpserted = nUpserted;
    } else if (batchType == BatchedCommandRequest::BatchType_Delete) {
        reply.nDeleted = n;
    }

    if (bcr.isErrDetailsSet()) {
        reply.nErrors = bcr.sizeErrDetails();

        for (const write_ops::WriteError& writeError : bcr.getErrDetails()) {
            details.emplace_back(writeError.getIndex(), writeError.getStatus(), boost::none);
        }
    }
    if (bcr.isUpsertDetailsSet()) {
        for (const BatchedUpsertDetail* detail : bcr.getUpsertDetails()) {
            if (detail->isUpsertedIDSet()) {
                auto upsertedId = IDLAnyTypeOwned(detail->getUpsertedID().firstElement());
                details.emplace_back(detail->getIndex(), Status::OK(), std::move(upsertedId));
            }
        }
    }

    auto tupleLt = [](const auto& lhs, const auto& rhs) {
        return std::get<0>(lhs) < std::get<0>(rhs);
    };
    auto tupleEq = [](const auto& lhs, const auto& rhs) {
        return std::get<0>(lhs) == std::get<0>(rhs);
    };

    std::stable_sort(details.begin(), details.end(), std::move(tupleLt));
    details.erase(std::unique(details.begin(), details.end(), std::move(tupleEq)), details.end());

    for (auto& [idx, status, upsertedId] : details) {
        BulkWriteReplyItem item(idx, std::move(status));
        item.setN(0);
        if (batchType == BatchedCommandRequest::BatchType_Update) {
            item.setNModified(0);
        }

        if (upsertedId) {
            item.setN(1);
            item.setUpserted(std::move(*upsertedId));
        }

        reply.items.emplace_back(std::move(item));
    }

    if (bcr.isWriteConcernErrorSet()) {
        reply.wcErrors = *bcr.getWriteConcernError();
    }

    if (bcr.areRetriedStmtIdsSet()) {
        reply.retriedStmtIds = bcr.getRetriedStmtIds();
    }

    return reply;
}

const Status& BasicResponse::getStatus() const {
    tassert(11272100, "Expected OK status or error", !isEmpty());
    return _swReply->getStatus();
}

CommandReplyVariant& BasicResponse::getReply() {
    tassert(11272101, "Expected OK status", isOK());
    return _swReply->getValue();
}

const CommandReplyVariant& BasicResponse::getReply() const {
    tassert(11272102, "Expected OK status", isOK());
    return _swReply->getValue();
}

bool BasicResponse::isShutdownError() const {
    if (isError()) {
        return ErrorCodes::isShutdownError(getStatus()) ||
            (getStatus() == ErrorCodes::CallbackCanceled && globalInShutdownDeprecated());
    }
    return false;
}

bool WriteBatchExecutor::usesProvidedRoutingContext(const WriteBatch& batch) const {
    // For SimpleWriteBatches, the executor use the provided RoutingContext. For all other batch
    // types, the executor does not use the provided RoutingContext.
    return std::visit(OverloadedVisitor{
                          [](const SimpleWriteBatch& data) { return true; },
                          [](const TwoPhaseWriteBatch& data) { return false; },
                          [](const InternalTransactionBatch& data) { return false; },
                          [](const MultiWriteBlockingMigrationsBatch& data) { return false; },
                          [](const EmptyBatch& data) { return false; },
                      },
                      batch.data);
}

WriteBatchResponse WriteBatchExecutor::execute(OperationContext* opCtx,
                                               RoutingContext& routingCtx,
                                               const WriteBatch& batch) {
    return std::visit(
        [&](const auto& batchData) -> WriteBatchResponse {
            return _execute(opCtx, routingCtx, batchData);
        },
        batch.data);
}

BatchedCommandRequest WriteBatchExecutor::buildBatchWriteRequest(
    OperationContext* opCtx,
    const std::vector<WriteOp>& ops,
    const std::map<NamespaceString, ShardEndpoint>& versionByNss,
    const std::set<NamespaceString>& nssIsViewfulTimeseries,
    const std::map<WriteOpId, UUID>& sampleIds,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    IsEmbeddedCommand isEmbeddedCommand,
    BatchedCommandRequest::BatchType batchType,
    const NamespaceString& nss,
    boost::optional<UUID> collectionUuid,
    const boost::optional<mongo::EncryptionInformation>& encryptionInformation) const {
    tassert(11468107, "Expected at least one write op", !ops.empty());

    // Translate timeseries collection to bucket namespace if detected by the analyzer.
    NamespaceString targetedNss = nss;
    bool isTimeseriesNamespace = false;
    if (nssIsViewfulTimeseries.contains(nss)) {
        targetedNss = nss.makeTimeseriesBucketsNamespace();

        if (!isRawDataOperation(opCtx)) {
            isTimeseriesNamespace = true;
        }
    }

    BatchedCommandRequest request([&]() -> BatchedCommandRequest {
        if (batchType == BatchedCommandRequest::BatchType_Insert) {
            std::vector<BSONObj> insertDocs;
            for (auto& op : ops) {
                insertDocs.emplace_back(op.getInsertOp().getDocument());
            }

            write_ops::InsertCommandRequest insertRequest(targetedNss);
            insertRequest.setDocuments(std::move(insertDocs));
            return std::move(insertRequest);
        } else if (batchType == BatchedCommandRequest::BatchType_Update) {
            // Copy the UpdateOpEntry from the original command, and then update the "sampleId"
            // and "$_allowShardKeyUpdatesWithoutFullShardKeyInQuery" fields appropriately.
            std::vector<write_ops::UpdateOpEntry> updateOps;
            for (auto& op : ops) {
                auto updateOpEntry = write_op_helpers::getOrMakeUpdateOpEntry(op.getUpdateOp());

                auto sampleIdIt = sampleIds.find(getWriteOpId(op));
                updateOpEntry.setSampleId(sampleIdIt != sampleIds.end()
                                              ? boost::make_optional(sampleIdIt->second)
                                              : boost::none);

                if (allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
                    updateOpEntry.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
                        *allowShardKeyUpdatesWithoutFullShardKeyInQuery);
                } else {
                    updateOpEntry.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(OptionalBool{});
                }

                updateOps.emplace_back(std::move(updateOpEntry));
            }

            write_ops::UpdateCommandRequest updateRequest(targetedNss);
            updateRequest.setUpdates(std::move(updateOps));
            return std::move(updateRequest);
        } else if (batchType == BatchedCommandRequest::BatchType_Delete) {
            // Copy the DeleteOpEntry from the original command, and then update the "sampleId"
            // field appropriately.
            std::vector<write_ops::DeleteOpEntry> deleteOps;
            for (auto& op : ops) {
                auto deleteOpEntry = write_op_helpers::getOrMakeDeleteOpEntry(op.getDeleteOp());

                auto sampleIdIt = sampleIds.find(getWriteOpId(op));
                deleteOpEntry.setSampleId(sampleIdIt != sampleIds.end()
                                              ? boost::make_optional(sampleIdIt->second)
                                              : boost::none);

                deleteOps.emplace_back(std::move(deleteOpEntry));
            }

            write_ops::DeleteCommandRequest deleteRequest(targetedNss);
            deleteRequest.setDeletes(std::move(deleteOps));
            return deleteRequest;
        } else {
            MONGO_UNREACHABLE;
        }
    }());

    auto& wcb = request.getWriteCommandRequestBase();

    wcb.setCollectionUUID(collectionUuid);
    wcb.setEncryptionInformation(encryptionInformation);
    if (isTimeseriesNamespace) {
        wcb.setIsTimeseriesNamespace(true);
    }

    wcb.setOrdered(_cmdRef.getOrdered());
    wcb.setBypassDocumentValidation(_cmdRef.getBypassDocumentValidation());
    wcb.setBypassEmptyTsReplacement(_cmdRef.getBypassEmptyTsReplacement());
    request.setLet(_cmdRef.getLet());
    request.getGenericArguments().setComment(_cmdRef.getComment());

    if (const auto& lrc = _cmdRef.getLegacyRuntimeConstants()) {
        request.setLegacyRuntimeConstants(*lrc);
    }

    if (opCtx->isRetryableWrite()) {
        std::vector<int> stmtIds;
        stmtIds.reserve(ops.size());
        for (auto& op : ops) {
            stmtIds.push_back(op.getEffectiveStmtId());
        }

        wcb.setStmtIds(std::move(stmtIds));
    }

    // Don't set "maxTimeMS" on the requests that get sent to the shards. If the router exceeds
    // its time limit, it will close the connections to the shards and return a MaxTimeMSExceeded
    // error to the client. We also don't need to set "$_originalQuery" or "$_originalCollation".
    // These fields are only used by ClusterWriteWithoutShardKeyCmd.

    // Append shard versions if needed.
    if (!versionByNss.empty()) {
        auto versionIt = versionByNss.find(nss);
        tassert(11468108,
                "The shard version info should be present in the batch",
                versionIt != versionByNss.end());
        auto& version = versionIt->second;

        if (version.shardVersion) {
            request.setShardVersion(*version.shardVersion);
        }

        if (version.databaseVersion) {
            request.setDbVersion(*version.databaseVersion);
        }
    }

    if (isEmbeddedCommand) {
        request.getGenericArguments().setRawData(isRawDataOperation(opCtx));
        filterGenericArgumentsForEmbeddedCommand(opCtx, request);
    }

    return request;
}

BulkWriteCommandRequest WriteBatchExecutor::buildBulkWriteRequest(
    OperationContext* opCtx,
    const std::vector<WriteOp>& ops,
    const std::map<NamespaceString, ShardEndpoint>& versionByNss,
    const std::set<NamespaceString>& nssIsViewfulTimeseries,
    const std::map<WriteOpId, UUID>& sampleIds,
    boost::optional<bool> errorsOnly,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    IsEmbeddedCommand isEmbeddedCommand) const {
    const bool isRetryableWrite = opCtx->isRetryableWrite();

    std::vector<BulkWriteOpVariant> bulkOps;
    std::vector<NamespaceInfoEntry> nsInfos;
    std::map<NamespaceString, int> nsIndexMap;
    std::vector<int> stmtIds;
    if (isRetryableWrite) {
        stmtIds.reserve(ops.size());
    }

    for (auto& op : ops) {
        auto bulkOp = write_op_helpers::getOrMakeBulkWriteOp(op);
        auto& nss = op.getNss();

        NamespaceInfoEntry nsInfo(nss);
        nsInfo.setCollectionUUID(op.getCollectionUUID());
        nsInfo.setEncryptionInformation(op.getEncryptionInformation());

        // Translate timeseries collection to bucket namespace if detected by the analyzer.
        if (nssIsViewfulTimeseries.contains(nss)) {
            nsInfo.setNs(nss.makeTimeseriesBucketsNamespace());
            if (!isRawDataOperation(opCtx)) {
                nsInfo.setIsTimeseriesNamespace(true);
            }
        }

        // Append shard versions if needed.
        if (!versionByNss.empty()) {
            auto versionIt = versionByNss.find(nss);
            tassert(10346801,
                    "The shard version info should be present in the batch",
                    versionIt != versionByNss.end());
            auto& version = versionIt->second;

            nsInfo.setShardVersion(version.shardVersion);
            nsInfo.setDatabaseVersion(version.databaseVersion);
        }

        // Reassigns the namespace index for the list of ops.
        if (nsIndexMap.find(nss) == nsIndexMap.end()) {
            nsIndexMap[nss] = nsInfos.size();
            nsInfos.push_back(nsInfo);
        }

        auto nsIndex = nsIndexMap[nss];
        visit([&](auto& value) { return value.setNsInfoIdx(nsIndex); }, bulkOp);

        if (getWriteOpType(op) == WriteType::kUpdate &&
            allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
            get<BulkWriteUpdateOp>(bulkOp).setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
                *allowShardKeyUpdatesWithoutFullShardKeyInQuery);
        }

        auto sampleIdIt = sampleIds.find(getWriteOpId(op));
        if (sampleIdIt != sampleIds.end()) {
            visit(OverloadedVisitor{
                      [&](mongo::BulkWriteInsertOp& op) { return; },
                      [&](mongo::BulkWriteUpdateOp& op) { op.setSampleId(sampleIdIt->second); },
                      [&](mongo::BulkWriteDeleteOp& op) { op.setSampleId(sampleIdIt->second); },
                  },
                  bulkOp);
        }

        bulkOps.emplace_back(bulkOp);

        if (isRetryableWrite) {
            stmtIds.push_back(op.getEffectiveStmtId());
        }
    }

    auto bulkRequest = BulkWriteCommandRequest(std::move(bulkOps), std::move(nsInfos));
    bulkRequest.setOrdered(_cmdRef.getOrdered());
    bulkRequest.setBypassDocumentValidation(_cmdRef.getBypassDocumentValidation());
    bulkRequest.setBypassEmptyTsReplacement(_cmdRef.getBypassEmptyTsReplacement());
    bulkRequest.setLet(_cmdRef.getLet());
    bulkRequest.setComment(_cmdRef.getComment());

    bulkRequest.setErrorsOnly(errorsOnly.value_or(false));

    if (isEmbeddedCommand) {
        bulkRequest.getGenericArguments().setRawData(isRawDataOperation(opCtx));
        filterGenericArgumentsForEmbeddedCommand(opCtx, bulkRequest);
    }

    if (isRetryableWrite) {
        bulkRequest.setStmtIds(std::move(stmtIds));
    }
    return bulkRequest;
}

namespace {
template <typename RequestType>
BSONObj buildWriteCommandRequestObj(OperationContext* opCtx,
                                    const RequestType& request,
                                    ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
                                    ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) {
    BSONObjBuilder builder;
    request.serialize(&builder);

    if (shouldAppendLsidAndTxnNumber) {
        logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
    }
    if (shouldAppendReadWriteConcern) {
        appendWriteConcern(opCtx, builder);
    }

    return builder.obj();
}

void unsetLegacyRuntimeConstants(CommandRequestVariant& requestVar) {
    std::visit(OverloadedVisitor(
                   [&](BatchedCommandRequest& request) { request.unsetLegacyRuntimeConstants(); },
                   [&](BulkWriteCommandRequest& request) {},
                   [&](write_ops::FindAndModifyCommandRequest& request) {
                       request.setLegacyRuntimeConstants(boost::none);
                   }),
               requestVar);
}
}  // namespace

BSONObj WriteBatchExecutor::buildBatchWriteRequestObj(
    OperationContext* opCtx,
    const BatchedCommandRequest& batchRequest,
    ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
    ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const {
    return buildWriteCommandRequestObj(
        opCtx, batchRequest, shouldAppendLsidAndTxnNumber, shouldAppendReadWriteConcern);
}

BSONObj WriteBatchExecutor::buildBulkWriteRequestObj(
    OperationContext* opCtx,
    const BulkWriteCommandRequest& bulkRequest,
    ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
    ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const {
    return buildWriteCommandRequestObj(
        opCtx, bulkRequest, shouldAppendLsidAndTxnNumber, shouldAppendReadWriteConcern);
}

write_ops::FindAndModifyCommandRequest WriteBatchExecutor::buildFindAndModifyRequest(
    OperationContext* opCtx,
    const std::vector<WriteOp>& ops,
    const std::map<NamespaceString, ShardEndpoint>& versionByNss,
    const std::set<NamespaceString>& nssIsViewfulTimeseries,
    const std::map<WriteOpId, UUID>& sampleIds,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    IsEmbeddedCommand isEmbeddedCommand) const {
    tassert(10394902, "Expected a findAndModify request", _cmdRef.isFindAndModifyCommand());
    tassert(10394901, "Expected a single write op for the findAndModify command", ops.size() == 1);

    const auto& op = ops.front();
    auto& nss = op.getNss();

    // Make a copy of the original request and clear attributes that we may append later.
    auto request = _cmdRef.getFindAndModifyCommandRequest();
    request.setLsid(boost::none);
    request.setTxnNumber(boost::none);
    request.setWriteConcern(boost::none);
    request.setReadConcern(boost::none);
    request.setShardVersion(boost::none);
    request.setDatabaseVersion(boost::none);
    request.setIsTimeseriesNamespace(false);
    request.setSampleId(boost::none);
    request.setStmtId(boost::none);
    request.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(OptionalBool{});

    request.setNamespace(nss);

    if (nssIsViewfulTimeseries.contains(nss)) {
        request.setNamespace(nss.makeTimeseriesBucketsNamespace());
        if (!isRawDataOperation(opCtx)) {
            request.setIsTimeseriesNamespace(true);
        }
    }

    auto versionIt = versionByNss.find(nss);
    if (versionIt != versionByNss.end()) {
        request.setShardVersion(versionIt->second.shardVersion);
        request.setDatabaseVersion(versionIt->second.databaseVersion);
    }

    auto sampleIdIt = sampleIds.find(getWriteOpId(op));
    if (sampleIdIt != sampleIds.end()) {
        request.setSampleId(sampleIdIt->second);
    }

    if (getWriteOpType(op) == WriteType::kUpdate &&
        allowShardKeyUpdatesWithoutFullShardKeyInQuery.has_value()) {
        request.setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
            *allowShardKeyUpdatesWithoutFullShardKeyInQuery);
    }

    if (isEmbeddedCommand) {
        request.getGenericArguments().setRawData(isRawDataOperation(opCtx));
        filterGenericArgumentsForEmbeddedCommand(opCtx, request);
    }

    if (opCtx->isRetryableWrite()) {
        request.setStmtId(op.getEffectiveStmtId());
    }

    return request;
}

BSONObj WriteBatchExecutor::buildFindAndModifyRequestObj(
    OperationContext* opCtx,
    write_ops::FindAndModifyCommandRequest request,
    ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
    ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const {
    auto cmdObj = request.toBSON();

    if (shouldAppendReadWriteConcern) {
        cmdObj = applyReadWriteConcern(opCtx, /*appendRC*/ true, /*appendWC*/ true, cmdObj);
    }

    BSONObjBuilder builder(cmdObj);
    if (shouldAppendLsidAndTxnNumber) {
        logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
    }

    // Try to preserve the original command name alias to forward to shards.
    cmdObj = builder.obj();
    if (!_originalCmdObj.isEmpty() &&
        _originalCmdObj.firstElementFieldNameStringData() !=
            cmdObj.firstElementFieldNameStringData()) {

        BSONObjBuilder builder;
        BSONObjIterator it(cmdObj);
        BSONElement firstElt = it.next();
        builder.append(_originalCmdObj.firstElementFieldNameStringData(),
                       firstElt.checkAndGetStringData());
        while (it.more()) {
            BSONElement elt = it.next();
            builder.append(elt);
        }
        cmdObj = builder.obj();
    }
    return cmdObj;
}

CommandRequestVariant WriteBatchExecutor::buildRequest(
    OperationContext* opCtx,
    const std::vector<WriteOp>& ops,
    const std::map<NamespaceString, ShardEndpoint>& versionByNss,
    const std::set<NamespaceString>& nssIsViewfulTimeseries,
    const std::map<WriteOpId, UUID>& sampleIds,
    boost::optional<bool> errorsOnly,
    boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
    IsEmbeddedCommand isEmbeddedCommand) const {
    return _cmdRef.visitRequest(OverloadedVisitor(
        [&](const BatchedCommandRequest& bcr) -> CommandRequestVariant {
            const auto& encryptionInfo =
                bcr.getWriteCommandRequestBase().getEncryptionInformation();
            return buildBatchWriteRequest(opCtx,
                                          ops,
                                          versionByNss,
                                          nssIsViewfulTimeseries,
                                          sampleIds,
                                          allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                          isEmbeddedCommand,
                                          bcr.getBatchType(),
                                          bcr.getNS(),
                                          bcr.getCollectionUUID(),
                                          encryptionInfo);
        },
        [&](const BulkWriteCommandRequest&) -> CommandRequestVariant {
            return buildBulkWriteRequest(opCtx,
                                         ops,
                                         versionByNss,
                                         nssIsViewfulTimeseries,
                                         sampleIds,
                                         errorsOnly,
                                         allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                         isEmbeddedCommand);
        },
        [&](const write_ops::FindAndModifyCommandRequest&) -> CommandRequestVariant {
            return buildFindAndModifyRequest(opCtx,
                                             ops,
                                             versionByNss,
                                             nssIsViewfulTimeseries,
                                             sampleIds,
                                             allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                             isEmbeddedCommand);
        }));
}

BSONObj WriteBatchExecutor::buildRequestObj(
    OperationContext* opCtx,
    CommandRequestVariant request,
    ShouldAppendLsidAndTxnNumber shouldAppendLsidAndTxnNumber,
    ShouldAppendReadWriteConcern shouldAppendReadWriteConcern) const {
    return std::visit(OverloadedVisitor(
                          [&](BatchedCommandRequest& request) {
                              return buildBatchWriteRequestObj(opCtx,
                                                               std::move(request),
                                                               shouldAppendLsidAndTxnNumber,
                                                               shouldAppendReadWriteConcern);
                          },
                          [&](BulkWriteCommandRequest& request) {
                              return buildBulkWriteRequestObj(opCtx,
                                                              request,
                                                              shouldAppendLsidAndTxnNumber,
                                                              shouldAppendReadWriteConcern);
                          },
                          [&](write_ops::FindAndModifyCommandRequest& request) {
                              return buildFindAndModifyRequestObj(opCtx,
                                                                  std::move(request),
                                                                  shouldAppendLsidAndTxnNumber,
                                                                  shouldAppendReadWriteConcern);
                          }),
                      request);
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const EmptyBatch& batch) {
    return EmptyBatchResponse{};
}

ShardResponse ShardResponse::make(StatusWith<executor::RemoteCommandResponse> swResponse,
                                  std::vector<WriteOp> ops,
                                  bool inTransaction,
                                  boost::optional<HostAndPort> hostAndPort,
                                  boost::optional<const ShardId&> shardId) {
    tassert(11468109, "Expected at least one write op", !ops.empty());

    WriteCommandRef cmdRef = ops.front().getCommand();

    // If there was a local error, return a ShardResponse that reports this local error.
    if (!swResponse.isOK()) {
        const Status& status = swResponse.getStatus();

        LOGV2_DEBUG(10896501,
                    4,
                    "Local error occurred when receiving write results from shard",
                    "error"_attr = redact(status),
                    "shardId"_attr = shardId,
                    "host"_attr = hostAndPort);

        const bool transientTxnError = isTransientTxnError(inTransaction, status);

        return ShardResponse{StatusWith<CommandReplyVariant>{status},
                             boost::none /*wce*/,
                             std::move(ops),
                             transientTxnError,
                             std::move(hostAndPort)};
    }

    const auto& response = swResponse.getValue();
    auto status = getStatusFromCommandResult(response.data);
    boost::optional<WriteConcernErrorDetail> wce;

    // If this is a findAndModify command, populate 'wce' now.
    if (cmdRef.isFindAndModifyCommand()) {
        if (auto wcStatus = getWriteConcernStatusFromCommandResult(response.data);
            !wcStatus.isOK()) {
            wce = WriteConcernErrorDetail{std::move(wcStatus)};
        }
    }

    // If there was a top-level error, return a ShardResponse that reports this top-level error.
    if (!status.isOK()) {
        if (!cmdRef.isFindAndModifyCommand()) {
            status = status.withContext(str::stream() << "cluster write results unavailable from "
                                                      << response.target);
        }

        LOGV2_DEBUG(10347001,
                    4,
                    "Remote error reported from shard when receiving cluster write results",
                    "error"_attr = redact(status),
                    "shardId"_attr = shardId,
                    "host"_attr = response.target);

        const bool transientTxnError = isTransientTxnError(inTransaction, status, response.data);

        return ShardResponse{StatusWith<CommandReplyVariant>{std::move(status)},
                             std::move(wce),
                             std::move(ops),
                             transientTxnError,
                             response.target};
    }

    // If there were no local errors or top-level errors, parse the reply, and then return a
    // ShardResponse that contains the parsed reply.
    LOGV2_DEBUG(10347003,
                4,
                "Parsing cluster write shard response",
                "response"_attr = response.data,
                "host"_attr = response.target);

    auto swReply = cmdRef.visitRequest(OverloadedVisitor(
        [&](const BatchedCommandRequest& bcr) -> StatusWith<CommandReplyVariant> {
            auto batchType = bcr.getBatchType();
            auto parsedReply = parseBatchWriteCommandReply(response.data, batchType);

            // If this is not a findAndModify command, then we populate 'wce' here.
            if (parsedReply.wcErrors) {
                wce = *parsedReply.wcErrors;
            }

            return CommandReplyVariant{std::move(parsedReply)};
        },
        [&](const BulkWriteCommandRequest&) -> StatusWith<CommandReplyVariant> {
            auto parsedReply = parseBulkWriteCommandReply(response.data);

            // If this is not a findAndModify command, then we populate 'wce' here.
            if (auto wcError = parsedReply.getWriteConcernError()) {
                wce = WriteConcernErrorDetail{
                    Status(ErrorCodes::Error(wcError->getCode()), wcError->getErrmsg())};
            }

            return CommandReplyVariant{std::move(parsedReply)};
        },
        [&](const write_ops::FindAndModifyCommandRequest&) -> StatusWith<CommandReplyVariant> {
            return CommandReplyVariant{parseFindAndModifyCommandReply(
                CommandHelpers::filterCommandReplyForPassthrough(response.data))};
        }));

    return ShardResponse{std::move(swReply),
                         std::move(wce),
                         std::move(ops),
                         false /*transientTxnError*/,
                         response.target};
}

ShardResponse ShardResponse::makeEmpty(std::vector<WriteOp> ops) {
    return ShardResponse{
        boost::none /*swReply*/, boost::none /*wce*/, std::move(ops), false /*transientTxnError*/};
}

NoRetryWriteBatchResponse NoRetryWriteBatchResponse::make(
    Status status,
    boost::optional<WriteConcernErrorDetail> wce,
    const WriteOp& op,
    bool inTransaction) {
    tassert(11468112, "Expected non-OK status", !status.isOK());
    const bool transientTxnError = isTransientTxnError(inTransaction, status);

    return NoRetryWriteBatchResponse{StatusWith<CommandReplyVariant>(std::move(status)),
                                     std::move(wce),
                                     std::vector<WriteOp>{op},
                                     transientTxnError};
}

NoRetryWriteBatchResponse NoRetryWriteBatchResponse::make(
    StatusWith<BatchWriteCommandReply> swResponse,
    boost::optional<WriteConcernErrorDetail> wce,
    const WriteOp& op,
    bool inTransaction) {
    return makeImpl(std::move(swResponse), std::move(wce), op, inTransaction);
}

NoRetryWriteBatchResponse NoRetryWriteBatchResponse::make(
    StatusWith<BulkWriteCommandReply> swResponse,
    boost::optional<WriteConcernErrorDetail> wce,
    const WriteOp& op,
    bool inTransaction) {
    return makeImpl(std::move(swResponse), std::move(wce), op, inTransaction);
}

NoRetryWriteBatchResponse NoRetryWriteBatchResponse::make(
    StatusWith<write_ops::FindAndModifyCommandReply> swResponse,
    boost::optional<WriteConcernErrorDetail> wce,
    const WriteOp& op,
    bool inTransaction) {
    return makeImpl(std::move(swResponse), std::move(wce), op, inTransaction);
}

template <typename ResponseType>
NoRetryWriteBatchResponse NoRetryWriteBatchResponse::makeImpl(
    StatusWith<ResponseType> swResponse,
    boost::optional<WriteConcernErrorDetail> wce,
    const WriteOp& op,
    bool inTransaction) {
    const auto& status = swResponse.getStatus();
    const bool transientTxnError = isTransientTxnError(inTransaction, status);

    auto swReply = status.isOK() ? StatusWith(CommandReplyVariant{std::move(swResponse.getValue())})
                                 : StatusWith<CommandReplyVariant>(status);

    return NoRetryWriteBatchResponse{
        std::move(swReply), std::move(wce), std::vector<WriteOp>{op}, transientTxnError};
}

NoRetryWriteBatchResponse NoRetryWriteBatchResponse::make(
    const StatusWith<BSONObj>& swResponse,
    boost::optional<WriteConcernErrorDetail> wce,
    const WriteOp& op,
    bool inTransaction) {
    WriteCommandRef cmdRef = op.getCommand();
    const auto& status = swResponse.getStatus();

    return cmdRef.visitRequest(OverloadedVisitor(
        [&](const BatchedCommandRequest& bcr) {
            auto batchType = bcr.getBatchType();
            auto swReply = status.isOK()
                ? StatusWith(parseBatchWriteCommandReplySingleOp(swResponse.getValue(), batchType))
                : StatusWith<BatchWriteCommandReply>(status);

            return make(std::move(swReply), std::move(wce), op, inTransaction);
        },
        [&](const BulkWriteCommandRequest&) {
            auto swReply = status.isOK()
                ? StatusWith(parseBulkWriteCommandReplySingleOp(swResponse.getValue(), op))
                : StatusWith<BulkWriteCommandReply>(status);

            return make(std::move(swReply), std::move(wce), op, inTransaction);
        },
        [&](const write_ops::FindAndModifyCommandRequest&) {
            auto swReply = status.isOK()
                ? StatusWith(parseFindAndModifyCommandReply(swResponse.getValue()))
                : StatusWith<write_ops::FindAndModifyCommandReply>(status);

            return make(std::move(swReply), std::move(wce), op, inTransaction);
        }));
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const SimpleWriteBatch& batch) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    const boost::optional<bool> errorsOnly = _cmdRef.getErrorsOnly();

    std::vector<AsyncRequestsSender::Request> requestsToSend;

    for (auto& [shardId, shardRequest] : batch.requestByShardId) {
        auto request =
            buildRequest(opCtx,
                         shardRequest.ops,
                         shardRequest.versionByNss,
                         shardRequest.nssIsViewfulTimeseries,
                         shardRequest.sampleIds,
                         errorsOnly,
                         boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */,
                         IsEmbeddedCommand::No);
        auto requestObj = buildRequestObj(opCtx,
                                          std::move(request),
                                          ShouldAppendLsidAndTxnNumber::Yes,
                                          ShouldAppendReadWriteConcern::Yes);

        LOGV2_DEBUG(10605503,
                    4,
                    "Constructed request for shard",
                    "request"_attr = requestObj,
                    "shardId"_attr = shardId);

        requestsToSend.emplace_back(shardId, std::move(requestObj));
    }

    // Note we check this rather than `isRetryableWrite()` because we do not want to retry
    // commands within retryable internal transactions.
    bool shouldRetry = opCtx->getTxnNumber() && !TransactionRouter::get(opCtx);
    auto databaseName = getExecutionDatabase();
    auto sender = MultiStatementTransactionRequestsSender(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        databaseName,
        std::move(requestsToSend),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        shouldRetry ? Shard::RetryPolicy::kIdempotent : Shard::RetryPolicy::kStrictlyNotIdempotent);

    // For each namespace 'nss' that is used by the current 'batch', inform the RoutingContext
    // that we are sending requests that involve 'nss'.
    for (const auto& nss : batch.getInvolvedNamespaces()) {
        routingCtx.onRequestSentForNss(nss);
    }

    auto resp = SimpleWriteBatchResponse::makeEmpty(batch.isRetryableWriteWithId);
    bool stopParsingResponses = false;

    while (!stopParsingResponses && !sender.done()) {
        auto arsResponse = sender.next();
        auto& shardId = arsResponse.shardId;

        const auto& shardRequest = batch.requestByShardId.at(shardId);

        ShardResponse shardResponse = ShardResponse::make(std::move(arsResponse.swResponse),
                                                          shardRequest.ops,
                                                          inTransaction,
                                                          std::move(arsResponse.shardHostAndPort),
                                                          shardId);

        const bool isShutdownError = shardResponse.isShutdownError();

        const bool hasTransientTxnError = shardResponse.hasTransientTxnError();
        const bool hasTopLevelErrorThatAbortsTxn =
            (inTransaction && shardResponse.isError() && !hasTransientTxnError &&
             !shardResponse.isWouldChangeOwningShardError());

        const auto numItemErrors = shardResponse.isOK() &&
                holds_alternative<BulkWriteCommandReply>(shardResponse.getReply())
            ? get<BulkWriteCommandReply>(shardResponse.getReply()).getNErrors()
            : 0;

        const bool hasItemErrorThatAbortsTxn = inTransaction && numItemErrors > 0;

        if (isShutdownError || hasTransientTxnError || hasTopLevelErrorThatAbortsTxn ||
            hasItemErrorThatAbortsTxn) {
            LOGV2_DEBUG(11272105,
                        4,
                        "Stopped parsing of shard responses due to error in transaction "
                        "or shutdown error",
                        "topLevelStatus"_attr = shardResponse.getStatus(),
                        "isShutdownError"_attr = (isShutdownError ? "true" : "false"),
                        "numItemErrors"_attr = numItemErrors,
                        "shardId"_attr = shardId,
                        "host"_attr = shardResponse.getHostAndPort());

            stopParsingResponses = true;
        }

        resp.shardResponses.emplace_back(std::move(shardId), std::move(shardResponse));
    }

    // If we stopped parsing responses early, generate empty ShardResponses for the remaining
    // ShardIds.
    if (stopParsingResponses) {
        // Make a set of the ShardIds for which we already have ShardResponses.
        absl::flat_hash_set<ShardId> shardIdSet;
        for (const auto& [shardId, _] : resp.shardResponses) {
            shardIdSet.emplace(shardId);
        }

        // For each 'shardId' that isn't in 'shardIdSet', create an empty ShardResponse and
        // add it to 'resp.shardResponses'.
        for (const auto& [shardId, _] : batch.requestByShardId) {
            if (!shardIdSet.count(shardId)) {
                const auto& shardRequest = batch.requestByShardId.at(shardId);
                resp.shardResponses.emplace_back(shardId,
                                                 ShardResponse::makeEmpty(shardRequest.ops));
            }
        }
    }

    tassert(10346800,
            "There should same number of requests and responses from a simple write batch",
            resp.shardResponses.size() == batch.requestByShardId.size());

    return WriteBatchResponse{std::move(resp)};
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const TwoPhaseWriteBatch& batch) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    const boost::optional<bool> errorsOnly = _cmdRef.getErrorsOnly();

    const WriteOp& writeOp = batch.op;
    const auto& nss = writeOp.getNss();

    bool allowShardKeyUpdatesWithoutFullShardKeyInQuery =
        opCtx->isRetryableWrite() || TransactionRouter::get(opCtx);

    std::map<WriteOpId, UUID> sampleIds;
    if (batch.sampleId) {
        sampleIds.emplace(getWriteOpId(writeOp), *batch.sampleId);
    }

    std::set<NamespaceString> nssIsViewfulTimeseries;
    if (batch.isViewfulTimeseries) {
        nssIsViewfulTimeseries.emplace(nss);
    }
    auto request = buildRequest(opCtx,
                                {writeOp},
                                {},
                                std::move(nssIsViewfulTimeseries),
                                sampleIds,
                                errorsOnly,
                                allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                                IsEmbeddedCommand::Yes);
    auto cmdObj = buildRequestObj(opCtx,
                                  std::move(request),
                                  ShouldAppendLsidAndTxnNumber::No,
                                  ShouldAppendReadWriteConcern::No);

    boost::optional<WriteConcernErrorDetail> wce;
    auto swRes =
        write_without_shard_key::runTwoPhaseWriteProtocol(opCtx, nss, std::move(cmdObj), wce);

    auto swResponse = swRes.isOK() ? StatusWith{swRes.getValue().getResponse().getOwned()}
                                   : StatusWith<BSONObj>{swRes.getStatus()};

    return NoRetryWriteBatchResponse::make(swResponse, std::move(wce), writeOp, inTransaction);
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const InternalTransactionBatch& batch) {
    const boost::optional<bool> errorsOnly = _cmdRef.getErrorsOnly();
    const WriteOp& writeOp = batch.op;

    std::map<WriteOpId, UUID> sampleIds;
    if (batch.sampleId) {
        sampleIds.emplace(getWriteOpId(writeOp), *batch.sampleId);
    }

    auto requestVar = buildRequest(opCtx,
                                   {writeOp},
                                   {}, /* versionByNss */
                                   {}, /* nssIsViewfulTimeseries */
                                   sampleIds,
                                   errorsOnly,
                                   boost::none /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */,
                                   IsEmbeddedCommand::No);

    // Remove the "legacyRuntimeConstants" field if it's present.
    unsetLegacyRuntimeConstants(requestVar);

    // Execute the op in an internal transaction, then return a NoRetryWriteBatchResponse
    // containing the result.
    using RetT = WriteBatchResponse;

    return visit(OverloadedVisitor(
                     [&](const BatchedCommandRequest& request) -> RetT {
                         return executeOpInInternalTransaction<BatchWriteCommandReply>(
                             opCtx, request, writeOp);
                     },
                     [&](const BulkWriteCommandRequest& request) -> RetT {
                         return executeOpInInternalTransaction<BulkWriteCommandReply>(
                             opCtx, request, writeOp);
                     },
                     [&](const write_ops::FindAndModifyCommandRequest& request) -> RetT {
                         tasserted(11288300, "Unexpected findAndModify command type");
                     }),
                 requestVar);
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const MultiWriteBlockingMigrationsBatch& batch) {
    const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
    const boost::optional<bool> errorsOnly = _cmdRef.getErrorsOnly();

    const WriteOp& writeOp = batch.op;
    const auto& nss = writeOp.getNss();

    auto request = buildRequest(opCtx,
                                {writeOp},
                                {}, /* versionByNss */
                                {}, /* nssIsViewfulTimeseries */
                                {}, /* sampleIds */
                                errorsOnly,
                                boost::none, /* allowShardKeyUpdatesWithoutFullShardKeyInQuery */
                                IsEmbeddedCommand::Yes);
    auto cmdObj = buildRequestObj(opCtx,
                                  std::move(request),
                                  ShouldAppendLsidAndTxnNumber::Yes,
                                  ShouldAppendReadWriteConcern::No);

    StatusWith<BSONObj> reply = [&]() {
        try {
            BSONObj res =
                coordinate_multi_update_util::executeCoordinateMultiUpdate(opCtx, nss, cmdObj);
            auto status = getStatusFromCommandResult(res);

            return status.isOK() ? StatusWith(std::move(res))
                                 : StatusWith<BSONObj>(std::move(status));
        } catch (const DBException& e) {
            return StatusWith<BSONObj>(e.toStatus());
        }
    }();

    return NoRetryWriteBatchResponse::make(reply, /*wce*/ boost::none, writeOp, inTransaction);
}

DatabaseName WriteBatchExecutor::getExecutionDatabase() const {
    return _cmdRef.visitRequest(OverloadedVisitor(
        [&](const BatchedCommandRequest& request) { return request.getNS().dbName(); },
        [&](const BulkWriteCommandRequest&) { return DatabaseName::kAdmin; },
        [&](const write_ops::FindAndModifyCommandRequest& request) {
            return request.getNamespace().dbName();
        }));
}

}  // namespace unified_write_executor
}  // namespace mongo
