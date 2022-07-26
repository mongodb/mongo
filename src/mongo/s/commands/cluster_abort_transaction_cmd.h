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

#pragma once

#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/transaction_router.h"

namespace mongo {
namespace {

/**
 * Implements the abortTransaction command for a router.
 */
template <typename Impl>
class ClusterAbortTransactionCmdBase
    : public BasicCommandWithRequestParser<ClusterAbortTransactionCmdBase<Impl>> {
public:
    ClusterAbortTransactionCmdBase()
        : BasicCommandWithRequestParser<ClusterAbortTransactionCmdBase<Impl>>(Impl::kName) {}

    using Request = AbortTransaction;
    using Reply = OkReply;
    using BaseType = BasicCommandWithRequestParser<ClusterAbortTransactionCmdBase<Impl>>;
    using RequestParser =
        typename BasicCommandWithRequestParser<ClusterAbortTransactionCmdBase<Impl>>::RequestParser;

    void validateResult(const BSONObj& resultObj) final {
        auto ctx = IDLParserContext("AbortReply");
        if (!BaseType::checkIsErrorStatus(resultObj, ctx)) {
            // Will throw if the result doesn't match the abortReply.
            Reply::parse(ctx, resultObj);
        }
    }

    const std::set<std::string>& apiVersions() const {
        return Impl::getApiVersions();
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        static const Status kOnlyTransactionsReadConcernsSupported{
            ErrorCodes::InvalidOptions, "only read concerns valid in transactions are supported"};
        static const Status kDefaultReadConcernNotPermitted{ErrorCodes::InvalidOptions,
                                                            "default read concern not permitted"};

        // abortTransaction commences running inside a transaction (even though the transaction will
        // be ended by the time it completes).  Therefore it needs to accept any readConcern which
        // is valid within a transaction.  However it is not appropriate to apply the default
        // readConcern, since the readConcern of the transaction (set by the first operation) is
        // what must apply.
        return {{!isReadConcernLevelAllowedInTransaction(level),
                 kOnlyTransactionsReadConcernsSupported},
                {kDefaultReadConcernNotPermitted}};
    }

    std::string help() const override {
        return "Aborts a transaction";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        return Impl::checkAuthForOperation(opCtx);
    }

    bool isTransactionCommand() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& result) final {
        Impl::checkCanRunHere(opCtx);

        auto txnRouter = TransactionRouter::get(opCtx);
        uassert(ErrorCodes::InvalidOptions,
                "abortTransaction can only be run within a session",
                txnRouter);

        auto abortRes = txnRouter.abortTransaction(opCtx);
        CommandHelpers::filterCommandReplyForPassthrough(abortRes, &result);
        return true;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::AbortTransaction::kAuthorizationContract;
    }
};

}  // namespace
}  // namespace mongo
