// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/modules.h"

namespace mongo {

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
                                 const DatabaseName& dbName,
                                 const BSONObj& bsonObj) const override {
        return Impl::checkAuthForOperation(opCtx, dbName, bsonObj);
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

        // Instruct the storage engine to not do any extra eviction while aborting transactions so
        // that resources will not get stuck.
        shard_role_details::getRecoveryUnit(opCtx)->setNoEvictionAfterCommitOrRollback();

        auto abortRes = txnRouter.abortTransaction(opCtx);
        CommandHelpers::filterCommandReplyForPassthrough(abortRes, &result);
        return true;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::AbortTransaction::kAuthorizationContract;
    }
};

}  // namespace mongo
