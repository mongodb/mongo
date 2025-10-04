/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/db/fle_crud.h"

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fle2_get_count_info_command_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/db/query/fle/server_rewrite.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_upsert_detail.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


MONGO_FAIL_POINT_DEFINE(fleCrudHangInsert);
MONGO_FAIL_POINT_DEFINE(fleCrudHangPreInsert);

MONGO_FAIL_POINT_DEFINE(fleCrudHangUpdate);
MONGO_FAIL_POINT_DEFINE(fleCrudHangPreUpdate);

MONGO_FAIL_POINT_DEFINE(fleCrudHangDelete);
MONGO_FAIL_POINT_DEFINE(fleCrudHangPreDelete);

MONGO_FAIL_POINT_DEFINE(fleCrudHangFindAndModify);
MONGO_FAIL_POINT_DEFINE(fleCrudHangPreFindAndModify);

namespace mongo {
namespace {
std::vector<write_ops::WriteError> singleStatusToWriteErrors(const Status& status) {
    std::vector<write_ops::WriteError> errors;

    errors.push_back(write_ops::WriteError(0, status));

    return errors;
}

void appendSingleStatusToWriteErrors(const Status& status,
                                     write_ops::WriteCommandReplyBase* replyBase) {
    std::vector<write_ops::WriteError> errors;

    if (replyBase->getWriteErrors()) {
        errors = std::move(replyBase->getWriteErrors().value());
    }

    errors.push_back(write_ops::WriteError(0, status));

    replyBase->setWriteErrors(errors);
}

void appendPossibleWriteConcernErrorToReply(const WriteConcernErrorDetail& wce,
                                            write_ops::WriteCommandReplyBase* replyBase) {
    if (wce.isValid(nullptr)) {
        replyBase->setWriteConcernError(wce.toBSON());
    }
}

// Throw transientTransaction errors as top-level errors instead of returning them as writeErrors.
// By throwing them, the top-level error handling code kicks in and adds the error label. Also
// discards the writeErrors.
void throwTransientTransactionErrorIfNeeded(
    bool hasWriteConcernError,
    const boost::optional<std::vector<write_ops::WriteError>>& writeErrors) {
    if (!writeErrors.has_value()) {
        return;
    }

    for (const auto& e : writeErrors.value()) {
        if (isTransientTransactionError(e.getStatus().code(), hasWriteConcernError, true)) {
            uassertStatusOK(e.getStatus());
        }
    }
}

void replyToResponse(OperationContext* opCtx,
                     write_ops::WriteCommandReplyBase* replyBase,
                     BatchedCommandResponse* response) {
    response->setStatus(Status::OK());
    response->setN(replyBase->getN());
    if (replyBase->getWriteErrors()) {
        for (const auto& error : *replyBase->getWriteErrors()) {
            response->addToErrDetails(error);
        }
    }

    if (auto& wcErrorObj = replyBase->getWriteConcernError()) {
        auto wcError = std::make_unique<WriteConcernErrorDetail>();
        wcError->parseBSON(wcErrorObj.value(), nullptr);
        response->setWriteConcernError(wcError.release());
    }

    if (auto& retriedStmtIds = replyBase->getRetriedStmtIds()) {
        response->setRetriedStmtIds(*retriedStmtIds);
    }

    // Update the OpTime for the reply to current OpTime
    //
    // The OpTime in the reply reflects the OpTime of when the request was run, not when it was
    // committed. The Transaction API propagates the OpTime from the commit transaction onto the
    // current thread so grab it from TLS and change the OpTime on the reply.
    //
    response->setLastOp({OperationTimeTracker::get(opCtx)->getMaxOperationTime().asTimestamp(),
                         repl::OpTime::kUninitializedTerm});
}

void responseToReply(const BatchedCommandResponse& response,
                     write_ops::WriteCommandReplyBase& replyBase) {
    if (response.isLastOpSet()) {
        replyBase.setOpTime(response.getLastOp());
    }

    if (response.isElectionIdSet()) {
        replyBase.setElectionId(response.getElectionId());
    }

    replyBase.setN(response.getN());
    if (response.isErrDetailsSet()) {
        replyBase.setWriteErrors(response.getErrDetails());
    }

    if (response.areRetriedStmtIdsSet()) {
        replyBase.setRetriedStmtIds(response.getRetriedStmtIds());
    }
}

boost::optional<BSONObj> mergeLetAndCVariables(const boost::optional<BSONObj>& let,
                                               const boost::optional<BSONObj>& c) {
    if (!let.has_value() && !c.has_value()) {
        return boost::none;
    } else if (let.has_value() && c.has_value()) {
        BSONObj obj = let.value();
        // Prioritize the fields in c over the fields in let in case of duplicates
        return obj.addFields(c.value());
    } else if (let.has_value()) {
        return let;
    }
    return c;
}

std::vector<QECountInfoRequestTokenSet> toTagSets(
    const std::vector<std::vector<FLEEdgePrfBlock>>& blockSets) {

    std::vector<QECountInfoRequestTokenSet> nestedBlocks;
    nestedBlocks.reserve(blockSets.size());

    for (const auto& tags : blockSets) {
        std::vector<QECountInfoRequestTokens> tagsets;

        tagsets.reserve(tags.size());

        for (auto& tag : tags) {
            tagsets.emplace_back(FLEUtil::vectorFromCDR(tag.esc));
            auto& tokenSet = tagsets.back();

            if (tag.edc.has_value()) {
                tokenSet.setEDCDerivedFromDataTokenAndContentionFactorToken(
                    ConstDataRange(tag.edc.value()));
            }
        }

        nestedBlocks.emplace_back();
        nestedBlocks.back().setTokens(std::move(tagsets));
    }

    return nestedBlocks;
}

FLEEdgeCountInfo convertTokensToEdgeCount(const QECountInfoReplyTokens& token) {

    auto edc = token.getEDCDerivedFromDataTokenAndContentionFactorToken().map(
        [](auto& t) { return EDCDerivedFromDataTokenAndContentionFactorToken::parse(t); });

    auto spos = token.getSearchedPositions().map([](auto& pair) {
        EmuBinaryResult newPair;
        newPair.cpos = pair.getCpos();
        newPair.apos = pair.getApos();
        return newPair;
    });

    auto npos = token.getNullAnchorPositions().map([](auto& pair) {
        ESCCountsPair newPair;
        newPair.cpos = pair.getCpos();
        newPair.apos = pair.getApos();
        return newPair;
    });

    auto esc = ESCTwiceDerivedTagToken::parse(token.getESCTwiceDerivedTagToken());

    return FLEEdgeCountInfo(token.getCount(), esc.asPrfBlock(), spos, npos, token.getStats(), edc);
}

std::vector<std::vector<FLEEdgeCountInfo>> toEdgeCounts(
    const std::vector<QECountInfoReplyTokenSet>& tupleSet) {

    std::vector<std::vector<FLEEdgeCountInfo>> nestedBlocks;
    nestedBlocks.reserve(tupleSet.size());

    for (const auto& tuple : tupleSet) {
        std::vector<FLEEdgeCountInfo> blocks;

        const auto& tuples = tuple.getTokens();

        blocks.reserve(tuples.size());

        for (auto& tuple : tuples) {
            blocks.emplace_back(convertTokensToEdgeCount(tuple));
        }

        nestedBlocks.emplace_back(std::move(blocks));
    }

    return nestedBlocks;
}

}  // namespace

std::shared_ptr<txn_api::SyncTransactionWithRetries> getTransactionWithRetriesForMongoS(
    OperationContext* opCtx) {

    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto fleInlineCrudExecutor = std::make_shared<executor::InlineExecutor>();

    return std::make_shared<txn_api::SyncTransactionWithRetries>(
        opCtx,
        executor,
        TransactionRouterResourceYielder::makeForLocalHandoff(),
        fleInlineCrudExecutor);
}

namespace {
/**
 * Make an expression context from a batch command request and a specific operation. Templated out
 * to work with update and delete.
 */
template <typename T, typename O>
boost::intrusive_ptr<ExpressionContext> makeExpCtx(OperationContext* opCtx,
                                                   const T& request,
                                                   const O& op) {
    std::unique_ptr<CollatorInterface> collator;
    if (op.getCollation()) {
        auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                      ->makeFromBSON(op.getCollation().value());

        uassertStatusOK(statusWithCollator.getStatus());
        collator = std::move(statusWithCollator.getValue());
    }
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .collator(std::move(collator))
                      .ns(request.getNamespace())
                      .runtimeConstants(request.getLegacyRuntimeConstants())
                      .letParameters(request.getLet())
                      .build();
    expCtx->stopExpressionCounters();
    return expCtx;
}

/**
 * We mark commands as "CrudProcessed" to ensure the various commands recognize them as QE related
 * to ensure they are filtered out.
 */
EncryptionInformation makeEmptyProcessEncryptionInformation() {
    EncryptionInformation encryptionInformation;
    encryptionInformation.setCrudProcessed(true);

    // We need to set an empty BSON object here for the schema.
    encryptionInformation.setSchema(BSONObj());

    return encryptionInformation;
}

}  // namespace

using VTS = auth::ValidatedTenancyScope;

/**
 * Checks that all encrypted payloads correspond to an encrypted field,
 * and that the encryption keyId used was appropriate for that field.
 */
void validateInsertUpdatePayloads(OperationContext* opCtx,
                                  const std::vector<EncryptedField>& fields,
                                  const std::vector<EDCServerPayloadInfo>& payload) {
    std::map<StringData, UUID> pathToKeyIdMap;
    for (const auto& field : fields) {
        pathToKeyIdMap.insert({field.getPath(), field.getKeyId()});
    }

    uassert(9783803,
            "Cannot insert an encrypted field with text search query type unless "
            "featureFlagQETextSearchPreview is enabled",
            std::none_of(payload.cbegin(),
                         payload.cend(),
                         [](const EDCServerPayloadInfo& p) { return p.isTextSearchPayload(); }) ||
                gFeatureFlagQETextSearchPreview.isEnabledUseLastLTSFCVWhenUninitialized(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

    for (const auto& field : payload) {
        auto& fieldPath = field.fieldPathName;
        auto expect = pathToKeyIdMap.find(fieldPath);
        uassert(6726300,
                str::stream() << "Field '" << fieldPath << "' is unexpectedly encrypted",
                expect != pathToKeyIdMap.end());
        auto& indexKeyId = field.payload.getIndexKeyId();
        uassert(6726301,
                str::stream() << "Mismatched keyId for field '" << fieldPath << "' expected "
                              << expect->second << ", found " << indexKeyId,
                indexKeyId == expect->second);
    }
}

std::pair<mongo::StatusWith<mongo::txn_api::CommitResult>,
          std::shared_ptr<write_ops::InsertCommandReply>>
insertSingleDocument(OperationContext* opCtx,
                     const write_ops::InsertCommandRequest& insertRequest,
                     BSONObj& document,
                     const mongo::EncryptedFieldConfig& efc,
                     int32_t* stmtId,
                     GetTxnCallback getTxns) {
    auto edcNss = insertRequest.getNamespace();

    bool bypassDocumentValidation =
        insertRequest.getWriteCommandRequestBase().getBypassDocumentValidation();

    EDCServerCollection::validateEncryptedFieldInfo(document, efc, bypassDocumentValidation);
    auto serverPayload = std::make_shared<std::vector<EDCServerPayloadInfo>>(
        EDCServerCollection::getEncryptedFieldInfo(document));

    validateInsertUpdatePayloads(opCtx, efc.getFields(), *serverPayload);

    std::shared_ptr<txn_api::SyncTransactionWithRetries> trun = getTxns(opCtx);

    // The function that handles the transaction may outlive this function so we need to use
    // shared_ptrs since it runs on another thread
    auto ownedDocument = document.getOwned();
    auto insertBlock = std::make_tuple(edcNss, efc, serverPayload, stmtId);
    auto sharedInsertBlock = std::make_shared<decltype(insertBlock)>(insertBlock);

    auto reply = std::make_shared<write_ops::InsertCommandReply>();
    auto service = opCtx->getService();

    auto baseStmtId = *stmtId;
    auto swResult = trun->runNoThrow(
        opCtx,
        [service, sharedInsertBlock, reply, ownedDocument, bypassDocumentValidation, baseStmtId](
            const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            FLEQueryInterfaceImpl queryImpl(txnClient, service);

            auto [edcNss2, efc2, serverPayload2, stmtId2] = *sharedInsertBlock.get();

            // Reset the statement ID in case of transient transaction retries
            *stmtId2 = baseStmtId;

            if (MONGO_unlikely(fleCrudHangPreInsert.shouldFail())) {
                LOGV2(6516701, "Hanging due to fleCrudHangPreInsert fail point");
                fleCrudHangPreInsert.pauseWhileSet();
            }

            *reply = uassertStatusOK(processInsert(&queryImpl,
                                                   edcNss2,
                                                   *serverPayload2.get(),
                                                   efc2,
                                                   stmtId2,
                                                   ownedDocument,
                                                   bypassDocumentValidation));

            if (MONGO_unlikely(fleCrudHangInsert.shouldFail())) {
                LOGV2(6371903, "Hanging due to fleCrudHangInsert fail point");
                fleCrudHangInsert.pauseWhileSet();
            }

            // If we have write errors but no unexpected internal errors, then we reach here
            // If we have write errors, we need to return a failed status to ensure the txn client
            // does not try to commit the transaction.
            if (reply->getWriteErrors().has_value() && !reply->getWriteErrors().value().empty()) {
                return SemiFuture<void>::makeReady(
                    Status(ErrorCodes::FLETransactionAbort,
                           "Queryable Encryption write errors on insert"));
            }

            return SemiFuture<void>::makeReady();
        });

    return {swResult, reply};
}

std::pair<FLEBatchResult, write_ops::InsertCommandReply> processInsert(
    OperationContext* opCtx,
    const write_ops::InsertCommandRequest& insertRequest,
    GetTxnCallback getTxns) {
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
    }

    auto documents = insertRequest.getDocuments();

    std::vector<write_ops::WriteError> writeErrors;
    WriteConcernErrorDetail wcError;
    int32_t stmtId = getStmtIdForWriteAt(insertRequest, 0);
    uint32_t iter = 0;
    uint32_t numDocs = 0;
    write_ops::WriteCommandReplyBase writeBase;
    std::vector<StmtId> retriedStmtIds;

    // This is an optimization for single-document unencrypted inserts.
    if (documents.size() == 1) {
        auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(documents[0]);
        if (serverPayload.size() == 0) {
            return std::pair<FLEBatchResult, write_ops::InsertCommandReply>{
                FLEBatchResult::kNotProcessed, write_ops::InsertCommandReply()};
        }
    }

    auto efc = EncryptionInformationHelpers::getAndValidateSchema(
        insertRequest.getNamespace(), insertRequest.getEncryptionInformation().value());

    for (auto& document : documents) {
        const auto& [swResult, reply] =
            insertSingleDocument(opCtx, insertRequest, document, efc, &stmtId, getTxns);

        writeBase.setElectionId(reply->getElectionId());

        if (!swResult.isOK()) {
            if (swResult.getStatus() == ErrorCodes::FLETransactionAbort) {
                // FLETransactionAbort is used for control flow so it means we have a valid
                // InsertCommandReply with write errors so we should return that.
                const auto& errors = reply->getWriteErrors().get();
                writeErrors.insert(writeErrors.end(), errors.begin(), errors.end());
            }
            writeErrors.push_back(write_ops::WriteError(iter, swResult.getStatus()));

            // If the request is ordered (inserts are ordered by default) we will return
            // early.
            if (insertRequest.getOrdered()) {
                break;
            }
        } else if (!swResult.getValue().getEffectiveStatus().isOK()) {
            auto& commitResult = swResult.getValue();

            if (commitResult.wcError.isValid(nullptr)) {
                commitResult.wcError.cloneTo(&wcError);
            }

            if (!commitResult.cmdStatus.isOK()) {
                writeErrors.push_back(
                    write_ops::WriteError(iter, swResult.getValue().getEffectiveStatus()));
                // If the request is ordered (inserts are ordered by default) we will return
                // early.
                if (insertRequest.getOrdered()) {
                    break;
                }
            } else {
                // If it gets here, then we merely have a write concern error.
                // The commit succeeded, only that the WC failed to be satisfied.
                numDocs++;
            }
        } else {
            if (auto& stmtIds = reply->getRetriedStmtIds()) {
                retriedStmtIds.insert(retriedStmtIds.end(), stmtIds->begin(), stmtIds->end());
            }
            numDocs++;
        }
        iter++;
    }

    write_ops::InsertCommandReply returnReply;

    writeBase.setN(numDocs);
    if (!writeErrors.empty()) {
        writeBase.setWriteErrors(writeErrors);
    }
    if (!retriedStmtIds.empty()) {
        writeBase.setRetriedStmtIds(std::move(retriedStmtIds));
    }
    appendPossibleWriteConcernErrorToReply(wcError, &writeBase);

    throwTransientTransactionErrorIfNeeded(wcError.isValid(nullptr), writeErrors);

    returnReply.setWriteCommandReplyBase(writeBase);

    return {FLEBatchResult::kProcessed, returnReply};
}

write_ops::DeleteCommandReply processDelete(OperationContext* opCtx,
                                            const write_ops::DeleteCommandRequest& deleteRequest,
                                            GetTxnCallback getTxns) {
    {
        auto deletes = deleteRequest.getDeletes();
        uassert(6371302, "Only single document deletes are permitted", deletes.size() == 1);

        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
    }

    std::shared_ptr<txn_api::SyncTransactionWithRetries> trun = getTxns(opCtx);

    auto reply = std::make_shared<write_ops::DeleteCommandReply>();

    auto ownedRequest = deleteRequest.serialize();

    if (gMultitenancySupport) {
        ownedRequest.validatedTenancyScope = auth::ValidatedTenancyScope::get(opCtx);
    }

    auto ownedDeleteRequest =
        write_ops::DeleteCommandRequest::parse(ownedRequest, IDLParserContext("delete"));
    // TODO: SERVER-90827 Only reset arguments not suitable for passing through to shards.
    ownedDeleteRequest.setGenericArguments({});

    auto ownedDeleteOpEntry = ownedDeleteRequest.getDeletes()[0];
    auto expCtx = makeExpCtx(opCtx, ownedDeleteRequest, ownedDeleteOpEntry);
    // The function that handles the transaction may outlive this function so we need to use
    // shared_ptrs
    auto deleteBlock = std::make_tuple(ownedDeleteRequest, expCtx);
    auto sharedDeleteBlock = std::make_shared<decltype(deleteBlock)>(deleteBlock);
    auto service = opCtx->getService();

    auto swResult = trun->runNoThrow(
        opCtx,
        [service, sharedDeleteBlock, ownedRequest, reply](
            const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            FLEQueryInterfaceImpl queryImpl(txnClient, service);

            auto [deleteRequest2, expCtx2] = *sharedDeleteBlock.get();

            if (MONGO_unlikely(fleCrudHangPreDelete.shouldFail())) {
                LOGV2(6516702, "Hanging due to fleCrudHangPreDelete fail point");
                fleCrudHangPreDelete.pauseWhileSet();
            }


            *reply = processDelete(&queryImpl, expCtx2, deleteRequest2);

            if (MONGO_unlikely(fleCrudHangDelete.shouldFail())) {
                LOGV2(6371902, "Hanging due to fleCrudHangDelete fail point");
                fleCrudHangDelete.pauseWhileSet();
            }

            // If we have write errors but no unexpected internal errors, then we reach here
            // If we have write errors, we need to return a failed status to ensure the txn client
            // does not try to commit the transaction.
            if (reply->getWriteErrors().has_value() && !reply->getWriteErrors().value().empty()) {
                return SemiFuture<void>::makeReady(
                    Status(ErrorCodes::FLETransactionAbort,
                           "Queryable Encryption write errors on delete"));
            }

            return SemiFuture<void>::makeReady();
        });

    bool hasWriteConcernError = false;

    if (!swResult.isOK()) {
        // FLETransactionAbort is used for control flow so it means we have a valid
        // InsertCommandReply with write errors so we should return that.
        if (swResult.getStatus() == ErrorCodes::FLETransactionAbort) {
            return *reply;
        }

        appendSingleStatusToWriteErrors(swResult.getStatus(), &reply->getWriteCommandReplyBase());
    } else if (!swResult.getValue().getEffectiveStatus().isOK()) {
        auto& commitResult = swResult.getValue();

        hasWriteConcernError = commitResult.wcError.isValid(nullptr);

        appendPossibleWriteConcernErrorToReply(commitResult.wcError,
                                               &reply->getWriteCommandReplyBase());
        if (!commitResult.cmdStatus.isOK()) {
            if (ErrorCodes::isA<ErrorCategory::WriteConcernError>(commitResult.cmdStatus)) {
                // On single-write-shard commits, the transaction API will abort the transaction and
                // return a CommitResult with one of the WriteConcernErrors if any of the read-only
                // shards fail to commit with the user requested write concern. This happens before
                // any commits to the write shards are performed, so none of the changes are
                // actually made durable.
                reply->setN(0);
            }
            appendSingleStatusToWriteErrors(commitResult.cmdStatus,
                                            &reply->getWriteCommandReplyBase());
        }
    }

    throwTransientTransactionErrorIfNeeded(hasWriteConcernError,
                                           reply->getWriteCommandReplyBase().getWriteErrors());

    return *reply;
}

write_ops::UpdateCommandReply processUpdate(OperationContext* opCtx,
                                            const write_ops::UpdateCommandRequest& updateRequest,
                                            GetTxnCallback getTxns) {

    {
        auto updates = updateRequest.getUpdates();
        uassert(6371502, "Only single document updates are permitted", updates.size() == 1);

        auto updateOpEntry = updates[0];

        uassert(6371503,
                "FLE only supports single document updates",
                updateOpEntry.getMulti() == false);

        // pipeline - is agg specific, delta is oplog, transform is internal (timeseries)
        uassert(6371517,
                "FLE only supports modifier and replacement style updates",
                updateOpEntry.getU().type() == write_ops::UpdateModification::Type::kModifier ||
                    updateOpEntry.getU().type() ==
                        write_ops::UpdateModification::Type::kReplacement);

        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
    }

    std::shared_ptr<txn_api::SyncTransactionWithRetries> trun = getTxns(opCtx);

    // The function that handles the transaction may outlive this function so we need to use
    // shared_ptrs
    auto reply = std::make_shared<write_ops::UpdateCommandReply>();

    auto ownedRequest = updateRequest.serialize();

    if (gMultitenancySupport) {
        ownedRequest.validatedTenancyScope = auth::ValidatedTenancyScope::get(opCtx);
    }

    auto ownedUpdateRequest =
        write_ops::UpdateCommandRequest::parse(ownedRequest, IDLParserContext("update"));
    // TODO: SERVER-90827 Only reset arguments not suitable for passing through to shards.
    ownedUpdateRequest.setGenericArguments({});
    auto ownedUpdateOpEntry = ownedUpdateRequest.getUpdates()[0];

    auto expCtx = makeExpCtx(opCtx, ownedUpdateRequest, ownedUpdateOpEntry);
    auto updateBlock = std::make_tuple(ownedUpdateRequest, expCtx);
    auto sharedupdateBlock = std::make_shared<decltype(updateBlock)>(updateBlock);
    auto service = opCtx->getService();

    auto swResult = trun->runNoThrow(
        opCtx,
        [service, sharedupdateBlock, reply, ownedRequest](
            const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            FLEQueryInterfaceImpl queryImpl(txnClient, service);

            auto [updateRequest2, expCtx2] = *sharedupdateBlock.get();

            if (MONGO_unlikely(fleCrudHangPreUpdate.shouldFail())) {
                LOGV2(6516703, "Hanging due to fleCrudHangPreUpdate fail point");
                fleCrudHangPreUpdate.pauseWhileSet();
            }

            *reply = processUpdate(&queryImpl, expCtx2, updateRequest2);

            if (MONGO_unlikely(fleCrudHangUpdate.shouldFail())) {
                LOGV2(6371901, "Hanging due to fleCrudHangUpdate fail point");
                fleCrudHangUpdate.pauseWhileSet();
            }

            // If we have write errors but no unexpected internal errors, then we reach here
            // If we have write errors, we need to return a failed status to ensure the txn client
            // does not try to commit the transaction.
            if (reply->getWriteErrors().has_value() && !reply->getWriteErrors().value().empty()) {
                return SemiFuture<void>::makeReady(
                    Status(ErrorCodes::FLETransactionAbort,
                           "Queryable Encryption write errors on update"));
            }

            return SemiFuture<void>::makeReady();
        });

    bool hasWriteConcernError = false;
    if (!swResult.isOK()) {
        // FLETransactionAbort is used for control flow so it means we have a valid
        // InsertCommandReply with write errors so we should return that.
        if (swResult.getStatus() == ErrorCodes::FLETransactionAbort) {
            return *reply;
        }

        appendSingleStatusToWriteErrors(swResult.getStatus(), &reply->getWriteCommandReplyBase());
    } else if (!swResult.getValue().getEffectiveStatus().isOK()) {
        auto& commitResult = swResult.getValue();

        hasWriteConcernError = commitResult.wcError.isValid(nullptr);

        appendPossibleWriteConcernErrorToReply(commitResult.wcError,
                                               &reply->getWriteCommandReplyBase());
        if (!commitResult.cmdStatus.isOK()) {
            if (ErrorCodes::isA<ErrorCategory::WriteConcernError>(commitResult.cmdStatus)) {
                // On single-write-shard commits, the transaction API will abort the transaction and
                // return a CommitResult with one of the WriteConcernErrors if any of the read-only
                // shards fail to commit with the user requested write concern. This happens before
                // any commits to the write shards are performed, so none of the changes are
                // actually made durable.
                reply->setNModified(0);
                reply->setN(0);
            }
            appendSingleStatusToWriteErrors(commitResult.cmdStatus,
                                            &reply->getWriteCommandReplyBase());
        }
    }

    throwTransientTransactionErrorIfNeeded(hasWriteConcernError,
                                           reply->getWriteCommandReplyBase().getWriteErrors());


    return *reply;
}

namespace {

template <class T>
FLEEdgePrfBlock toFLEEdgePrfBlock(const T& ts, const FLEEdgePrfBlock* prev = nullptr) {
    FLEEdgePrfBlock blk;
    blk.esc = ts.getEscDerivedToken().asPrfBlock();
    if (prev && blk.esc == prev->esc) {
        blk.paddingIndex = prev->paddingIndex + 1;
    }
    return blk;
}

void processFieldsForInsert(FLEQueryInterface* queryImpl,
                            const NamespaceString& edcNss,
                            std::vector<EDCServerPayloadInfo>& serverPayload,
                            const EncryptedFieldConfig& efc,
                            int32_t* pStmtId,
                            bool bypassDocumentValidation) {
    if (serverPayload.empty()) {
        return;
    }

    const NamespaceString nssEsc =
        NamespaceStringUtil::deserialize(edcNss.dbName(), efc.getEscCollection().value());

    size_t totalTokens = 0;

    std::vector<std::vector<FLEEdgePrfBlock>> tokensSets;
    tokensSets.reserve(serverPayload.size());

    for (auto& payload : serverPayload) {
        payload.counts.clear();

        if (payload.isRangePayload()) {
            const auto& edgeTokenSet = payload.payload.getEdgeTokenSet().get();

            tokensSets.push_back({});
            auto& tokens = tokensSets.back();
            tokens.reserve(edgeTokenSet.size());

            for (const auto& et : edgeTokenSet) {
                tokens.emplace_back(toFLEEdgePrfBlock(et));
            }
            totalTokens += edgeTokenSet.size();
        } else if (payload.isTextSearchPayload()) {
            const auto& tsts = payload.payload.getTextSearchTokenSets().get();

            tokensSets.push_back({});
            auto& tokens = tokensSets.back();
            size_t tokenCount = payload.getTotalTextSearchTokenSetCount();
            tokens.reserve(tokenCount);

            // The order of appending tokens below is important
            tokens.emplace_back(toFLEEdgePrfBlock(tsts.getExactTokenSet()));

            // identical tokens are guaranteed to be next to each other in the array
            FLEEdgePrfBlock* prev = nullptr;
            for (const auto& ts : tsts.getSubstringTokenSets()) {
                prev = &tokens.emplace_back(toFLEEdgePrfBlock(ts, prev));
            }

            prev = nullptr;
            for (const auto& ts : tsts.getSuffixTokenSets()) {
                prev = &tokens.emplace_back(toFLEEdgePrfBlock(ts, prev));
            }

            prev = nullptr;
            for (const auto& ts : tsts.getPrefixTokenSets()) {
                prev = &tokens.emplace_back(toFLEEdgePrfBlock(ts, prev));
            }
            dassert(tokens.size() == tokenCount);
            totalTokens += tokenCount;
        } else {
            tokensSets.push_back({toFLEEdgePrfBlock(payload.payload)});
            totalTokens++;
        }
    }

    auto countInfoSets =
        queryImpl->getTags(nssEsc, tokensSets, FLETagQueryInterface::TagQueryType::kInsert);

    uassert(7415101,
            "Mismatch in the number of expected tokens",
            countInfoSets.size() == serverPayload.size());

    std::vector<BSONObj> escDocuments;
    escDocuments.reserve(totalTokens);

    HmacContext hmacCtx;

    for (size_t i = 0; i < countInfoSets.size(); i++) {
        auto& countInfos = countInfoSets[i];

        uassert(7415104,
                "Mismatch in the number of expected counts for a token",
                countInfos.size() == tokensSets[i].size());

        // each countInfo is returned from getTags with the "count" resulting from emuBinary,
        // the "tagTokenData" which is the "tag" token derived from the ESC data & cf token,
        for (size_t tokenIndex = 0; tokenIndex < countInfos.size(); tokenIndex++) {
            const auto& countInfo = countInfos.at(tokenIndex);
            auto count = countInfo.count + tokensSets[i][tokenIndex].paddingIndex;

            serverPayload[i].counts.push_back(count);

            escDocuments.push_back(ESCCollection::generateNonAnchorDocument(
                &hmacCtx, ESCTwiceDerivedTagToken(countInfo.tagTokenData), count));
        }
    }

    auto escInsertReply =
        uassertStatusOK(queryImpl->insertDocuments(nssEsc, escDocuments, pStmtId, true));
    checkWriteErrors(escInsertReply);

    NamespaceString nssEcoc =
        NamespaceStringUtil::deserialize(edcNss.dbName(), efc.getEcocCollection().value());
    std::vector<BSONObj> ecocDocuments;
    ecocDocuments.reserve(totalTokens);

    for (auto& payload : serverPayload) {
        if (payload.isRangePayload()) {
            const auto& edgeTokenSet = payload.payload.getEdgeTokenSet().get();

            for (const auto& et : edgeTokenSet) {
                ecocDocuments.push_back(
                    et.getEncryptedTokens().generateDocument(payload.fieldPathName));
            }
        } else if (payload.isTextSearchPayload()) {
            const auto& tsts = payload.payload.getTextSearchTokenSets().get();
            auto exactSet = tsts.getExactTokenSet();
            exactSet.getEncryptedTokens().assertIsValidForTextSearch();
            ecocDocuments.push_back(
                exactSet.getEncryptedTokens().generateDocument(payload.fieldPathName));
            for (const auto& ts : tsts.getSubstringTokenSets()) {
                ts.getEncryptedTokens().assertIsValidForTextSearch();
                ecocDocuments.push_back(
                    ts.getEncryptedTokens().generateDocument(payload.fieldPathName));
            }
            for (const auto& ts : tsts.getSuffixTokenSets()) {
                ts.getEncryptedTokens().assertIsValidForTextSearch();
                ecocDocuments.push_back(
                    ts.getEncryptedTokens().generateDocument(payload.fieldPathName));
            }
            for (const auto& ts : tsts.getPrefixTokenSets()) {
                ts.getEncryptedTokens().assertIsValidForTextSearch();
                ecocDocuments.push_back(
                    ts.getEncryptedTokens().generateDocument(payload.fieldPathName));
            }
        } else {
            ecocDocuments.push_back(
                payload.payload.getEncryptedTokens().generateDocument(payload.fieldPathName));
        }
    }

    auto ecocInsertReply = uassertStatusOK(queryImpl->insertDocuments(
        nssEcoc, ecocDocuments, pStmtId, false, bypassDocumentValidation));
    checkWriteErrors(ecocInsertReply);
}

template <typename ReplyType>
std::shared_ptr<ReplyType> constructDefaultReply() {
    return std::make_shared<ReplyType>();
}

template <>
std::shared_ptr<write_ops::FindAndModifyCommandRequest> constructDefaultReply() {
    return std::make_shared<write_ops::FindAndModifyCommandRequest>(NamespaceString::kEmpty);
}

template <typename ReplyType>
void addWriteConcernErrorInfoToReply(const WriteConcernErrorDetail& wce, ReplyType* reply) {
    return;
}
template <>
void addWriteConcernErrorInfoToReply(const WriteConcernErrorDetail& wce,
                                     write_ops::FindAndModifyCommandReply* reply) {
    if (wce.isValid(nullptr)) {
        reply->setWriteConcernError(wce.toBSON());
    }
}

/**
 * Extracts update payloads from a {findAndModify: nss, ...} request,
 * and proxies to `validateInsertUpdatePayload()`.
 */
void validateFindAndModifyRequest(OperationContext* opCtx,
                                  const write_ops::FindAndModifyCommandRequest& request) {
    // Is this a delete?
    const bool isDelete = request.getRemove().value_or(false);

    // User can only specify either remove = true or update != {}
    uassert(6371401,
            "Must specify either update or remove to findAndModify, not both",
            !(request.getUpdate().has_value() && isDelete));

    uassert(6371402,
            "findAndModify with encryption only supports new: false",
            request.getNew().value_or(false) == false);

    uassert(6371408,
            "findAndModify fields must be empty",
            request.getFields().value_or(BSONObj()).isEmpty());

    // pipeline - is agg specific, delta is oplog, transform is internal (timeseries)
    auto updateMod = request.getUpdate().get_value_or({});
    const auto updateModicationType = updateMod.type();

    uassert(6439901,
            "FLE only supports modifier and replacement style updates",
            updateModicationType == write_ops::UpdateModification::Type::kModifier ||
                updateModicationType == write_ops::UpdateModification::Type::kReplacement);

    auto nss = request.getNamespace();
    auto ei = request.getEncryptionInformation().get();
    auto efc = EncryptionInformationHelpers::getAndValidateSchema(nss, ei);

    BSONObj update;
    if (updateMod.type() == write_ops::UpdateModification::Type::kReplacement) {
        update = updateMod.getUpdateReplacement();
    } else {
        invariant(updateMod.type() == write_ops::UpdateModification::Type::kModifier);
        update = updateMod.getUpdateModifier().getObjectField("$set"_sd);
    }

    if (!update.firstElement().eoo()) {
        auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(update);
        validateInsertUpdatePayloads(opCtx, efc.getFields(), serverPayload);
    }
}

}  // namespace

template <typename ReplyType>
StatusWith<std::pair<ReplyType, OpMsgRequest>> processFindAndModifyRequest(
    OperationContext* opCtx,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest,
    GetTxnCallback getTxns,
    ProcessFindAndModifyCallback<ReplyType> processCallback,
    ErrorWithWriteConcernErrorCallback wceCallback) {

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
    }

    validateFindAndModifyRequest(opCtx, findAndModifyRequest);

    std::shared_ptr<txn_api::SyncTransactionWithRetries> trun = getTxns(opCtx);

    // The function that handles the transaction may outlive this function so we need to use
    // shared_ptrs
    std::shared_ptr<ReplyType> reply = constructDefaultReply<ReplyType>();


    auto ownedRequest = findAndModifyRequest.serialize();

    if (gMultitenancySupport) {
        ownedRequest.validatedTenancyScope = auth::ValidatedTenancyScope::get(opCtx);
    }

    auto ownedFindAndModifyRequest = write_ops::FindAndModifyCommandRequest::parse(
        ownedRequest, IDLParserContext("findAndModify"));
    // TODO: SERVER-90827 Only reset arguments not suitable for passing through to shards.
    auto wc = ownedFindAndModifyRequest.getWriteConcern();
    ownedFindAndModifyRequest.setGenericArguments({});
    ownedFindAndModifyRequest.setWriteConcern(std::move(wc));

    auto expCtx = makeExpCtx(opCtx, ownedFindAndModifyRequest, ownedFindAndModifyRequest);
    auto findAndModifyBlock = std::make_tuple(ownedFindAndModifyRequest, expCtx);
    auto sharedFindAndModifyBlock =
        std::make_shared<decltype(findAndModifyBlock)>(findAndModifyBlock);
    auto service = opCtx->getService();

    auto swResult = trun->runNoThrow(
        opCtx,
        [service, sharedFindAndModifyBlock, ownedRequest, reply, processCallback](
            const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            FLEQueryInterfaceImpl queryImpl(txnClient, service);

            auto [findAndModifyRequest2, expCtx] = *sharedFindAndModifyBlock.get();

            if (MONGO_unlikely(fleCrudHangPreFindAndModify.shouldFail())) {
                LOGV2(6516704, "Hanging due to fleCrudHangPreFindAndModify fail point");
                fleCrudHangPreFindAndModify.pauseWhileSet();
            }

            *reply = processCallback(expCtx, &queryImpl, findAndModifyRequest2);

            if (MONGO_unlikely(fleCrudHangFindAndModify.shouldFail())) {
                LOGV2(6371900, "Hanging due to fleCrudHangFindAndModify fail point");
                fleCrudHangFindAndModify.pauseWhileSet();
            }

            return SemiFuture<void>::makeReady();
        });

    if (!swResult.isOK()) {
        return swResult.getStatus();
    } else if (!swResult.getValue().getEffectiveStatus().isOK()) {
        auto& commitResult = swResult.getValue();

        if (commitResult.cmdStatus.isOK()) {
            // commit encountered a write concern error, but succeeded with the write
            addWriteConcernErrorInfoToReply(commitResult.wcError, reply.get());
        } else {
            // commit encountered an error, and maybe a write concern error as well
            if (commitResult.wcError.isValid(nullptr) && wceCallback) {
                wceCallback(commitResult.wcError);
            }
            return commitResult.cmdStatus;
        }
    }

    return std::pair<ReplyType, OpMsgRequest>{*reply, ownedRequest};
}

template StatusWith<std::pair<write_ops::FindAndModifyCommandReply, OpMsgRequest>>
processFindAndModifyRequest<write_ops::FindAndModifyCommandReply>(
    OperationContext* opCtx,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest,
    GetTxnCallback getTxns,
    ProcessFindAndModifyCallback<write_ops::FindAndModifyCommandReply> processCallback,
    ErrorWithWriteConcernErrorCallback wceCallback);

template StatusWith<std::pair<write_ops::FindAndModifyCommandRequest, OpMsgRequest>>
processFindAndModifyRequest<write_ops::FindAndModifyCommandRequest>(
    OperationContext* opCtx,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest,
    GetTxnCallback getTxns,
    ProcessFindAndModifyCallback<write_ops::FindAndModifyCommandRequest> processCallback,
    ErrorWithWriteConcernErrorCallback wceCallback);

StatusWith<write_ops::InsertCommandReply> processInsert(
    FLEQueryInterface* queryImpl,
    const NamespaceString& edcNss,
    std::vector<EDCServerPayloadInfo>& serverPayload,
    const EncryptedFieldConfig& efc,
    int32_t* stmtId,
    BSONObj document,
    bool bypassDocumentValidation) {

    if (serverPayload.empty()) {
        return queryImpl->insertDocuments(edcNss, {document}, stmtId, false);
    }

    processFieldsForInsert(queryImpl, edcNss, serverPayload, efc, stmtId, bypassDocumentValidation);

    auto finalDoc = EDCServerCollection::finalizeForInsert(document, serverPayload);

    return queryImpl->insertDocuments(edcNss, {finalDoc}, stmtId, false);
}

write_ops::DeleteCommandReply processDelete(FLEQueryInterface* queryImpl,
                                            boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const write_ops::DeleteCommandRequest& deleteRequest) {

    auto edcNss = deleteRequest.getNamespace();
    auto ei = deleteRequest.getEncryptionInformation().value();

    auto efc = EncryptionInformationHelpers::getAndValidateSchema(edcNss, ei);

    int32_t stmtId = getStmtIdForWriteAt(deleteRequest, 0);

    auto newDeleteRequest = deleteRequest;

    auto newDeleteOp = newDeleteRequest.getDeletes()[0];
    newDeleteOp.setQ(
        fle::rewriteEncryptedFilterInsideTxn(queryImpl, edcNss, efc, expCtx, newDeleteOp.getQ()));

    newDeleteRequest.setDeletes({newDeleteOp});

    newDeleteRequest.getWriteCommandRequestBase().setStmtIds(boost::none);

    newDeleteRequest.getWriteCommandRequestBase().setStmtId(boost::none);
    newDeleteRequest.getWriteCommandRequestBase().setEncryptionInformation(boost::none);

    auto deleteReply = queryImpl->deleteDocument(edcNss, stmtId, newDeleteRequest);
    checkWriteErrors(deleteReply);

    return deleteReply;
}

bool hasIndexedFieldsInSchema(const std::vector<EncryptedField>& fields) {
    for (const auto& field : fields) {
        if (field.getQueries().has_value()) {
            const auto& queries = field.getQueries().get();
            if (holds_alternative<std::vector<mongo::QueryTypeConfig>>(queries)) {
                const auto& vec = get<0>(queries);
                if (!vec.empty()) {
                    return true;
                }
            } else {
                return true;
            }
        }
    }
    return false;
}

/**
 * Update is the most complicated FLE operation.
 * It is basically an insert followed by a delete, sort of.
 *
 * 1. Process the update for any encrypted fields like insert, update the ESC and get new counters
 * 2. Extend the update $push new tags into the document
 * 3. Run the update with findAndModify to get the pre-image
 * 4. Run a find to get the post-image update with the id from the pre-image
 * -- Fail if we cannot find the new document. This could happen if they updated _id.
 * 5. Find the removed fields
 * 6. Remove the stale tags from the original document with a new push
 */
write_ops::UpdateCommandReply processUpdate(FLEQueryInterface* queryImpl,
                                            boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const write_ops::UpdateCommandRequest& updateRequest) {

    auto edcNss = updateRequest.getNamespace();
    auto ei = updateRequest.getEncryptionInformation().value();

    auto efc = EncryptionInformationHelpers::getAndValidateSchema(edcNss, ei);

    const auto updateOpEntry = updateRequest.getUpdates()[0];

    auto bypassDocumentValidation =
        updateRequest.getWriteCommandRequestBase().getBypassDocumentValidation();

    const auto updateModification = updateOpEntry.getU();

    int32_t stmtId = getStmtIdForWriteAt(updateRequest, 0);

    // Step 1 ----
    std::vector<EDCServerPayloadInfo> serverPayload;
    auto newUpdateOpEntry = updateRequest.getUpdates()[0];

    auto encryptedCollScanModeAllowed = newUpdateOpEntry.getUpsert()
        ? fle::EncryptedCollScanModeAllowed::kDisallow
        : fle::EncryptedCollScanModeAllowed::kAllow;

    newUpdateOpEntry.setQ(fle::rewriteEncryptedFilterInsideTxn(
        queryImpl, edcNss, efc, expCtx, newUpdateOpEntry.getQ(), encryptedCollScanModeAllowed));

    if (updateModification.type() == write_ops::UpdateModification::Type::kModifier) {
        auto updateModifier = updateModification.getUpdateModifier();
        auto setObject = updateModifier.getObjectField("$set");
        EDCServerCollection::validateEncryptedFieldInfo(setObject, efc, bypassDocumentValidation);
        serverPayload = EDCServerCollection::getEncryptedFieldInfo(setObject);
        validateInsertUpdatePayloads(expCtx->getOperationContext(), efc.getFields(), serverPayload);

        processFieldsForInsert(
            queryImpl, edcNss, serverPayload, efc, &stmtId, bypassDocumentValidation);

        // Step 2 ----
        auto pushUpdate = EDCServerCollection::finalizeForUpdate(updateModifier, serverPayload);

        newUpdateOpEntry.setU(mongo::write_ops::UpdateModification(
            pushUpdate, write_ops::UpdateModification::ModifierUpdateTag{}));
    } else {
        auto replacementDocument = updateModification.getUpdateReplacement();
        EDCServerCollection::validateEncryptedFieldInfo(
            replacementDocument, efc, bypassDocumentValidation);
        serverPayload = EDCServerCollection::getEncryptedFieldInfo(replacementDocument);
        validateInsertUpdatePayloads(expCtx->getOperationContext(), efc.getFields(), serverPayload);

        processFieldsForInsert(
            queryImpl, edcNss, serverPayload, efc, &stmtId, bypassDocumentValidation);

        // Step 2 ----
        auto safeContentReplace =
            EDCServerCollection::finalizeForInsert(replacementDocument, serverPayload);

        newUpdateOpEntry.setU(mongo::write_ops::UpdateModification(
            safeContentReplace, write_ops::UpdateModification::ReplacementTag{}));
    }

    // Step 3 ----
    auto newUpdateRequest = updateRequest;
    newUpdateRequest.setUpdates({newUpdateOpEntry});
    newUpdateRequest.getWriteCommandRequestBase().setStmtIds(boost::none);
    newUpdateRequest.getWriteCommandRequestBase().setStmtId(stmtId);
    newUpdateRequest.getWriteCommandRequestBase().setBypassDocumentValidation(
        bypassDocumentValidation);

    ++stmtId;

    auto [updateReply, originalDocument] =
        queryImpl->updateWithPreimage(edcNss, ei, newUpdateRequest);
    if (originalDocument.isEmpty()) {
        // if there is no preimage, then we did not update any documents, we are done
        return updateReply;
    }

    // If this is a retried write, we are done
    if (updateReply.getWriteCommandReplyBase().getRetriedStmtIds().has_value()) {
        return updateReply;
    }

    // If there are errors, we are done
    if (updateReply.getWriteErrors().has_value() && !updateReply.getWriteErrors().value().empty()) {
        return updateReply;
    }

    // Validate that the original document does not contain values with on-disk version
    // incompatible with the current protocol version.
    EDCServerCollection::validateModifiedDocumentCompatibility(originalDocument);

    // Step 4 ----
    auto idElement = originalDocument.firstElement();
    uassert(6371504,
            "Missing _id field in pre-image document",
            idElement.fieldNameStringData() == "_id"_sd);
    BSONObj newDocument = queryImpl->getById(edcNss, idElement);

    // Fail if we could not find the new document
    uassert(6371505, "Could not find pre-image document by _id", !newDocument.isEmpty());

    // Step 5 ----
    auto originalFields = EDCServerCollection::getEncryptedIndexedFields(originalDocument);
    auto newFields = EDCServerCollection::getEncryptedIndexedFields(newDocument);

    if (hasIndexedFieldsInSchema(efc.getFields()) && !(newFields.empty())) {
        // Check the user did not remove/destroy the __safeContent__ array. If there are no
        // indexed fields, then there will not be a safeContent array in the document.
        FLEClientCrypto::validateTagsArray(newDocument);
    }

    // Step 6 ----
    // GarbageCollect steps:
    //  1. Gather the tags from the metadata block(s) of each removed field. These are stale tags.
    //  2. Generate the update command that pulls the stale tags from __safeContent__
    //  3. Perform the update
    auto staleTags = EDCServerCollection::getRemovedTags(originalFields, newFields);

    if (!staleTags.empty()) {
        BSONObj pullUpdate = EDCServerCollection::generateUpdateToRemoveTags(staleTags);
        auto pullUpdateOpEntry = write_ops::UpdateOpEntry();
        pullUpdateOpEntry.setUpsert(false);
        pullUpdateOpEntry.setMulti(false);
        pullUpdateOpEntry.setQ(BSON("_id"_sd << idElement));
        pullUpdateOpEntry.setU(mongo::write_ops::UpdateModification(
            pullUpdate, write_ops::UpdateModification::ModifierUpdateTag{}));
        newUpdateRequest.setUpdates({pullUpdateOpEntry});
        newUpdateRequest.getWriteCommandRequestBase().setStmtId(boost::none);
        newUpdateRequest.setLegacyRuntimeConstants(boost::none);
        newUpdateRequest.getWriteCommandRequestBase().setEncryptionInformation(boost::none);
        /* ignore */ queryImpl->update(edcNss, stmtId, newUpdateRequest);
    }

    return updateReply;
}

FLEBatchResult processFLEBatch(OperationContext* opCtx,
                               const BatchedCommandRequest& request,
                               BatchedCommandResponse* response) {
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
    }

