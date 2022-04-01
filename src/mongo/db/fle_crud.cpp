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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

#include "mongo/db/fle_crud.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/fle/server_rewrite.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"

MONGO_FAIL_POINT_DEFINE(fleCrudHangInsert);
MONGO_FAIL_POINT_DEFINE(fleCrudHangUpdate);
MONGO_FAIL_POINT_DEFINE(fleCrudHangDelete);
MONGO_FAIL_POINT_DEFINE(fleCrudHangFindAndModify);

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

void replyToResponse(write_ops::WriteCommandReplyBase* replyBase,
                     BatchedCommandResponse* response) {
    response->setStatus(Status::OK());
    response->setN(replyBase->getN());
    if (replyBase->getElectionId()) {
        response->setElectionId(replyBase->getElectionId().value());
    }
    if (replyBase->getOpTime()) {
        response->setLastOp(replyBase->getOpTime().value());
    }
    if (replyBase->getWriteErrors()) {
        for (const auto& error : *replyBase->getWriteErrors()) {
            response->addToErrDetails(error);
        }
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

}  // namespace

StatusWith<txn_api::CommitResult> runInTxnWithRetry(
    OperationContext* opCtx,
    std::shared_ptr<txn_api::TransactionWithRetries> trun,
    std::function<SemiFuture<void>(const txn_api::TransactionClient& txnClient,
                                   ExecutorPtr txnExec)> callback) {

    bool inClientTransaction = opCtx->inMultiDocumentTransaction();

    // TODO SERVER-59566 - how much do we retry before we give up?
    while (true) {

        // Result will get the status of the TXN
        // Non-client initiated txns get retried automatically.
        // Client txns are the user responsibility to retry and so if we hit a contention
        // placeholder, we need to abort and defer to the client
        auto swResult = trun->runSyncNoThrow(opCtx, callback);
        if (swResult.isOK()) {
            return swResult;
        }

        // We cannot retry the transaction if initiated by a user
        if (inClientTransaction) {
            return swResult;
        }

        // - DuplicateKeyException - suggestions contention on ESC
        // - FLEContention
        if (swResult.getStatus().code() != ErrorCodes::FLECompactionPlaceholder &&
            swResult.getStatus().code() != ErrorCodes::FLEStateCollectionContention) {
            return swResult;
        }

        if (!swResult.isOK()) {
            return swResult;
        }

        auto commitResult = swResult.getValue();
        if (commitResult.getEffectiveStatus().isOK()) {
            return commitResult;
        }
    }
}

std::shared_ptr<txn_api::TransactionWithRetries> getTransactionWithRetriesForMongoS(
    OperationContext* opCtx) {
    return std::make_shared<txn_api::TransactionWithRetries>(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        TransactionRouterResourceYielder::makeForLocalHandoff());
}

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
                                      ->makeFromBSON(op.getCollation().get());

        uassertStatusOK(statusWithCollator.getStatus());
        collator = std::move(statusWithCollator.getValue());
    }
    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    std::move(collator),
                                                    request.getNamespace(),
                                                    request.getLegacyRuntimeConstants(),
                                                    request.getLet());
    expCtx->stopExpressionCounters();
    return expCtx;
}

std::pair<FLEBatchResult, write_ops::InsertCommandReply> processInsert(
    OperationContext* opCtx,
    const write_ops::InsertCommandRequest& insertRequest,
    GetTxnCallback getTxns) {

    auto documents = insertRequest.getDocuments();
    // TODO - how to check if a document will be too large???
    uassert(6371202, "Only single insert batches are supported in FLE2", documents.size() == 1);

    auto document = documents[0];
    auto serverPayload = std::make_shared<std::vector<EDCServerPayloadInfo>>(
        EDCServerCollection::getEncryptedFieldInfo(document));

    if (serverPayload->size() == 0) {
        // No actual FLE2 indexed fields
        return std::pair<FLEBatchResult, write_ops::InsertCommandReply>{
            FLEBatchResult::kNotProcessed, write_ops::InsertCommandReply()};
    }

    auto ei = insertRequest.getEncryptionInformation().get();

    auto edcNss = insertRequest.getNamespace();
    auto efc = EncryptionInformationHelpers::getAndValidateSchema(insertRequest.getNamespace(), ei);
    write_ops::InsertCommandReply reply;

    std::shared_ptr<txn_api::TransactionWithRetries> trun = getTxns(opCtx);

    // The function that handles the transaction may outlive this function so we need to use
    // shared_ptrs since it runs on another thread
    auto ownedDocument = document.getOwned();
    auto insertBlock = std::tie(edcNss, efc, serverPayload, reply);
    auto sharedInsertBlock = std::make_shared<decltype(insertBlock)>(insertBlock);

    auto swResult = runInTxnWithRetry(
        opCtx,
        trun,
        [sharedInsertBlock, ownedDocument](const txn_api::TransactionClient& txnClient,
                                           ExecutorPtr txnExec) {
            FLEQueryInterfaceImpl queryImpl(txnClient);

            auto [edcNss2, efc2, serverPayload2, reply2] = *sharedInsertBlock.get();

            reply2 = uassertStatusOK(
                processInsert(&queryImpl, edcNss2, *serverPayload2.get(), efc2, ownedDocument));

            if (MONGO_unlikely(fleCrudHangInsert.shouldFail())) {
                LOGV2(6371903, "Hanging due to fleCrudHangInsert fail point");
                fleCrudHangInsert.pauseWhileSet();
            }

            // If we have write errors but no unexpected internal errors, then we reach here
            // If we have write errors, we need to return a failed status to ensure the txn client
            // does not try to commit the transaction.
            if (reply2.getWriteErrors().has_value() && !reply2.getWriteErrors().value().empty()) {
                return SemiFuture<void>::makeReady(
                    Status(ErrorCodes::FLETransactionAbort, "FLE2 write errors on insert"));
            }

            return SemiFuture<void>::makeReady();
        });

    if (!swResult.isOK()) {
        // FLETransactionAbort is used for control flow so it means we have a valid
        // InsertCommandReply with write errors so we should return that.
        if (swResult.getStatus() == ErrorCodes::FLETransactionAbort) {
            return std::pair<FLEBatchResult, write_ops::InsertCommandReply>{
                FLEBatchResult::kProcessed, reply};
        }

        appendSingleStatusToWriteErrors(swResult.getStatus(), &reply.getWriteCommandReplyBase());
    }

    return std::pair<FLEBatchResult, write_ops::InsertCommandReply>{FLEBatchResult::kProcessed,
                                                                    reply};
}

