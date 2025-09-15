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


#include "mongo/s/write_ops/fle.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/commands/query_cmd/bulk_write_common.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

void fillOKInsertReplies(bulk_write_exec::BulkWriteReplyInfo& replyInfo, int size) {
    replyInfo.replyItems.reserve(size);
    for (int i = 0; i < size; ++i) {
        BulkWriteReplyItem reply;
        reply.setN(1);
        reply.setOk(1);
        reply.setIdx(i);
        replyInfo.replyItems.push_back(reply);
    }
}

void assertBulkWriteFLENamespaces(const BulkWriteCommandRequest& request) {
    if (request.getNsInfo().size() > 1) {
        for (const auto& nsInfo : request.getNsInfo()) {
            uassert(ErrorCodes::BadValue,
                    "BulkWrite with Queryable Encryption supports only a single namespace.",
                    !nsInfo.getEncryptionInformation().has_value());
        }
    }
}

bulk_write_exec::BulkWriteReplyInfo processFLEResponse(OperationContext* opCtx,
                                                       const BatchedCommandRequest& request,
                                                       const BulkWriteCRUDOp::OpType& firstOpType,
                                                       bool errorsOnly,
                                                       const BatchedCommandResponse& response) {
    bulk_write_exec::BulkWriteReplyInfo replyInfo;
    if (response.toStatus().isOK() || response.isWriteConcernErrorSet()) {
        if (firstOpType == BulkWriteCRUDOp::kInsert) {
            if (!errorsOnly) {
                fillOKInsertReplies(replyInfo, response.getN());
            }
            replyInfo.summaryFields.nInserted += response.getN();
        } else {
            BulkWriteReplyItem reply;
            reply.setN(response.getN());
            if (firstOpType == BulkWriteCRUDOp::kUpdate) {
                if (response.isUpsertDetailsSet()) {
                    std::vector<BatchedUpsertDetail*> upsertDetails = response.getUpsertDetails();
                    invariant(upsertDetails.size() == 1);
                    // BulkWrite needs only _id, not index.
                    reply.setUpserted(
                        IDLAnyTypeOwned(upsertDetails[0]->getUpsertedID().firstElement()));
                    replyInfo.summaryFields.nUpserted += 1;
                } else {
                    replyInfo.summaryFields.nMatched += response.getN();
                }

                reply.setNModified(response.getNModified());
                replyInfo.summaryFields.nModified += response.getNModified();
            } else {
                replyInfo.summaryFields.nDeleted += response.getN();
            }
            reply.setOk(1);
            reply.setIdx(0);
            if (!errorsOnly) {
                replyInfo.replyItems.push_back(reply);
            }
        }
        if (response.isWriteConcernErrorSet()) {
            auto bwWce =
                BulkWriteWriteConcernError::parseOwned(response.getWriteConcernError()->toBSON());
            replyInfo.wcErrors = bwWce;
        }
    } else {
        if (response.isErrDetailsSet()) {
            const auto& errDetails = response.getErrDetails();
            if (firstOpType == BulkWriteCRUDOp::kInsert) {
                replyInfo.summaryFields.nInserted += response.getN();
                if (!errorsOnly) {
                    fillOKInsertReplies(replyInfo, response.getN() + errDetails.size());
                    for (const auto& err : errDetails) {
                        int32_t idx = err.getIndex();
                        replyInfo.replyItems[idx].setN(0);
                        replyInfo.replyItems[idx].setOk(0);
                        replyInfo.replyItems[idx].setStatus(err.getStatus());
                    }
                } else {
                    // For errorsOnly the errors are the only things we store in replyItems.
                    for (const auto& err : errDetails) {
                        BulkWriteReplyItem item;
                        item.setOk(0);
                        item.setN(0);
                        item.setStatus(err.getStatus());
                        item.setIdx(err.getIndex());
                        replyInfo.replyItems.push_back(item);
                    }
                }
            } else {
                invariant(errDetails.size() == 1 && response.getN() == 0);
                BulkWriteReplyItem reply(0, errDetails[0].getStatus());
                reply.setN(0);
                if (firstOpType == BulkWriteCRUDOp::kUpdate) {
                    reply.setNModified(0);
                }
                replyInfo.replyItems.push_back(reply);
            }
            replyInfo.summaryFields.nErrors += errDetails.size();
        } else {
            // response.toStatus() is not OK but there is no errDetails so the
            // top level status should be not OK instead. Raising an exception.
            uassertStatusOK(response.getTopLevelStatus());
            MONGO_UNREACHABLE;
        }
    }

    switch (firstOpType) {
        // We support only 1 update or 1 delete or multiple inserts for FLE bulkWrites.
        case BulkWriteCRUDOp::kInsert:
            serviceOpCounters(ClusterRole::RouterServer).gotInserts(response.getN());
            break;
        case BulkWriteCRUDOp::kUpdate: {
            const auto& updateRequest = request.getUpdateRequest();
            const mongo::write_ops::UpdateOpEntry& updateOpEntry = updateRequest.getUpdates()[0];
            bulk_write_common::incrementBulkWriteUpdateMetrics(getQueryCounters(opCtx),
                                                               ClusterRole::RouterServer,
                                                               updateOpEntry.getU(),
                                                               updateRequest.getNamespace(),
                                                               updateOpEntry.getArrayFilters(),
                                                               updateOpEntry.getMulti());
            break;
        }
        case BulkWriteCRUDOp::kDelete:
            serviceOpCounters(ClusterRole::RouterServer).gotDelete();
            break;
        default:
            MONGO_UNREACHABLE
    }

    return replyInfo;
}