    if (request.getWriteCommandRequestBase().getEncryptionInformation()->getCrudProcessed()) {
        return FLEBatchResult::kNotProcessed;
    }

    if (request.getBatchType() == BatchedCommandRequest::BatchType_Insert) {
        auto insertRequest = request.getInsertRequest();

        auto [batchResult, insertReply] =
            processInsert(opCtx, insertRequest, &getTransactionWithRetriesForMongoS);
        if (batchResult == FLEBatchResult::kNotProcessed) {
            return FLEBatchResult::kNotProcessed;
        }

        replyToResponse(opCtx, &insertReply.getWriteCommandReplyBase(), response);

        return FLEBatchResult::kProcessed;
    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Delete) {

        auto deleteRequest = request.getDeleteRequest();

        auto deleteReply = processDelete(opCtx, deleteRequest, &getTransactionWithRetriesForMongoS);

        replyToResponse(opCtx, &deleteReply.getWriteCommandReplyBase(), response);
        return FLEBatchResult::kProcessed;

    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Update) {

        auto updateRequest = request.getUpdateRequest();

        auto updateReply = processUpdate(opCtx, updateRequest, &getTransactionWithRetriesForMongoS);

        replyToResponse(opCtx, &updateReply.getWriteCommandReplyBase(), response);

        response->setNModified(updateReply.getNModified());

        if (updateReply.getUpserted().has_value() && updateReply.getUpserted().value().size() > 0) {

            auto upsertReply = updateReply.getUpserted().value()[0];

            BatchedUpsertDetail upsert;
            upsert.setIndex(upsertReply.getIndex());
            upsert.setUpsertedID(upsertReply.get_id().getElement().wrap(""));

            std::vector<BatchedUpsertDetail*> upserts;
            upserts.push_back(&upsert);

            response->setUpsertDetails(upserts);
        }

        return FLEBatchResult::kProcessed;
    }