write_ops::DeleteCommandReply processDelete(OperationContext* opCtx,
                                            const write_ops::DeleteCommandRequest& deleteRequest,
                                            GetTxnCallback getTxns) {

    auto deletes = deleteRequest.getDeletes();
    uassert(6371302, "Only single document deletes are permitted", deletes.size() == 1);

    auto deleteOpEntry = deletes[0];

    uassert(
        6371303, "FLE only supports single document deletes", deleteOpEntry.getMulti() == false);

    std::shared_ptr<txn_api::TransactionWithRetries> trun = getTxns(opCtx);

    write_ops::DeleteCommandReply reply;

    auto expCtx = makeExpCtx(opCtx, deleteRequest, deleteOpEntry);
    // The function that handles the transaction may outlive this function so we need to use
    // shared_ptrs
    auto deleteBlock = std::tie(deleteRequest, reply, expCtx);
    auto sharedDeleteBlock = std::make_shared<decltype(deleteBlock)>(deleteBlock);

    auto swResult = runInTxnWithRetry(
        opCtx,
        trun,
        [sharedDeleteBlock](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            FLEQueryInterfaceImpl queryImpl(txnClient);

            auto [deleteRequest2, reply2, expCtx2] = *sharedDeleteBlock.get();

            reply2 = processDelete(&queryImpl, expCtx2, deleteRequest2);

            if (MONGO_unlikely(fleCrudHangDelete.shouldFail())) {
                LOGV2(6371902, "Hanging due to fleCrudHangDelete fail point");
                fleCrudHangDelete.pauseWhileSet();
            }

            // If we have write errors but no unexpected internal errors, then we reach here
            // If we have write errors, we need to return a failed status to ensure the txn client
            // does not try to commit the transaction.
            if (reply2.getWriteErrors().has_value() && !reply2.getWriteErrors().value().empty()) {
                return SemiFuture<void>::makeReady(
                    Status(ErrorCodes::FLETransactionAbort, "FLE2 write errors on delete"));
            }

            return SemiFuture<void>::makeReady();
        });

    if (!swResult.isOK()) {
        // FLETransactionAbort is used for control flow so it means we have a valid
        // InsertCommandReply with write errors so we should return that.
        if (swResult.getStatus() == ErrorCodes::FLETransactionAbort) {
            return reply;
        }

        appendSingleStatusToWriteErrors(swResult.getStatus(), &reply.getWriteCommandReplyBase());
    }

    return reply;
}

write_ops::UpdateCommandReply processUpdate(OperationContext* opCtx,
                                            const write_ops::UpdateCommandRequest& updateRequest,
                                            GetTxnCallback getTxns) {

    auto updates = updateRequest.getUpdates();
    uassert(6371502, "Only single document updates are permitted", updates.size() == 1);

    auto updateOpEntry = updates[0];

    uassert(
        6371503, "FLE only supports single document updates", updateOpEntry.getMulti() == false);

    // pipeline - is agg specific, delta is oplog, transform is internal (timeseries)
    uassert(6371517,
            "FLE only supports modifier and replacement style updates",
            updateOpEntry.getU().type() == write_ops::UpdateModification::Type::kModifier ||
                updateOpEntry.getU().type() == write_ops::UpdateModification::Type::kReplacement);

    std::shared_ptr<txn_api::TransactionWithRetries> trun = getTxns(opCtx);

    // The function that handles the transaction may outlive this function so we need to use
    // shared_ptrs
    write_ops::UpdateCommandReply reply;
    auto expCtx = makeExpCtx(opCtx, updateRequest, updateOpEntry);
    auto updateBlock = std::tie(updateRequest, reply, expCtx);
    auto sharedupdateBlock = std::make_shared<decltype(updateBlock)>(updateBlock);

    auto swResult = runInTxnWithRetry(
        opCtx,
        trun,
        [sharedupdateBlock](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            FLEQueryInterfaceImpl queryImpl(txnClient);

            auto [updateRequest2, reply2, expCtx2] = *sharedupdateBlock.get();

            reply2 = processUpdate(&queryImpl, expCtx2, updateRequest2);

            if (MONGO_unlikely(fleCrudHangUpdate.shouldFail())) {
                LOGV2(6371901, "Hanging due to fleCrudHangUpdate fail point");
                fleCrudHangUpdate.pauseWhileSet();
            }

            // If we have write errors but no unexpected internal errors, then we reach here
            // If we have write errors, we need to return a failed status to ensure the txn client
            // does not try to commit the transaction.
            if (reply2.getWriteErrors().has_value() && !reply2.getWriteErrors().value().empty()) {
                return SemiFuture<void>::makeReady(
                    Status(ErrorCodes::FLETransactionAbort, "FLE2 write errors on delete"));
            }

            return SemiFuture<void>::makeReady();
        });

    if (!swResult.isOK()) {
        // FLETransactionAbort is used for control flow so it means we have a valid
        // InsertCommandReply with write errors so we should return that.
        if (swResult.getStatus() == ErrorCodes::FLETransactionAbort) {
            return reply;
        }

        appendSingleStatusToWriteErrors(swResult.getStatus(), &reply.getWriteCommandReplyBase());
    }

    return reply;
}

