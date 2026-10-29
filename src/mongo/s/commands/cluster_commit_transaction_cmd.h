// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Implements the commitTransaction command for a router.
 */
template <typename Impl>
class ClusterCommitTransactionCmdBase
    : public BasicCommandWithRequestParser<ClusterCommitTransactionCmdBase<Impl>> {
public:
    ClusterCommitTransactionCmdBase()
        : BasicCommandWithRequestParser<ClusterCommitTransactionCmdBase<Impl>>(Impl::kName) {}

    using Request = CommitTransaction;
    using Reply = OkReply;
    using BaseType = BasicCommandWithRequestParser<ClusterCommitTransactionCmdBase<Impl>>;
    using RequestParser = typename BasicCommandWithRequestParser<
        ClusterCommitTransactionCmdBase<Impl>>::RequestParser;

    void validateResult(const BSONObj& resultObj) final {
        auto ctx = IDLParserContext("CommitReply");
        if (!BaseType::checkIsErrorStatus(resultObj, ctx)) {
            // Will throw if the result doesn't match the commitReply.
            Reply::parse(resultObj, ctx);
        }
    }

    const std::set<std::string>& apiVersions() const override {
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

    std::string help() const override {
        return "Commits a transaction";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        return Impl::checkAuthForOperation(opCtx, dbName, cmdObj);
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
                "commitTransaction can only be run within a session",
                txnRouter);

        auto commitRes =
            txnRouter.commitTransaction(opCtx, requestParser.request().getRecoveryToken());
        CommandHelpers::filterCommandReplyForPassthrough(commitRes, &result);
        return true;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::CommitTransaction::kAuthorizationContract;
    }
};

}  // namespace mongo