    MONGO_UNREACHABLE;
}

std::unique_ptr<BatchedCommandRequest> processFLEBatchExplain(
    OperationContext* opCtx, const BatchedCommandRequest& request) {
    invariant(request.hasEncryptionInformation());
    auto getExpCtx = [&](const auto& op) {
        auto expCtx =
            ExpressionContextBuilder{}
                .opCtx(opCtx)
                .collator(fle::collatorFromBSON(opCtx, op.getCollation().value_or(BSONObj())))
                .ns(request.getNS())
                .runtimeConstants(request.getLegacyRuntimeConstants())
                .letParameters(request.getLet())
                .build();
        expCtx->stopExpressionCounters();
        return expCtx;
    };

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
    }

    if (request.getBatchType() == BatchedCommandRequest::BatchType_Delete) {
        auto deleteRequest = request.getDeleteRequest();
        auto newDeleteOp = deleteRequest.getDeletes()[0];
        newDeleteOp.setQ(fle::rewriteQuery(opCtx,
                                           getExpCtx(newDeleteOp),
                                           request.getNS(),
                                           deleteRequest.getEncryptionInformation().value(),
                                           newDeleteOp.getQ(),
                                           &getTransactionWithRetriesForMongoS,
                                           fle::EncryptedCollScanModeAllowed::kAllow));
        deleteRequest.setDeletes({newDeleteOp});
        deleteRequest.getWriteCommandRequestBase().setEncryptionInformation(
            makeEmptyProcessEncryptionInformation());

        return std::make_unique<BatchedCommandRequest>(deleteRequest);
    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Update) {
        auto updateRequest = request.getUpdateRequest();
        auto newUpdateOp = updateRequest.getUpdates()[0];
        auto encryptedCollScanModeAllowed = newUpdateOp.getUpsert()
            ? fle::EncryptedCollScanModeAllowed::kDisallow
            : fle::EncryptedCollScanModeAllowed::kAllow;

        newUpdateOp.setQ(fle::rewriteQuery(opCtx,
                                           getExpCtx(newUpdateOp),
                                           request.getNS(),
                                           updateRequest.getEncryptionInformation().value(),
                                           newUpdateOp.getQ(),
                                           &getTransactionWithRetriesForMongoS,
                                           encryptedCollScanModeAllowed));
        updateRequest.setUpdates({newUpdateOp});
        updateRequest.getWriteCommandRequestBase().setEncryptionInformation(
            makeEmptyProcessEncryptionInformation());
        return std::make_unique<BatchedCommandRequest>(updateRequest);
    }
    MONGO_UNREACHABLE;
}

