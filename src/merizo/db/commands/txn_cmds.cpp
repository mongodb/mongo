/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kTransaction

#include "merizo/platform/basic.h"

#include "merizo/bson/bsonobj.h"
#include "merizo/bson/bsonobjbuilder.h"
#include "merizo/db/commands.h"
#include "merizo/db/commands/txn_cmds_gen.h"
#include "merizo/db/curop_failpoint_helpers.h"
#include "merizo/db/op_observer.h"
#include "merizo/db/operation_context.h"
#include "merizo/db/repl/repl_client_info.h"
#include "merizo/db/service_context.h"
#include "merizo/db/transaction_participant.h"
#include "merizo/util/log.h"

namespace merizo {
namespace {

MERIZO_FAIL_POINT_DEFINE(participantReturnNetworkErrorForAbortAfterExecutingAbortLogic);
MERIZO_FAIL_POINT_DEFINE(participantReturnNetworkErrorForCommitAfterExecutingCommitLogic);
MERIZO_FAIL_POINT_DEFINE(hangBeforeCommitingTxn);
MERIZO_FAIL_POINT_DEFINE(hangBeforeAbortingTxn);

class CmdCommitTxn : public BasicCommand {
public:
    CmdCommitTxn() : BasicCommand("commitTransaction") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Commits a transaction";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        IDLParserErrorContext ctx("commitTransaction");
        auto cmd = CommitTransaction::parse(ctx, cmdObj);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        uassert(ErrorCodes::CommandFailed,
                "commitTransaction must be run within a transaction",
                txnParticipant);

        LOG(3) << "Received commitTransaction for transaction with txnNumber "
               << opCtx->getTxnNumber() << " on session " << opCtx->getLogicalSessionId()->toBSON();

        // commitTransaction is retryable.
        if (txnParticipant.transactionIsCommitted()) {
            // We set the client last op to the last optime observed by the system to ensure that
            // we wait for the specified write concern on an optime greater than or equal to the
            // commit oplog entry.
            auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
            replClient.setLastOpToSystemLastOpTime(opCtx);
            if (MERIZO_FAIL_POINT(participantReturnNetworkErrorForCommitAfterExecutingCommitLogic)) {
                uasserted(ErrorCodes::HostUnreachable,
                          "returning network error because failpoint is on");
            }

            return true;
        }

        uassert(ErrorCodes::NoSuchTransaction,
                "Transaction isn't in progress",
                txnParticipant.inMultiDocumentTransaction());

        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangBeforeCommitingTxn, opCtx, "hangBeforeCommitingTxn");

        auto optionalCommitTimestamp = cmd.getCommitTimestamp();
        if (optionalCommitTimestamp) {
            // commitPreparedTransaction will throw if the transaction is not prepared.
            txnParticipant.commitPreparedTransaction(opCtx, optionalCommitTimestamp.get(), {});
        } else {
            // commitUnpreparedTransaction will throw if the transaction is prepared.
            txnParticipant.commitUnpreparedTransaction(opCtx);
        }
        if (MERIZO_FAIL_POINT(participantReturnNetworkErrorForCommitAfterExecutingCommitLogic)) {
            uasserted(ErrorCodes::HostUnreachable,
                      "returning network error because failpoint is on");
        }

        return true;
    }

} commitTxn;

class CmdAbortTxn : public BasicCommand {
public:
    CmdAbortTxn() : BasicCommand("abortTransaction") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Aborts a transaction";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto txnParticipant = TransactionParticipant::get(opCtx);
        uassert(ErrorCodes::CommandFailed,
                "abortTransaction must be run within a transaction",
                txnParticipant);

        LOG(3) << "Received abortTransaction for transaction with txnNumber "
               << opCtx->getTxnNumber() << " on session " << opCtx->getLogicalSessionId()->toBSON();

        uassert(ErrorCodes::NoSuchTransaction,
                "Transaction isn't in progress",
                txnParticipant.inMultiDocumentTransaction());

        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &hangBeforeAbortingTxn, opCtx, "hangBeforeAbortingTxn");

        txnParticipant.abortActiveTransaction(opCtx);

        if (MERIZO_FAIL_POINT(participantReturnNetworkErrorForAbortAfterExecutingAbortLogic)) {
            uasserted(ErrorCodes::HostUnreachable,
                      "returning network error because failpoint is on");
        }

        return true;
    }

} abortTxn;

}  // namespace
}  // namespace merizo