namespace {

void processFieldsForInsert(FLEQueryInterface* queryImpl,
                            const NamespaceString& edcNss,
                            std::vector<EDCServerPayloadInfo>& serverPayload,
                            const EncryptedFieldConfig& efc) {

    NamespaceString nssEsc(edcNss.db(), efc.getEscCollection().get());

    auto docCount = queryImpl->countDocuments(nssEsc);

    TxnCollectionReader reader(docCount, queryImpl, nssEsc);

    for (auto& payload : serverPayload) {

        auto escToken = payload.getESCToken();
        auto tagToken = FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escToken);
        auto valueToken =
            FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(escToken);

        int position = 1;
        int count = 1;
        auto alpha = ESCCollection::emuBinary(reader, tagToken, valueToken);

        if (alpha.has_value() && alpha.value() == 0) {
            position = 1;
            count = 1;
        } else if (!alpha.has_value()) {
            auto block = ESCCollection::generateId(tagToken, boost::none);

            auto r_esc = reader.getById(block);
            uassert(6371203, "ESC document not found", !r_esc.isEmpty());

            auto escNullDoc =
                uassertStatusOK(ESCCollection::decryptNullDocument(valueToken, r_esc));

            position = escNullDoc.position + 2;
            count = escNullDoc.count + 1;
        } else {
            auto block = ESCCollection::generateId(tagToken, alpha);

            auto r_esc = reader.getById(block);
            uassert(6371204, "ESC document not found", !r_esc.isEmpty());

            auto escDoc = uassertStatusOK(ESCCollection::decryptDocument(valueToken, r_esc));

            position = alpha.value() + 1;
            count = escDoc.count + 1;

            if (escDoc.compactionPlaceholder) {
                uassertStatusOK(Status(ErrorCodes::FLECompactionPlaceholder,
                                       "Found ESC contention placeholder"));
            }
        }

        payload.count = count;

        auto escInsertReply = uassertStatusOK(queryImpl->insertDocument(
            nssEsc,
            ESCCollection::generateInsertDocument(tagToken, valueToken, position, count),
            true));
        checkWriteErrors(escInsertReply);


        NamespaceString nssEcoc(edcNss.db(), efc.getEcocCollection().get());

        // TODO - should we make this a batch of ECOC updates?
        auto ecocInsertReply = uassertStatusOK(queryImpl->insertDocument(
            nssEcoc,
            ECOCCollection::generateDocument(payload.fieldPathName,
                                             payload.payload.getEncryptedTokens()),
            false));
        checkWriteErrors(ecocInsertReply);
    }
}