// See processUpdate for algorithm overview
write_ops::FindAndModifyCommandReply processFindAndModify(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    FLEQueryInterface* queryImpl,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) {

    {
        stdx::lock_guard<Client> lk(*expCtx->getOperationContext()->getClient());
        CurOp::get(expCtx->getOperationContext())->setShouldOmitDiagnosticInformation(lk, true);
    }

    auto edcNss = findAndModifyRequest.getNamespace();
    auto ei = findAndModifyRequest.getEncryptionInformation().value();

    auto efc = EncryptionInformationHelpers::getAndValidateSchema(edcNss, ei);

    int32_t stmtId = findAndModifyRequest.getStmtId().value_or(0);

    auto newFindAndModifyRequest = findAndModifyRequest;

    const auto bypassDocumentValidation =
        findAndModifyRequest.getBypassDocumentValidation().value_or(false);

    // Step 0 ----
    // Rewrite filter
    auto encryptedCollScanModeAllowed = findAndModifyRequest.getUpsert().value_or(false)
        ? fle::EncryptedCollScanModeAllowed::kDisallow
        : fle::EncryptedCollScanModeAllowed::kAllow;

    newFindAndModifyRequest.setQuery(
        fle::rewriteEncryptedFilterInsideTxn(queryImpl,
                                             edcNss,
                                             efc,
                                             expCtx,
                                             findAndModifyRequest.getQuery(),
                                             encryptedCollScanModeAllowed));

    // Make sure not to inherit the command's writeConcern, this should be set at the transaction
    // level.
    newFindAndModifyRequest.setWriteConcern(boost::none);

    // Step 1 ----
    // If we have an update object, we have to process for ESC
    if (findAndModifyRequest.getUpdate().has_value()) {

        std::vector<EDCServerPayloadInfo> serverPayload;
        const auto updateModification = findAndModifyRequest.getUpdate().value();
        write_ops::UpdateModification newUpdateModification;

        if (updateModification.type() == write_ops::UpdateModification::Type::kModifier) {
            auto updateModifier = updateModification.getUpdateModifier();
            auto setObject = updateModifier.getObjectField("$set");
            EDCServerCollection::validateEncryptedFieldInfo(
                setObject, efc, bypassDocumentValidation);
            serverPayload = EDCServerCollection::getEncryptedFieldInfo(setObject);
            processFieldsForInsert(
                queryImpl, edcNss, serverPayload, efc, &stmtId, bypassDocumentValidation);

            auto pushUpdate = EDCServerCollection::finalizeForUpdate(updateModifier, serverPayload);

            // Step 2 ----
            newUpdateModification = write_ops::UpdateModification(
                pushUpdate, write_ops::UpdateModification::ModifierUpdateTag{});
        } else {
            auto replacementDocument = updateModification.getUpdateReplacement();
            EDCServerCollection::validateEncryptedFieldInfo(
                replacementDocument, efc, bypassDocumentValidation);
            serverPayload = EDCServerCollection::getEncryptedFieldInfo(replacementDocument);

            processFieldsForInsert(
                queryImpl, edcNss, serverPayload, efc, &stmtId, bypassDocumentValidation);

            // Step 2 ----
            auto safeContentReplace =
                EDCServerCollection::finalizeForInsert(replacementDocument, serverPayload);

            newUpdateModification = write_ops::UpdateModification(
                safeContentReplace, write_ops::UpdateModification::ReplacementTag{});
        }

        // Step 3 ----
        newFindAndModifyRequest.setUpdate(newUpdateModification);
    }

    newFindAndModifyRequest.setNew(false);
    newFindAndModifyRequest.setStmtId(stmtId);
    ++stmtId;

    auto reply = queryImpl->findAndModify(edcNss, ei, newFindAndModifyRequest);
    if (!reply.getValue().has_value() || reply.getValue().value().isEmpty()) {
        // if there is no preimage, then we did not update or delete any documents, we are done
        return reply;
    }

    // If this is a retried write, we are done
    if (reply.getRetriedStmtId()) {
        return reply;
    }

    // Step 4 ----
    BSONObj originalDocument = reply.getValue().value();
    auto idElement = originalDocument.firstElement();
    uassert(6371403,
            "Missing _id field in pre-image document, the fields document must contain _id",
            idElement.fieldNameStringData() == "_id"_sd);

    // Is this a delete? If so, there's no need to GarbageCollect.
    if (findAndModifyRequest.getRemove().value_or(false)) {
        return reply;
    }

    // Validate that the original document does not contain values with on-disk version
    // incompatible with the current protocol version.
    EDCServerCollection::validateModifiedDocumentCompatibility(originalDocument);

    auto newDocument = queryImpl->getById(edcNss, idElement);

    // Fail if we could not find the new document
    uassert(7293302, "Could not find pre-image document by _id", !newDocument.isEmpty());

    // Step 5 ----
    auto originalFields = EDCServerCollection::getEncryptedIndexedFields(originalDocument);
    auto newFields = EDCServerCollection::getEncryptedIndexedFields(newDocument);

    if (hasIndexedFieldsInSchema(efc.getFields()) && !(newFields.empty())) {
        // Check the user did not remove/destroy the __safeContent__ array. If there are no
        // indexed fields, then there will not be a safeContent array in the document.
        FLEClientCrypto::validateTagsArray(newDocument);
    }

    // Step 6 ----
    // GarbageCollect steps:
    //  1. Gather the tags from the metadata block(s) of each removed field. These are stale tags.
    //  2. Generate the update command that pulls the stale tags from __safeContent__
    //  3. Perform the update
    auto staleTags = EDCServerCollection::getRemovedTags(originalFields, newFields);

    if (!staleTags.empty()) {
        BSONObj pullUpdate = EDCServerCollection::generateUpdateToRemoveTags(staleTags);
        auto newUpdateRequest =
            write_ops::UpdateCommandRequest(findAndModifyRequest.getNamespace());
        auto pullUpdateOpEntry = write_ops::UpdateOpEntry();
        pullUpdateOpEntry.setUpsert(false);
        pullUpdateOpEntry.setMulti(false);
        pullUpdateOpEntry.setQ(BSON("_id"_sd << idElement));
        pullUpdateOpEntry.setU(mongo::write_ops::UpdateModification(
            pullUpdate, write_ops::UpdateModification::ModifierUpdateTag{}));
        newUpdateRequest.setUpdates({pullUpdateOpEntry});
        newUpdateRequest.setLegacyRuntimeConstants(boost::none);
        newUpdateRequest.getWriteCommandRequestBase().setStmtId(boost::none);
        newUpdateRequest.getWriteCommandRequestBase().setEncryptionInformation(boost::none);

        auto finalUpdateReply = queryImpl->update(edcNss, stmtId, newUpdateRequest);
        checkWriteErrors(finalUpdateReply);
    }

    return reply;
}

