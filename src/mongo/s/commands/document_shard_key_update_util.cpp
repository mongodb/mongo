/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding
#include "mongo/platform/basic.h"

#include "mongo/s/commands/document_shard_key_update_util.h"

#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/would_change_owning_shard_exception.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/cluster_write.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeInsertOnUpdateShardKey);

/**
 * Calls into the command execution stack to run the given command. Will blindly uassert on any
 * error returned by a command. If the original update was sent with {upsert: false}, returns
 * whether or not we deleted the original doc and inserted the new one sucessfully. If the original
 * update was sent with {upsert: true}, returns whether or not we inserted the new doc successfully.
 */
bool executeOperationsAsPartOfShardKeyUpdate(OperationContext* opCtx,
                                             const BSONObj& deleteCmdObj,
                                             const BSONObj& insertCmdObj,
                                             const StringData db,
                                             const bool shouldUpsert) {
    auto deleteOpMsg = OpMsgRequest::fromDBAndBody(db, deleteCmdObj);
    auto deleteRequest = BatchedCommandRequest::parseDelete(deleteOpMsg);

    BatchedCommandResponse deleteResponse;
    BatchWriteExecStats deleteStats;

    ClusterWriter::write(opCtx, deleteRequest, &deleteStats, &deleteResponse);
    uassertStatusOK(deleteResponse.toStatus());
    // If shouldUpsert is true, this means the original command specified {upsert: true} and did not
    // match any docs, so we should not match any when doing this delete. If shouldUpsert is false
    // and we do not delete any document, this is essentially equivalent to not matching a doc and
    // we should not insert.
    if (shouldUpsert) {
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "Delete matched a document when it should not have.",
                deleteResponse.getN() == 0);
    } else if (deleteResponse.getN() != 1) {
        return false;
    }

    if (MONGO_unlikely(hangBeforeInsertOnUpdateShardKey.shouldFail())) {
        log() << "Hit hangBeforeInsertOnUpdateShardKey failpoint";
        hangBeforeInsertOnUpdateShardKey.pauseWhileSet(opCtx);
    }

    auto insertOpMsg = OpMsgRequest::fromDBAndBody(db, insertCmdObj);
    auto insertRequest = BatchedCommandRequest::parseInsert(insertOpMsg);

    BatchedCommandResponse insertResponse;
    BatchWriteExecStats insertStats;
    ClusterWriter::write(opCtx, insertRequest, &insertStats, &insertResponse);
    uassertStatusOK(insertResponse.toStatus());
    uassert(ErrorCodes::NamespaceNotFound,
            "Document not successfully inserted while changing shard key for namespace " +
                insertRequest.getNS().toString(),
            insertResponse.getN() == 1);

    return true;
}

/**
 * Creates the delete op that will be used to delete the pre-image document. Will also attach the
 * original document _id retrieved from 'updatePreImage'.
 */
write_ops::Delete createShardKeyDeleteOp(const NamespaceString& nss,
                                         const BSONObj& updatePreImage) {
    write_ops::Delete deleteOp(nss);
    deleteOp.setDeletes({[&] {
        write_ops::DeleteOpEntry entry;
        entry.setQ(updatePreImage);
        entry.setMulti(false);
        return entry;
    }()});

    return deleteOp;
}

/**
 * Creates the insert op that will be used to insert the new document with the post-update image.
 */
write_ops::Insert createShardKeyInsertOp(const NamespaceString& nss,
                                         const BSONObj& updatePostImage) {
    write_ops::Insert insertOp(nss);
    insertOp.setDocuments({updatePostImage});
    return insertOp;
}

}  //  namespace

namespace documentShardKeyUpdateUtil {

bool updateShardKeyForDocument(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const WouldChangeOwningShardInfo& documentKeyChangeInfo) {
    auto updatePreImage = documentKeyChangeInfo.getPreImage().getOwned();
    auto updatePostImage = documentKeyChangeInfo.getPostImage().getOwned();

    auto deleteCmdObj = constructShardKeyDeleteCmdObj(nss, updatePreImage);
    auto insertCmdObj = constructShardKeyInsertCmdObj(nss, updatePostImage);

    return executeOperationsAsPartOfShardKeyUpdate(
        opCtx, deleteCmdObj, insertCmdObj, nss.db(), documentKeyChangeInfo.getShouldUpsert());
}

void startTransactionForShardKeyUpdate(OperationContext* opCtx) {
    auto txnRouter = TransactionRouter::get(opCtx);
    invariant(txnRouter);

    auto txnNumber = opCtx->getTxnNumber();
    invariant(txnNumber);

    txnRouter.beginOrContinueTxn(opCtx, *txnNumber, TransactionRouter::TransactionActions::kStart);
}

BSONObj commitShardKeyUpdateTransaction(OperationContext* opCtx) {
    auto txnRouter = TransactionRouter::get(opCtx);
    invariant(txnRouter);

    return txnRouter.commitTransaction(opCtx, boost::none);
}

BSONObj constructShardKeyDeleteCmdObj(const NamespaceString& nss, const BSONObj& updatePreImage) {
    auto deleteOp = createShardKeyDeleteOp(nss, updatePreImage);
    return deleteOp.toBSON({});
}

BSONObj constructShardKeyInsertCmdObj(const NamespaceString& nss, const BSONObj& updatePostImage) {
    auto insertOp = createShardKeyInsertOp(nss, updatePostImage);
    return insertOp.toBSON({});
}

}  // namespace documentShardKeyUpdateUtil
}  // namespace mongo
