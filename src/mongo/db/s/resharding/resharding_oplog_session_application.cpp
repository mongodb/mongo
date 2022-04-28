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

#include "mongo/db/s/resharding/resharding_oplog_session_application.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/redaction.h"

namespace mongo {

ReshardingOplogSessionApplication::ReshardingOplogSessionApplication(NamespaceString oplogBufferNss)
    : _oplogBufferNss(std::move(oplogBufferNss)) {}


boost::optional<repl::OpTime> ReshardingOplogSessionApplication::_logPrePostImage(
    OperationContext* opCtx,
    const ReshardingDonorOplogId& opId,
    const repl::OpTime& prePostImageOpTime) const {
    DBDirectClient client(opCtx);

    auto prePostImageTxnOpId =
        ReshardingDonorOplogId{opId.getClusterTime(), prePostImageOpTime.getTimestamp()};
    auto prePostImageNonTxnOpId = ReshardingDonorOplogId{prePostImageOpTime.getTimestamp(),
                                                         prePostImageOpTime.getTimestamp()};
    auto result =
        client.findOne(_oplogBufferNss,
                       BSON(repl::OplogEntry::k_idFieldName
                            << BSON("$in" << BSON_ARRAY(prePostImageTxnOpId.toBSON()
                                                        << prePostImageNonTxnOpId.toBSON()))));

    tassert(6344401,
            str::stream() << "Could not find pre/post image oplog entry with op time "
                          << redact(prePostImageOpTime.toBSON()),
            !result.isEmpty());

    auto prePostImageOp = uassertStatusOK(repl::DurableOplogEntry::parse(result));
    uassert(4990408,
            str::stream() << "Expected a no-op oplog entry for pre/post image oplog entry: "
                          << redact(prePostImageOp.toBSON()),
            prePostImageOp.getOpType() == repl::OpTypeEnum::kNoop);

    auto noopEntry = uassertStatusOK(repl::MutableOplogEntry::parse(prePostImageOp.toBSON()));
    // Reset the OpTime so logOp() can assign a new one.
    noopEntry.setOpTime(OplogSlot());
    noopEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());
    noopEntry.setFromMigrate(true);

    return writeConflictRetry(
        opCtx,
        "ReshardingOplogSessionApplication::_logPrePostImage",
        NamespaceString::kRsOplogNamespace.ns(),
        [&] {
            AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);

            WriteUnitOfWork wuow(opCtx);
            const auto& opTime = repl::logOp(opCtx, &noopEntry);

            uassert(4990409,
                    str::stream() << "Failed to create new oplog entry for pre/post image oplog"
                                     " entry with original opTime: "
                                  << prePostImageOp.getOpTime().toString() << ": "
                                  << redact(noopEntry.toBSON()),
                    !opTime.isNull());

            wuow.commit();
            return opTime;
        });
}

boost::optional<SharedSemiFuture<void>> ReshardingOplogSessionApplication::tryApplyOperation(
    OperationContext* opCtx, const mongo::repl::OplogEntry& op) const {
    invariant(op.getSessionId());
    invariant(op.getTxnNumber());
    invariant(op.get_id());

    auto lsid = *op.getSessionId();
    if (isInternalSessionForNonRetryableWrite(lsid)) {
        // Skip internal sessions for non-retryable writes since they only support transactions
        // and those transactions are not retryable so there is no need to transfer the write
        // history to resharding recipient(s).
        return boost::none;
    }
    if (isInternalSessionForRetryableWrite(lsid)) {
        // The oplog preparer should have turned each applyOps oplog entry for a retryable internal
        // transaction into retryable write CRUD oplog entries.
        invariant(op.getCommandType() != repl::OplogEntry::CommandType::kApplyOps);

        if (op.getCommandType() == repl::OplogEntry::CommandType::kAbortTransaction) {
            // Skip this oplog entry since there is no retryable write history to apply and writing
            // a sentinel noop oplog entry would make retryable write statements that successfully
            // executed outside of this internal transaction not retryable.
            return boost::none;
        }
    }

    auto txnNumber = *op.getTxnNumber();
    bool isRetryableWrite = op.isCrudOpType();

    auto o2Field =
        isRetryableWrite ? op.getEntry().getRaw() : TransactionParticipant::kDeadEndSentinel;

    auto stmtIds =
        isRetryableWrite ? op.getStatementIds() : std::vector<StmtId>{kIncompleteHistoryStmtId};
    invariant(!stmtIds.empty());

    auto opId = ReshardingDonorOplogId::parse({"ReshardingOplogSessionApplication"},
                                              op.get_id()->getDocument().toBson());

    boost::optional<repl::OpTime> preImageOpTime;
    if (auto originalPreImageOpTime = op.getPreImageOpTime()) {
        preImageOpTime = _logPrePostImage(opCtx, opId, *originalPreImageOpTime);
    }

    boost::optional<repl::OpTime> postImageOpTime;
    if (auto originalPostImageOpTime = op.getPostImageOpTime()) {
        postImageOpTime = _logPrePostImage(opCtx, opId, *originalPostImageOpTime);
    }

    return resharding::data_copy::withSessionCheckedOut(
        opCtx, lsid, txnNumber, stmtIds.front(), [&] {
            resharding::data_copy::updateSessionRecord(opCtx,
                                                       std::move(o2Field),
                                                       std::move(stmtIds),
                                                       std::move(preImageOpTime),
                                                       std::move(postImageOpTime));
        });
}

}  // namespace mongo