write_ops::FindAndModifyCommandRequest processFindAndModifyExplain(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    FLEQueryInterface* queryImpl,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) {

    auto edcNss = findAndModifyRequest.getNamespace();
    auto ei = findAndModifyRequest.getEncryptionInformation().value();

    auto efc = EncryptionInformationHelpers::getAndValidateSchema(edcNss, ei);

    auto newFindAndModifyRequest = findAndModifyRequest;
    auto encryptedCollScanModeAllowed = findAndModifyRequest.getUpsert().value_or(false)
        ? fle::EncryptedCollScanModeAllowed::kDisallow
        : fle::EncryptedCollScanModeAllowed::kAllow;

    newFindAndModifyRequest.setQuery(
        fle::rewriteEncryptedFilterInsideTxn(queryImpl,
                                             edcNss,
                                             efc,
                                             expCtx,
                                             findAndModifyRequest.getQuery(),
                                             encryptedCollScanModeAllowed));

    newFindAndModifyRequest.setEncryptionInformation(makeEmptyProcessEncryptionInformation());
    return newFindAndModifyRequest;
}

FLEBatchResult processFLEFindAndModify(OperationContext* opCtx,
                                       const BSONObj& cmdObj,
                                       BSONObjBuilder& result) {
    // There is no findAndModify parsing in mongos so we need to first parse to decide if it is for
    // FLE2
    auto request =
        write_ops::FindAndModifyCommandRequest::parse(cmdObj, IDLParserContext("findAndModify"));

    if (!request.getEncryptionInformation().has_value()) {
        return FLEBatchResult::kNotProcessed;
    }

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
    }

    // FLE2 Mongos CRUD operations loopback through MongoS with EncryptionInformation as
    // findAndModify so query can do any necessary transformations. But on the nested call, CRUD
    // does not need to do any more work.
    if (request.getEncryptionInformation()->getCrudProcessed()) {
        return FLEBatchResult::kNotProcessed;
    }

    // This callback ensures that any write concern errors are set in the reply in the event
    // that processFindAndModifyRequest returned a non-OK status, which is then thrown.
    auto onErrorWithWCE = [&result](const WriteConcernErrorDetail& wce) {
        if (!result.hasField("writeConcernError")) {
            result.append("writeConcernError", wce.toBSON());
        }
    };

    auto swReply = processFindAndModifyRequest<write_ops::FindAndModifyCommandReply>(
        opCtx, request, &getTransactionWithRetriesForMongoS, processFindAndModify, onErrorWithWCE);

    auto reply = uassertStatusOK(swReply).first;

    reply.serialize(&result);

    return FLEBatchResult::kProcessed;
}