void processRemovedFields(FLEQueryInterface* queryImpl,
                          const NamespaceString& edcNss,
                          const EncryptedFieldConfig& efc,
                          const StringMap<FLEDeleteToken>& tokenMap,
                          const std::vector<EDCIndexedFields>& deletedFields) {

    NamespaceString nssEcc(edcNss.db(), efc.getEccCollection().get());


    auto docCount = queryImpl->countDocuments(nssEcc);

    TxnCollectionReader reader(docCount, queryImpl, nssEcc);


    for (const auto& deletedField : deletedFields) {
        // TODO - verify each indexed fields is listed in EncryptionInformation for the
        // schema

        auto it = tokenMap.find(deletedField.fieldPathName);
        uassert(6371304,
                str::stream() << "Could not find delete token for field: "
                              << deletedField.fieldPathName,
                it != tokenMap.end());

        auto deleteToken = it->second;

        auto [encryptedTypeBinding, subCdr] = fromEncryptedConstDataRange(deletedField.value);

        // TODO - add support other types
        uassert(6371305,
                "Ony support deleting equality indexed fields",
                encryptedTypeBinding == EncryptedBinDataType::kFLE2EqualityIndexedValue);

        auto plainTextField = uassertStatusOK(FLE2IndexedEqualityEncryptedValue::decryptAndParse(
            deleteToken.serverEncryptionToken, subCdr));

        auto tagToken =
            FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(plainTextField.ecc);
        auto valueToken =
            FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(plainTextField.ecc);

        auto alpha = ECCCollection::emuBinary(reader, tagToken, valueToken);

        uint64_t index = 0;
        if (alpha.has_value() && alpha.value() == 0) {
            index = 1;
        } else if (!alpha.has_value()) {
            auto block = ECCCollection::generateId(tagToken, boost::none);

            auto r_ecc = reader.getById(block);
            uassert(6371306, "ECC null document not found", !r_ecc.isEmpty());

            auto eccNullDoc =
                uassertStatusOK(ECCCollection::decryptNullDocument(valueToken, r_ecc));
            index = eccNullDoc.position + 2;
        } else {
            auto block = ECCCollection::generateId(tagToken, alpha);

            auto r_ecc = reader.getById(block);
            uassert(6371307, "ECC document not found", !r_ecc.isEmpty());

            auto eccDoc = uassertStatusOK(ECCCollection::decryptDocument(valueToken, r_ecc));

            if (eccDoc.valueType == ECCValueType::kCompactionPlaceholder) {
                uassertStatusOK(
                    Status(ErrorCodes::FLECompactionPlaceholder, "Found contention placeholder"));
            }

            index = alpha.value() + 1;
        }

        auto eccInsertReply = uassertStatusOK(queryImpl->insertDocument(
            nssEcc,
            ECCCollection::generateDocument(tagToken, valueToken, index, plainTextField.count),
            true));
        checkWriteErrors(eccInsertReply);

        NamespaceString nssEcoc(edcNss.db(), efc.getEcocCollection().get());

        // TODO - make this a batch of ECOC updates?
        EncryptedStateCollectionTokens tokens(plainTextField.esc, plainTextField.ecc);
        auto encryptedTokens = uassertStatusOK(tokens.serialize(deleteToken.ecocToken));
        auto ecocInsertReply = uassertStatusOK(queryImpl->insertDocument(
            nssEcoc,
            ECOCCollection::generateDocument(deletedField.fieldPathName, encryptedTokens),
            false));
        checkWriteErrors(ecocInsertReply);
    }
}

}  // namespace

StatusWith<write_ops::FindAndModifyCommandReply> processFindAndModifyRequest(
    OperationContext* opCtx,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest,
    GetTxnCallback getTxns) {

    // Is this a delete
    bool isDelete = findAndModifyRequest.getRemove().value_or(false);

    // User can only specify either remove = true or update != {}
    uassert(6371401,
            "Must specify either update or remove to findAndModify, not both",
            !(findAndModifyRequest.getUpdate().has_value() && isDelete));

    uassert(6371402,
            "findAndModify with encryption only supports new: false",
            findAndModifyRequest.getNew().value_or(false) == false);

    uassert(6371408,
            "findAndModify fields must be empty",
            findAndModifyRequest.getFields().value_or(BSONObj()).isEmpty());

    // pipeline - is agg specific, delta is oplog, transform is internal (timeseries)
    auto updateModicationType =
        findAndModifyRequest.getUpdate().value_or(write_ops::UpdateModification()).type();
    uassert(6439901,
            "FLE only supports modifier and replacement style updates",
            updateModicationType == write_ops::UpdateModification::Type::kModifier ||
                updateModicationType == write_ops::UpdateModification::Type::kReplacement);

    std::shared_ptr<txn_api::TransactionWithRetries> trun = getTxns(opCtx);

    // The function that handles the transaction may outlive this function so we need to use
    // shared_ptrs
    write_ops::FindAndModifyCommandReply reply;
    auto findAndModifyBlock = std::tie(findAndModifyRequest, reply);
    auto sharedFindAndModifyBlock =
        std::make_shared<decltype(findAndModifyBlock)>(findAndModifyBlock);

    auto swResult = runInTxnWithRetry(
        opCtx,
        trun,
        [sharedFindAndModifyBlock](const txn_api::TransactionClient& txnClient,
                                   ExecutorPtr txnExec) {
            FLEQueryInterfaceImpl queryImpl(txnClient);

            auto [findAndModifyRequest2, reply2] = *sharedFindAndModifyBlock.get();

            reply2 = processFindAndModify(&queryImpl, findAndModifyRequest2);


            if (MONGO_unlikely(fleCrudHangFindAndModify.shouldFail())) {
                LOGV2(6371900, "Hanging due to fleCrudHangFindAndModify fail point");
                fleCrudHangFindAndModify.pauseWhileSet();
            }

            return SemiFuture<void>::makeReady();
        });

    if (!swResult.isOK()) {
        return swResult.getStatus();
    }

    return reply;
}

FLEQueryInterface::~FLEQueryInterface() {}

StatusWith<write_ops::InsertCommandReply> processInsert(
    FLEQueryInterface* queryImpl,
    const NamespaceString& edcNss,
    std::vector<EDCServerPayloadInfo>& serverPayload,
    const EncryptedFieldConfig& efc,
    BSONObj document) {

    processFieldsForInsert(queryImpl, edcNss, serverPayload, efc);

    auto finalDoc = EDCServerCollection::finalizeForInsert(document, serverPayload);

    return queryImpl->insertDocument(edcNss, finalDoc, false);
}

