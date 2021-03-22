/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_data_copy_util.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_participant.h"

namespace mongo::resharding::data_copy {

void ensureCollectionExists(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionOptions& options) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    writeConflictRetry(opCtx, "resharding::data_copy::ensureCollectionExists", nss.toString(), [&] {
        AutoGetCollection coll(opCtx, nss, MODE_IX);
        if (coll) {
            return;
        }

        WriteUnitOfWork wuow(opCtx);
        coll.ensureDbExists()->createCollection(opCtx, nss, options);
        wuow.commit();
    });
}

void ensureCollectionDropped(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<CollectionUUID>& uuid) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    writeConflictRetry(
        opCtx, "resharding::data_copy::ensureCollectionDropped", nss.toString(), [&] {
            AutoGetCollection coll(opCtx, nss, MODE_X);
            if (!coll || (uuid && coll->uuid() != uuid)) {
                // If the collection doesn't exist or exists with a different UUID, then the
                // requested collection has been dropped already.
                return;
            }

            WriteUnitOfWork wuow(opCtx);
            uassertStatusOK(coll.getDb()->dropCollectionEvenIfSystem(opCtx, nss));
            wuow.commit();
        });
}

Value findHighestInsertedId(OperationContext* opCtx, const CollectionPtr& collection) {
    auto findCommand = std::make_unique<FindCommand>(collection->ns());
    findCommand->setLimit(1);
    findCommand->setSort(BSON("_id" << -1));

    auto recordId =
        Helpers::findOne(opCtx, collection, std::move(findCommand), true /* requireIndex */);
    if (recordId.isNull()) {
        return Value{};
    }

    auto doc = collection->docFor(opCtx, recordId).value();
    auto value = Value{doc["_id"]};
    uassert(4929300,
            "Missing _id field for document in temporary resharding collection",
            !value.missing());

    return value;
}

boost::optional<SharedSemiFuture<void>> withSessionCheckedOut(OperationContext* opCtx,
                                                              LogicalSessionId lsid,
                                                              TxnNumber txnNumber,
                                                              boost::optional<StmtId> stmtId,
                                                              unique_function<void()> callable) {

    opCtx->setLogicalSessionId(std::move(lsid));
    opCtx->setTxnNumber(txnNumber);

    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    try {
        txnParticipant.beginOrContinue(opCtx, txnNumber, boost::none, boost::none);

        if (stmtId && txnParticipant.checkStatementExecuted(opCtx, *stmtId)) {
            // Skip the incoming statement because it has already been logged locally.
            return boost::none;
        }
    } catch (const ExceptionFor<ErrorCodes::TransactionTooOld>&) {
        // txnNumber < txnParticipant.o().activeTxnNumber
        return boost::none;
    } catch (const ExceptionFor<ErrorCodes::IncompleteTransactionHistory>&) {
        // txnNumber == txnParticipant.o().activeTxnNumber &&
        // !txnParticipant.transactionIsInRetryableWriteMode()
        //
        // If the transaction chain is incomplete because the oplog was truncated, just ignore the
        // incoming write and don't attempt to "patch up" the missing pieces.
        //
        // This situation could also happen if the client reused the txnNumber for distinct
        // operations (which is a violation of the protocol). The client would receive an error if
        // they attempted to retry the retryable write they had reused the txnNumber with so it is
        // safe to leave config.transactions as-is.
        return boost::none;
    } catch (const ExceptionFor<ErrorCodes::PreparedTransactionInProgress>&) {
        // txnParticipant.transactionIsPrepared()
        return txnParticipant.onExitPrepare();
    }

    callable();
    return boost::none;
}

}  // namespace mongo::resharding::data_copy