std::pair<write_ops::FindAndModifyCommandRequest, OpMsgRequest>
processFLEFindAndModifyExplainMongos(OperationContext* opCtx,
                                     const write_ops::FindAndModifyCommandRequest& request) {
    tassert(6513400,
            "Missing encryptionInformation for findAndModify",
            request.getEncryptionInformation().has_value());

    return uassertStatusOK(processFindAndModifyRequest<write_ops::FindAndModifyCommandRequest>(
        opCtx, request, &getTransactionWithRetriesForMongoS, processFindAndModifyExplain));
}

BSONObj FLEQueryInterfaceImpl::getById(const NamespaceString& nss, BSONElement element) {
    FindCommandRequest find(nss);
    find.setFilter(BSON("_id" << element));
    find.setSingleBatch(true);

    find.setEncryptionInformation(makeEmptyProcessEncryptionInformation());

    // Throws on error
    auto docs = _txnClient.exhaustiveFindSync(find);

    if (docs.size() == 0) {
        return BSONObj();
    } else {
        // We only expect one document in the state collection considering that _id is a unique
        // index
        uassert(6371201,
                "Unexpected to find more then one FLE state collection document",
                docs.size() == 1);
        return docs[0];
    }
}

QECountInfoQueryTypeEnum queryTypeTranslation(FLEQueryInterface::TagQueryType type) {
    switch (type) {
        case FLEQueryInterface::TagQueryType::kInsert:
            return QECountInfoQueryTypeEnum::Insert;
        case FLEQueryInterface::TagQueryType::kQuery:
            return QECountInfoQueryTypeEnum::Query;
        case FLEQueryInterface::TagQueryType::kCompact:
            return QECountInfoQueryTypeEnum::Compact;
        case FLEQueryInterface::TagQueryType::kCleanup:
            return QECountInfoQueryTypeEnum::Cleanup;
        case FLEQueryInterface::TagQueryType::kPadding:
            return QECountInfoQueryTypeEnum::Padding;
        default:
            uasserted(7517101, "Invalid TagQueryType value.");
    }
}