write_ops::DeleteCommandReply processDelete(FLEQueryInterface* queryImpl,
                                            boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const write_ops::DeleteCommandRequest& deleteRequest) {

    auto edcNss = deleteRequest.getNamespace();
    auto ei = deleteRequest.getEncryptionInformation().get();

    auto efc = EncryptionInformationHelpers::getAndValidateSchema(edcNss, ei);
    auto tokenMap = EncryptionInformationHelpers::getDeleteTokens(edcNss, ei);

    write_ops::DeleteCommandRequest newDeleteRequest = deleteRequest;

    auto newDeleteOp = newDeleteRequest.getDeletes()[0];
    newDeleteOp.setQ(fle::rewriteEncryptedFilterInsideTxn(
        queryImpl, deleteRequest.getDbName(), efc, expCtx, newDeleteOp.getQ()));
    newDeleteRequest.setDeletes({newDeleteOp});

    // TODO SERVER-64143 - use this delete for retryable writes
    auto [deleteReply, deletedDocument] =
        queryImpl->deleteWithPreimage(edcNss, ei, newDeleteRequest);

    // If the delete did not actually delete anything, we are done
    if (deletedDocument.isEmpty()) {
        write_ops::DeleteCommandReply reply;
        reply.getWriteCommandReplyBase().setN(0);
        return reply;
    }


    auto deletedFields = EDCServerCollection::getEncryptedIndexedFields(deletedDocument);

    processRemovedFields(queryImpl, edcNss, efc, tokenMap, deletedFields);

    return deleteReply;
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
 * 5. Find the removed fields and update ECC
 * 6. Remove the stale tags from the original document with a new push
 */
write_ops::UpdateCommandReply processUpdate(FLEQueryInterface* queryImpl,
                                            boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const write_ops::UpdateCommandRequest& updateRequest) {

    auto edcNss = updateRequest.getNamespace();
    auto ei = updateRequest.getEncryptionInformation().get();

    auto efc = EncryptionInformationHelpers::getAndValidateSchema(edcNss, ei);
    auto tokenMap = EncryptionInformationHelpers::getDeleteTokens(edcNss, ei);
    const auto updateOpEntry = updateRequest.getUpdates()[0];

    const auto updateModification = updateOpEntry.getU();


    // Step 1 ----
    std::vector<EDCServerPayloadInfo> serverPayload;
    auto newUpdateOpEntry = updateRequest.getUpdates()[0];
    newUpdateOpEntry.setQ(fle::rewriteEncryptedFilterInsideTxn(
        queryImpl, updateRequest.getDbName(), efc, expCtx, newUpdateOpEntry.getQ()));

    if (updateModification.type() == write_ops::UpdateModification::Type::kModifier) {
        auto updateModifier = updateModification.getUpdateModifier();
        serverPayload = EDCServerCollection::getEncryptedFieldInfo(updateModifier);

        processFieldsForInsert(queryImpl, edcNss, serverPayload, efc);

        // Step 2 ----
        auto pushUpdate = EDCServerCollection::finalizeForUpdate(updateModifier, serverPayload);

        newUpdateOpEntry.setU(mongo::write_ops::UpdateModification(
            pushUpdate, write_ops::UpdateModification::ClassicTag(), false));
    } else {
        auto replacementDocument = updateModification.getUpdateReplacement();
        serverPayload = EDCServerCollection::getEncryptedFieldInfo(replacementDocument);

        processFieldsForInsert(queryImpl, edcNss, serverPayload, efc);

        // Step 2 ----
        auto safeContentReplace =
            EDCServerCollection::finalizeForInsert(replacementDocument, serverPayload);

        newUpdateOpEntry.setU(mongo::write_ops::UpdateModification(
            safeContentReplace, write_ops::UpdateModification::ClassicTag(), true));
    }

    // Step 3 ----
    auto newUpdateRequest = updateRequest;
    newUpdateRequest.setUpdates({newUpdateOpEntry});

    // TODO - use this update for retryable writes
    auto [updateReply, originalDocument] =
        queryImpl->updateWithPreimage(edcNss, ei, newUpdateRequest);
    if (originalDocument.isEmpty()) {
        // if there is no preimage, then we did not update any documents, we are done
        return updateReply;
    }

    // If there are errors, we are done
    if (updateReply.getWriteErrors().has_value() && !updateReply.getWriteErrors().value().empty()) {
        return updateReply;
    }

    // Step 4 ----
    auto idElement = originalDocument.firstElement();
    uassert(6371504,
            "Missing _id field in pre-image document",
            idElement.fieldNameStringData() == "_id"_sd);
    BSONObj newDocument = queryImpl->getById(edcNss, idElement);

    // Fail if we could not find the new document
    uassert(6371505, "Could not find pre-image document by _id", !newDocument.isEmpty());

    // Check the user did not remove/destroy the __safeContent__ array
    FLEClientCrypto::validateTagsArray(newDocument);

    // Step 5 ----
    auto originalFields = EDCServerCollection::getEncryptedIndexedFields(originalDocument);
    auto newFields = EDCServerCollection::getEncryptedIndexedFields(newDocument);
    auto deletedFields = EDCServerCollection::getRemovedTags(originalFields, newFields);

    processRemovedFields(queryImpl, edcNss, efc, tokenMap, deletedFields);

    // Step 6 ----
    BSONObj pullUpdate = EDCServerCollection::generateUpdateToRemoveTags(deletedFields, tokenMap);
    auto pullUpdateOpEntry = write_ops::UpdateOpEntry();
    pullUpdateOpEntry.setUpsert(false);
    pullUpdateOpEntry.setMulti(false);
    pullUpdateOpEntry.setQ(BSON("_id"_sd << idElement));
    pullUpdateOpEntry.setU(mongo::write_ops::UpdateModification(
        pullUpdate, write_ops::UpdateModification::ClassicTag(), false));
    newUpdateRequest.setUpdates({pullUpdateOpEntry});
    /* ignore */ queryImpl->updateWithPreimage(edcNss, ei, newUpdateRequest);

    return updateReply;
}

FLEBatchResult processFLEBatch(OperationContext* opCtx,
                               const BatchedCommandRequest& request,
                               BatchWriteExecStats* stats,
                               BatchedCommandResponse* response,
                               boost::optional<OID> targetEpoch) {

    if (!gFeatureFlagFLE2.isEnabledAndIgnoreFCV()) {
        uasserted(6371209, "Feature flag FLE2 is not enabled");
    }

    if (request.getBatchType() == BatchedCommandRequest::BatchType_Insert) {
        auto insertRequest = request.getInsertRequest();

        auto [batchResult, insertReply] =
            processInsert(opCtx, insertRequest, &getTransactionWithRetriesForMongoS);
        if (batchResult == FLEBatchResult::kNotProcessed) {
            return FLEBatchResult::kNotProcessed;
        }

        replyToResponse(&insertReply.getWriteCommandReplyBase(), response);

        return FLEBatchResult::kProcessed;
    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Delete) {

        auto deleteRequest = request.getDeleteRequest();

        auto deleteReply = processDelete(opCtx, deleteRequest, &getTransactionWithRetriesForMongoS);

        replyToResponse(&deleteReply.getWriteCommandReplyBase(), response);
        return FLEBatchResult::kProcessed;

    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Update) {

        auto updateRequest = request.getUpdateRequest();

        auto updateReply = processUpdate(opCtx, updateRequest, &getTransactionWithRetriesForMongoS);

        replyToResponse(&updateReply.getWriteCommandReplyBase(), response);

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

// See processUpdate for algorithm overview
write_ops::FindAndModifyCommandReply processFindAndModify(
    FLEQueryInterface* queryImpl,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) {

    auto edcNss = findAndModifyRequest.getNamespace();
    auto ei = findAndModifyRequest.getEncryptionInformation().get();

    auto efc = EncryptionInformationHelpers::getAndValidateSchema(edcNss, ei);
    auto tokenMap = EncryptionInformationHelpers::getDeleteTokens(edcNss, ei);

    // Step 1 ----
    // If we have an update object, we have to process for ESC
    auto newFindAndModifyRequest = findAndModifyRequest;
    if (findAndModifyRequest.getUpdate().has_value()) {

        std::vector<EDCServerPayloadInfo> serverPayload;
        const auto updateModification = findAndModifyRequest.getUpdate().value();
        write_ops::UpdateModification newUpdateModification;

        if (updateModification.type() == write_ops::UpdateModification::Type::kModifier) {
            auto updateModifier = updateModification.getUpdateModifier();
            serverPayload = EDCServerCollection::getEncryptedFieldInfo(updateModifier);
            processFieldsForInsert(queryImpl, edcNss, serverPayload, efc);

            auto pushUpdate = EDCServerCollection::finalizeForUpdate(updateModifier, serverPayload);

            // Step 2 ----
            newUpdateModification = write_ops::UpdateModification(
                pushUpdate, write_ops::UpdateModification::ClassicTag(), false);
        } else {
            auto replacementDocument = updateModification.getUpdateReplacement();
            serverPayload = EDCServerCollection::getEncryptedFieldInfo(replacementDocument);

            processFieldsForInsert(queryImpl, edcNss, serverPayload, efc);

            // Step 2 ----
            auto safeContentReplace =
                EDCServerCollection::finalizeForInsert(replacementDocument, serverPayload);

            newUpdateModification = write_ops::UpdateModification(
                safeContentReplace, write_ops::UpdateModification::ClassicTag(), true);
        }

        // Step 3 ----
        newFindAndModifyRequest.setUpdate(newUpdateModification);
    }

    // TODO - use this update for retryable writes
    newFindAndModifyRequest.setNew(false);

    auto reply = queryImpl->findAndModify(edcNss, ei, newFindAndModifyRequest);
    if (!reply.getValue().has_value() || reply.getValue().value().isEmpty()) {
        // if there is no preimage, then we did not update or delete any documents, we are done
        return reply;
    }

    // Step 4 ----
    BSONObj originalDocument = reply.getValue().value();
    auto idElement = originalDocument.firstElement();
    uassert(6371403,
            "Missing _id field in pre-image document, the fields document must contain _id",
            idElement.fieldNameStringData() == "_id"_sd);

    BSONObj newDocument;
    std::vector<EDCIndexedFields> newFields;

    // Is this a delete
    bool isDelete = findAndModifyRequest.getRemove().value_or(false);

    // Unlike update, there will not always be a new document since users can delete the document
    if (!isDelete) {
        newDocument = queryImpl->getById(edcNss, idElement);

        // Fail if we could not find the new document
        uassert(6371404, "Could not find pre-image document by _id", !newDocument.isEmpty());

        // Check the user did not remove/destroy the __safeContent__ array
        FLEClientCrypto::validateTagsArray(newDocument);

        newFields = EDCServerCollection::getEncryptedIndexedFields(newDocument);
    }

    // Step 5 ----
    auto originalFields = EDCServerCollection::getEncryptedIndexedFields(originalDocument);
    auto deletedFields = EDCServerCollection::getRemovedTags(originalFields, newFields);

    processRemovedFields(queryImpl, edcNss, efc, tokenMap, deletedFields);

    // Step 6 ----
    // We don't need to make a second update in the case of a delete
    if (!isDelete) {
        BSONObj pullUpdate =
            EDCServerCollection::generateUpdateToRemoveTags(deletedFields, tokenMap);
        auto newUpdateRequest =
            write_ops::UpdateCommandRequest(findAndModifyRequest.getNamespace());
        auto pullUpdateOpEntry = write_ops::UpdateOpEntry();
        pullUpdateOpEntry.setUpsert(false);
        pullUpdateOpEntry.setMulti(false);
        pullUpdateOpEntry.setQ(BSON("_id"_sd << idElement));
        pullUpdateOpEntry.setU(mongo::write_ops::UpdateModification(
            pullUpdate, write_ops::UpdateModification::ClassicTag(), false));
        newUpdateRequest.setUpdates({pullUpdateOpEntry});
        auto [finalUpdateReply, finalCorrectDocument] =
            queryImpl->updateWithPreimage(edcNss, ei, newUpdateRequest);
        checkWriteErrors(finalUpdateReply);
    }

    return reply;
}

FLEBatchResult processFLEFindAndModify(OperationContext* opCtx,
                                       const std::string& dbName,
                                       const BSONObj& cmdObj,
                                       BSONObjBuilder& result) {
    // There is no findAndModify parsing in mongos so we need to first parse to decide if it is for
    // FLE2
    auto request = write_ops::FindAndModifyCommandRequest::parse(
        IDLParserErrorContext("findAndModify"), cmdObj);

    if (!request.getEncryptionInformation().has_value()) {
        return FLEBatchResult::kNotProcessed;
    }

    if (!gFeatureFlagFLE2.isEnabledAndIgnoreFCV()) {
        uasserted(6371405, "Feature flag FLE2 is not enabled");
    }

    // FLE2 Mongos CRUD operations loopback through MongoS with EncryptionInformation as
    // findAndModify so query can do any necessary transformations. But on the nested call, CRUD
    // does not need to do any more work.
    if (request.getEncryptionInformation()->getCrudProcessed()) {
        return FLEBatchResult::kNotProcessed;
    }

    auto swReply = processFindAndModifyRequest(opCtx, request, &getTransactionWithRetriesForMongoS);

    auto reply = uassertStatusOK(swReply);

    reply.serialize(&result);

    return FLEBatchResult::kProcessed;
}

BSONObj FLEQueryInterfaceImpl::getById(const NamespaceString& nss, BSONElement element) {
    FindCommandRequest find(nss);
    find.setFilter(BSON("_id" << element));
    find.setSingleBatch(true);

    // Throws on error
    auto docs = _txnClient.exhaustiveFind(find).get();

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

uint64_t FLEQueryInterfaceImpl::countDocuments(const NamespaceString& nss) {
    // TODO - what about
    // count cmd
    // $collStats
    // approxCount

    // Build the following pipeline:
    //
    //{ aggregate : "testColl", pipeline: [{$match:{}}, {$group : {_id: null, n : {$sum:1}
    //}} ], cursor: {}}

    BSONObjBuilder builder;
    // $db - TXN API DOES THIS FOR US by building OP_MSG
    builder.append("aggregate", nss.coll());

    AggregateCommandRequest request(nss);

    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << BSONObj()));

    {
        BSONObjBuilder sub;
        {
            BSONObjBuilder sub2(sub.subobjStart("$group"));
            sub2.appendNull("_id");
            {
                BSONObjBuilder sub3(sub.subobjStart("n"));
                sub3.append("$sum", 1);
            }
        }

        pipeline.push_back(sub.obj());
    }

    request.setPipeline(pipeline);

    auto commandResponse = _txnClient.runCommand(nss.db(), request.toBSON({})).get();

    uint64_t docCount = 0;
    auto cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(commandResponse));

    auto firstBatch = cursorResponse.getBatch();
    if (!firstBatch.empty()) {
        auto countObj = firstBatch.front();
        docCount = countObj.getIntField("n"_sd);
    }

    return docCount;
}

StatusWith<write_ops::InsertCommandReply> FLEQueryInterfaceImpl::insertDocument(
    const NamespaceString& nss, BSONObj obj, bool translateDuplicateKey) {
    write_ops::InsertCommandRequest insertRequest(nss);
    insertRequest.setDocuments({obj});
    // TODO SERVER-64143 - insertRequest.setWriteConcern

    // TODO SERVER-64143 - propagate the retryable statement ids to runCRUDOp
    auto response = _txnClient.runCRUDOp(BatchedCommandRequest(insertRequest), {}).get();

    auto status = response.toStatus();
    if (translateDuplicateKey && status.code() == ErrorCodes::DuplicateKey) {
        return Status(ErrorCodes::FLEStateCollectionContention, status.reason());
    }

    write_ops::InsertCommandReply reply;

    if (response.isLastOpSet()) {
        reply.getWriteCommandReplyBase().setOpTime(response.getLastOp());
    }

    if (response.isElectionIdSet()) {
        reply.getWriteCommandReplyBase().setElectionId(response.getElectionId());
    }

    reply.getWriteCommandReplyBase().setN(response.getN());
    if (response.isErrDetailsSet()) {
        reply.getWriteCommandReplyBase().setWriteErrors(response.getErrDetails());
    }

    reply.getWriteCommandReplyBase().setRetriedStmtIds(reply.getRetriedStmtIds());

    return {reply};
}

std::pair<write_ops::DeleteCommandReply, BSONObj> FLEQueryInterfaceImpl::deleteWithPreimage(

    const NamespaceString& nss,
    const EncryptionInformation& ei,
    const write_ops::DeleteCommandRequest& deleteRequest) {
    auto deleteOpEntry = deleteRequest.getDeletes()[0];

    write_ops::FindAndModifyCommandRequest findAndModifyRequest(nss);
    findAndModifyRequest.setQuery(deleteOpEntry.getQ());
    findAndModifyRequest.setHint(deleteOpEntry.getHint());
    findAndModifyRequest.setBatchSize(1);
    findAndModifyRequest.setSingleBatch(true);
    findAndModifyRequest.setRemove(true);
    findAndModifyRequest.setCollation(deleteOpEntry.getCollation());
    findAndModifyRequest.setLet(deleteRequest.getLet());
    // TODO SERVER-64143 - writeConcern
    auto ei2 = ei;
    ei2.setCrudProcessed(true);
    findAndModifyRequest.setEncryptionInformation(ei2);

    auto response = _txnClient.runCommand(nss.db(), findAndModifyRequest.toBSON({})).get();
    auto status = getStatusFromWriteCommandReply(response);

    auto reply =
        write_ops::FindAndModifyCommandReply::parse(IDLParserErrorContext("reply"), response);

    write_ops::DeleteCommandReply deleteReply;

    if (!status.isOK()) {
        deleteReply.getWriteCommandReplyBase().setN(0);
        deleteReply.getWriteCommandReplyBase().setWriteErrors(singleStatusToWriteErrors(status));
    } else if (reply.getLastErrorObject().getNumDocs() > 0) {
        deleteReply.getWriteCommandReplyBase().setN(1);
    }

    return {deleteReply, reply.getValue().value_or(BSONObj())};
}

std::pair<write_ops::UpdateCommandReply, BSONObj> FLEQueryInterfaceImpl::updateWithPreimage(
    const NamespaceString& nss,
    const EncryptionInformation& ei,
    const write_ops::UpdateCommandRequest& updateRequest) {
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
    // TODO SERVER-64143 - writeConcern
    auto ei2 = ei;
    ei2.setCrudProcessed(true);
    findAndModifyRequest.setEncryptionInformation(ei2);

    auto response = _txnClient.runCommand(nss.db(), findAndModifyRequest.toBSON({})).get();
    auto status = getStatusFromWriteCommandReply(response);
    uassertStatusOK(status);

    auto reply =
        write_ops::FindAndModifyCommandReply::parse(IDLParserErrorContext("reply"), response);

    write_ops::UpdateCommandReply updateReply;

    if (!status.isOK()) {
        updateReply.getWriteCommandReplyBase().setN(0);
        updateReply.getWriteCommandReplyBase().setWriteErrors(singleStatusToWriteErrors(status));
    } else {
        if (reply.getRetriedStmtId().has_value()) {
            updateReply.getWriteCommandReplyBase().setRetriedStmtIds(
                std::vector<std::int32_t>{reply.getRetriedStmtId().value()});
        }
        updateReply.getWriteCommandReplyBase().setN(reply.getLastErrorObject().getNumDocs());

        if (reply.getLastErrorObject().getUpserted().has_value()) {
            write_ops::Upserted upserted;
            upserted.setIndex(0);
            upserted.set_id(reply.getLastErrorObject().getUpserted().value());
            updateReply.setUpserted(std::vector<mongo::write_ops::Upserted>{upserted});
        }

        if (reply.getLastErrorObject().getNumDocs() > 0) {
            updateReply.setNModified(1);
            updateReply.getWriteCommandReplyBase().setN(1);
        }
    }

    return {updateReply, reply.getValue().value_or(BSONObj())};
}

write_ops::FindAndModifyCommandReply FLEQueryInterfaceImpl::findAndModify(
    const NamespaceString& nss,
    const EncryptionInformation& ei,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) {

    auto newFindAndModifyRequest = findAndModifyRequest;
    auto ei2 = ei;
    ei2.setCrudProcessed(true);
    newFindAndModifyRequest.setEncryptionInformation(ei2);

    auto response = _txnClient.runCommand(nss.db(), newFindAndModifyRequest.toBSON({})).get();
    auto status = getStatusFromWriteCommandReply(response);
    uassertStatusOK(status);

    return write_ops::FindAndModifyCommandReply::parse(IDLParserErrorContext("reply"), response);
}

std::vector<BSONObj> FLEQueryInterfaceImpl::findDocuments(const NamespaceString& nss,
                                                          BSONObj filter) {
    FindCommandRequest find(nss);
    find.setFilter(filter);
    find.setSingleBatch(true);

    // Throws on error
    return _txnClient.exhaustiveFind(find).get();
}

void processFLEFindS(OperationContext* opCtx, FindCommandRequest* findCommand) {
    fle::processFindCommand(opCtx, findCommand, &getTransactionWithRetriesForMongoS);
}

}  // namespace mongo