BatchedCommandRequest makeFLECommandRequest(OperationContext* opCtx,
                                            const BulkWriteCommandRequest& clientRequest,
                                            const std::vector<BulkWriteOpVariant>& ops) {
    BulkWriteCRUDOp firstOp(ops[0]);
    auto firstOpType = firstOp.getType();
    if (firstOpType == BulkWriteCRUDOp::kInsert) {
        std::vector<mongo::BSONObj> documents;
        documents.reserve(ops.size());
        for (const auto& opVariant : ops) {
            BulkWriteCRUDOp op(opVariant);
            uassert(ErrorCodes::InvalidOptions,
                    "BulkWrite with Queryable Encryption and multiple operations supports only "
                    "insert.",
                    op.getType() == BulkWriteCRUDOp::kInsert);
            documents.push_back(op.getInsert()->getDocument());
        }

        write_ops::InsertCommandRequest insertOp =
            bulk_write_common::makeInsertCommandRequestForFLE(
                documents, clientRequest, clientRequest.getNsInfo()[0]);

        return BatchedCommandRequest(insertOp);
    } else if (firstOpType == BulkWriteCRUDOp::kUpdate) {
        uassert(ErrorCodes::InvalidOptions,
                "BulkWrite update with Queryable Encryption supports only a single operation.",
                ops.size() == 1);

        write_ops::UpdateCommandRequest updateCommand =
            bulk_write_common::makeUpdateCommandRequestFromUpdateOp(
                opCtx, firstOp.getUpdate(), clientRequest, /*currentOpIdx=*/0);

        return BatchedCommandRequest(updateCommand);
    } else {
        uassert(ErrorCodes::InvalidOptions,
                "BulkWrite delete with Queryable Encryption supports only a single operation.",
                ops.size() == 1);

        write_ops::DeleteCommandRequest deleteCommand =
            bulk_write_common::makeDeleteCommandRequestForFLE(
                opCtx, firstOp.getDelete(), clientRequest, clientRequest.getNsInfo()[0]);

        return BatchedCommandRequest(deleteCommand);
    }
}

std::pair<FLEBatchResult, bulk_write_exec::BulkWriteReplyInfo> attemptExecuteFLE(
    OperationContext* opCtx, const BulkWriteCommandRequest& clientRequest) {
    assertBulkWriteFLENamespaces(clientRequest);

    const auto& ops = clientRequest.getOps();
    BulkWriteCRUDOp firstOp(ops[0]);
    auto firstOpType = firstOp.getType();
    try {
        BatchedCommandResponse response;
        FLEBatchResult fleResult;

        BatchedCommandRequest fleRequest = makeFLECommandRequest(opCtx, clientRequest, ops);
        fleResult = processFLEBatch(opCtx, fleRequest, &response);

        if (fleResult == FLEBatchResult::kNotProcessed) {
            return {FLEBatchResult::kNotProcessed, bulk_write_exec::BulkWriteReplyInfo()};
        }

        bulk_write_exec::BulkWriteReplyInfo replyInfo = processFLEResponse(
            opCtx, fleRequest, firstOpType, clientRequest.getErrorsOnly(), response);
        return {FLEBatchResult::kProcessed, std::move(replyInfo)};
    } catch (const DBException& ex) {
        LOGV2_WARNING(7749700,
                      "Failed to process bulkWrite with Queryable Encryption",
                      "error"_attr = redact(ex));
        // If Queryable encryption adds support for update with multi: true, we might have to update
        // the way we make replies here to handle SERVER-15292 correctly.
        bulk_write_exec::BulkWriteReplyInfo replyInfo;
        BulkWriteReplyItem reply(0, ex.toStatus());
        reply.setN(0);
        if (firstOpType == BulkWriteCRUDOp::kUpdate) {
            reply.setNModified(0);
        }

        replyInfo.replyItems.push_back(reply);
        replyInfo.summaryFields.nErrors = 1;
        return {FLEBatchResult::kProcessed, std::move(replyInfo)};
    }
}

}  // namespace mongo