std::vector<std::vector<FLEEdgeCountInfo>> FLEQueryInterfaceImpl::getTags(
    const NamespaceString& nss,
    const std::vector<std::vector<FLEEdgePrfBlock>>& tokensSets,
    FLEQueryInterface::TagQueryType type) {

    GetQueryableEncryptionCountInfo getCountsCmd(nss);

    getCountsCmd.setTokens(toTagSets(tokensSets));
    getCountsCmd.setQueryType(queryTypeTranslation(type));

    auto response = _txnClient.runCommandSync(nss.dbName(), getCountsCmd.toBSON());

    auto status = getStatusFromWriteCommandReply(response);
    uassertStatusOK(status);

    auto reply = QECountInfosReply::parse(response, IDLParserContext("reply"));

    return toEdgeCounts(reply.getCounts());
}

StatusWith<write_ops::InsertCommandReply> FLEQueryInterfaceImpl::insertDocuments(
    const NamespaceString& nss,
    std::vector<BSONObj> objs,
    StmtId* pStmtId,
    bool translateDuplicateKey,
    bool bypassDocumentValidation) {
    write_ops::InsertCommandRequest insertRequest(nss);
    auto documentCount = objs.size();
    dassert(documentCount > 0);
    insertRequest.setDocuments(std::move(objs));

    insertRequest.getWriteCommandRequestBase().setEncryptionInformation(
        makeEmptyProcessEncryptionInformation());
    insertRequest.getWriteCommandRequestBase().setBypassDocumentValidation(
        bypassDocumentValidation);

    std::vector<StmtId> stmtIds;
    int32_t stmtId = *pStmtId;
    if (stmtId != kUninitializedStmtId) {
        (*pStmtId) += documentCount;

        stmtIds.reserve(documentCount);
        for (size_t i = 0; i < documentCount; i++) {
            stmtIds.push_back(stmtId + i);
        }
    }

    auto response = _txnClient.runCRUDOpSync(BatchedCommandRequest(insertRequest), stmtIds);

    write_ops::InsertCommandReply reply;

    responseToReply(response, reply.getWriteCommandReplyBase());

    return {reply};
}

std::pair<write_ops::DeleteCommandReply, BSONObj> FLEQueryInterfaceImpl::deleteWithPreimage(
    const NamespaceString& nss,
    const EncryptionInformation& ei,
    const write_ops::DeleteCommandRequest& deleteRequest) {
    // We only support a single delete
    dassert(deleteRequest.getStmtIds().value_or(std::vector<int32_t>()).empty());

    auto deleteOpEntry = deleteRequest.getDeletes()[0];

    write_ops::FindAndModifyCommandRequest findAndModifyRequest(nss);
    findAndModifyRequest.setQuery(deleteOpEntry.getQ());
    findAndModifyRequest.setHint(deleteOpEntry.getHint());
    findAndModifyRequest.setBatchSize(1);
    findAndModifyRequest.setSingleBatch(true);
    findAndModifyRequest.setRemove(true);
    findAndModifyRequest.setCollation(deleteOpEntry.getCollation());
    findAndModifyRequest.setLet(deleteRequest.getLet());
    findAndModifyRequest.setStmtId(deleteRequest.getStmtId());

    // Intentionally copying the original encryption information here as we are going to modify it.
    auto ei2 = ei;
    ei2.setCrudProcessed(true);
    findAndModifyRequest.setEncryptionInformation(ei2);

    auto response = _txnClient.runCommandSync(nss.dbName(), findAndModifyRequest.toBSON());

    auto status = getStatusFromWriteCommandReply(response);

    BSONObj returnObj;
    write_ops::DeleteCommandReply deleteReply;

    if (!status.isOK()) {
        deleteReply.getWriteCommandReplyBase().setN(0);
        deleteReply.getWriteCommandReplyBase().setWriteErrors(singleStatusToWriteErrors(status));
    } else {
        auto reply =
            write_ops::FindAndModifyCommandReply::parse(response, IDLParserContext("reply"));
        if (reply.getLastErrorObject().getNumDocs() > 0) {
            deleteReply.getWriteCommandReplyBase().setN(1);
        }

        returnObj = reply.getValue().value_or(BSONObj());
    }

    return {deleteReply, returnObj};
}

write_ops::DeleteCommandReply FLEQueryInterfaceImpl::deleteDocument(
    const NamespaceString& nss, int32_t stmtId, write_ops::DeleteCommandRequest& deleteRequest) {

    dassert(!deleteRequest.getWriteCommandRequestBase().getEncryptionInformation());
    dassert(deleteRequest.getStmtIds().value_or(std::vector<int32_t>()).empty());

    deleteRequest.getWriteCommandRequestBase().setEncryptionInformation(
        makeEmptyProcessEncryptionInformation());

    generic_argument_util::prepareRequestForInternalTransactionPassthrough(deleteRequest);

    auto response = _txnClient.runCRUDOpSync(BatchedCommandRequest(deleteRequest), {stmtId});
    write_ops::DeleteCommandReply reply;
    responseToReply(response, reply.getWriteCommandReplyBase());
    return {reply};
}

std::pair<write_ops::UpdateCommandReply, BSONObj> FLEQueryInterfaceImpl::updateWithPreimage(
    const NamespaceString& nss,
    const EncryptionInformation& ei,
    const write_ops::UpdateCommandRequest& updateRequest) {
    // We only support a single update
    dassert(updateRequest.getStmtIds().value_or(std::vector<int32_t>()).empty());

    auto updateOpEntry = updateRequest.getUpdates()[0];

    write_ops::FindAndModifyCommandRequest findAndModifyRequest(nss);
    findAndModifyRequest.setQuery(updateOpEntry.getQ());
    findAndModifyRequest.setUpdate(updateOpEntry.getU());
    findAndModifyRequest.setBatchSize(1);
    findAndModifyRequest.setUpsert(updateOpEntry.getUpsert());
    findAndModifyRequest.setSingleBatch(true);
    findAndModifyRequest.setRemove(false);
    findAndModifyRequest.setArrayFilters(updateOpEntry.getArrayFilters());
    findAndModifyRequest.setCollation(updateOpEntry.getCollation());
    findAndModifyRequest.setHint(updateOpEntry.getHint());
    findAndModifyRequest.setLet(
        mergeLetAndCVariables(updateRequest.getLet(), updateOpEntry.getC()));
    findAndModifyRequest.setStmtId(updateRequest.getStmtId());
    findAndModifyRequest.setBypassDocumentValidation(updateRequest.getBypassDocumentValidation());

    // Intentionally copying the original encryption information here as we are going to modify it.
    auto ei2 = ei;
    ei2.setCrudProcessed(true);
    findAndModifyRequest.setEncryptionInformation(ei2);

    auto response = _txnClient.runCommandSync(nss.dbName(), findAndModifyRequest.toBSON());

    auto status = getStatusFromWriteCommandReply(response);
    uassertStatusOK(status);

    auto reply = write_ops::FindAndModifyCommandReply::parse(response, IDLParserContext("reply"));

    write_ops::UpdateCommandReply updateReply;

    if (!status.isOK()) {
        updateReply.getWriteCommandReplyBase().setN(0);
        updateReply.getWriteCommandReplyBase().setWriteErrors(singleStatusToWriteErrors(status));
    } else {
        if (reply.getRetriedStmtId().has_value()) {
            updateReply.getWriteCommandReplyBase().setRetriedStmtIds(
                std::vector<std::int32_t>{reply.getRetriedStmtId().value()});
        }

        auto& lastErrorObject = reply.getLastErrorObject();

        updateReply.getWriteCommandReplyBase().setN(lastErrorObject.getNumDocs());

        if (lastErrorObject.getUpserted().has_value()) {
            write_ops::Upserted upserted;
            upserted.setIndex(0);
            upserted.set_id(lastErrorObject.getUpserted().value());
            updateReply.setUpserted(std::vector<mongo::write_ops::Upserted>{upserted});
        } else {
            dassert(lastErrorObject.getUpdatedExisting().has_value());
            if (lastErrorObject.getUpdatedExisting().value()) {
                updateReply.setNModified(1);
            }
        }
    }

    return {updateReply, reply.getValue().value_or(BSONObj())};
}

write_ops::UpdateCommandReply FLEQueryInterfaceImpl::update(
    const NamespaceString& nss, int32_t stmtId, write_ops::UpdateCommandRequest& updateRequest) {

    invariant(!updateRequest.getWriteCommandRequestBase().getEncryptionInformation());

    updateRequest.getWriteCommandRequestBase().setEncryptionInformation(
        makeEmptyProcessEncryptionInformation());

    generic_argument_util::prepareRequestForInternalTransactionPassthrough(updateRequest);

    dassert(updateRequest.getStmtIds().value_or(std::vector<int32_t>()).empty());

    auto response = _txnClient.runCRUDOpSync(BatchedCommandRequest(updateRequest), {stmtId});

    write_ops::UpdateCommandReply reply;

    responseToReply(response, reply.getWriteCommandReplyBase());

    reply.setNModified(response.getNModified());

    return {reply};
}

write_ops::FindAndModifyCommandReply FLEQueryInterfaceImpl::findAndModify(
    const NamespaceString& nss,
    const EncryptionInformation& ei,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) {
    // Intentional copy of the request made here, as the original request is modified.
    auto newFindAndModifyRequest = findAndModifyRequest;
    // Intentionally copying the original encryption information here as we are going to modify it.
    auto ei2 = ei;
    ei2.setCrudProcessed(true);
    newFindAndModifyRequest.setEncryptionInformation(ei2);
    generic_argument_util::prepareRequestForInternalTransactionPassthrough(newFindAndModifyRequest);

    auto response = _txnClient.runCommandSync(nss.dbName(), newFindAndModifyRequest.toBSON());

    auto status = getStatusFromWriteCommandReply(response);
    uassertStatusOK(status);

    return write_ops::FindAndModifyCommandReply::parse(response, IDLParserContext("reply"));
}

std::vector<BSONObj> FLEQueryInterfaceImpl::findDocuments(const NamespaceString& nss,
                                                          BSONObj filter) {
    FindCommandRequest find(nss);
    find.setFilter(filter);

    find.setEncryptionInformation(makeEmptyProcessEncryptionInformation());

    // Throws on error
    return _txnClient.exhaustiveFindSync(find);
}

void processFLEFindS(OperationContext* opCtx,
                     const NamespaceString& nss,
                     FindCommandRequest* findCommand) {
    fle::processFindCommand(opCtx, nss, findCommand, &getTransactionWithRetriesForMongoS);
}

void processFLECountS(OperationContext* opCtx,
                      const NamespaceString& nss,
                      CountCommandRequest& countCommand) {
    fle::processCountCommand(opCtx, nss, &countCommand, &getTransactionWithRetriesForMongoS);
}

std::unique_ptr<Pipeline> processFLEPipelineS(OperationContext* opCtx,
                                              NamespaceString nss,
                                              const EncryptionInformation& encryptInfo,
                                              std::unique_ptr<Pipeline> toRewrite) {
    return fle::processPipeline(
        opCtx, nss, encryptInfo, std::move(toRewrite), &getTransactionWithRetriesForMongoS);
}

FLETagNoTXNQuery::FLETagNoTXNQuery(OperationContext* opCtx) : _opCtx(opCtx) {}

BSONObj FLETagNoTXNQuery::getById(const NamespaceString& nss, BSONElement element) {
    invariant(false);
    return {};
};

std::vector<std::vector<FLEEdgeCountInfo>> FLETagNoTXNQuery::getTags(
    const NamespaceString& nss,
    const std::vector<std::vector<FLEEdgePrfBlock>>& tokensSets,
    FLEQueryInterface::TagQueryType type) {

    invariant(!_opCtx->inMultiDocumentTransaction());

    // Pop off the current op context so we can get a fresh set of read concern settings
    // Allow the thread to be killable. If interrupted, the call to runCommand will fail with the
    // interruption.
    auto client = _opCtx->getService()->makeClient("FLETagNoTXNQuery");

    AlternativeClientRegion clientRegion(client);
    auto opCtx = cc().makeOperationContext();
    auto as = AuthorizationSession::get(cc());
    as->grantInternalAuthorization();

    const auto setDollarTenant = nss.tenantId() && gMultitenancySupport;
    const auto vts = auth::ValidatedTenancyScope::get(_opCtx);

    // We need to instruct the request object (via serialization context passed in when constructing
    // getCountsCmd) that we do not ALSO prefix the $db field when serialize() is later called since
    // we will already be setting the unsigned security token with default tenant protocol below.
    // Providing both a tenant prefix and a default tenant protocol is unsupported and can lead to
    // namespace errors.
    auto sc = SerializationContext::stateCommandRequest(
        setDollarTenant, vts != boost::none && vts->isFromAtlasProxy());

    GetQueryableEncryptionCountInfo getCountsCmd(nss, sc);

    getCountsCmd.setTokens(toTagSets(tokensSets));
    getCountsCmd.setQueryType(queryTypeTranslation(type));

    DBDirectClient directClient(opCtx.get());

    auto request = getCountsCmd.serialize();
    if (vts) {
        request.validatedTenancyScope = *vts;
    }

    auto uniqueReply = directClient.runCommand(request);
    auto response = uniqueReply->getCommandReply();

    auto status = getStatusFromWriteCommandReply(response);
    uassertStatusOK(status);

    auto reply = QECountInfosReply::parse(response, IDLParserContext("reply"));

    return toEdgeCounts(reply.getCounts());
}
}  // namespace mongo
