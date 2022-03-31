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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cluster_transaction_api.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/transaction_api.h"
#include "mongo/logv2/log.h"
#include "mongo/s/commands/internal_transactions_test_commands_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/transaction_router_resource_yielder.h"

namespace mongo {
namespace {

class InternalTransactionsTestCommand final : public TypedCommand<InternalTransactionsTestCommand> {
public:
    using Request = TestInternalTransactions;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        TestInternalTransactionsReply typedRun(OperationContext* opCtx) {
            Grid::get(opCtx)->assertShardingIsInitialized();

            auto fixedExec = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
            auto txn = txn_api::TransactionWithRetries(
                opCtx,
                fixedExec,
                std::make_unique<txn_api::details::SEPTransactionClient>(
                    opCtx,
                    fixedExec,
                    std::make_unique<txn_api::details::ClusterSEPTransactionClientBehaviors>(
                        opCtx->getServiceContext())),
                TransactionRouterResourceYielder::makeForLocalHandoff());

            struct SharedBlock {
                SharedBlock(std::vector<TestInternalTransactionsCommandInfo> commandInfos_)
                    : commandInfos(commandInfos_) {}

                std::vector<TestInternalTransactionsCommandInfo> commandInfos;
                std::vector<BSONObj> responses;
            };
            auto sharedBlock = std::make_shared<SharedBlock>(request().getCommandInfos());

            // Swallow errors and let clients inspect the responses array to determine success /
            // failure.
            (void)txn.runSyncNoThrow(
                opCtx,
                [sharedBlock](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                    for (const auto& commandInfo : sharedBlock->commandInfos) {
                        const auto& dbName = commandInfo.getDbName();
                        const auto& command = commandInfo.getCommand();
                        auto assertSucceeds = commandInfo.getAssertSucceeds();
                        auto exhaustCursor = commandInfo.getExhaustCursor();

                        if (exhaustCursor == boost::optional<bool>(true)) {
                            // We can't call a getMore without knowing its cursor's id, so we
                            // use the exhaustiveFind helper to test getMores. Make an OpMsgRequest
                            // from the command to append $db, which FindCommandRequest expects.
                            auto findOpMsgRequest = OpMsgRequest::fromDBAndBody(dbName, command);
                            auto findCommand = FindCommandRequest::parse(
                                IDLParserErrorContext("FindCommandRequest", false /* apiStrict */),
                                findOpMsgRequest.body);

                            auto docs = txnClient.exhaustiveFind(findCommand).get();

                            BSONObjBuilder resBob;
                            resBob.append("docs", std::move(docs));
                            sharedBlock->responses.emplace_back(resBob.obj());
                            continue;
                        }

                        auto res = txnClient.runCommand(dbName, command).get();
                        sharedBlock->responses.emplace_back(
                            CommandHelpers::filterCommandReplyForPassthrough(
                                res.removeField("recoveryToken")));

                        if (assertSucceeds) {
                            // Note this only inspects the top level ok field for non-write
                            // commands.
                            uassertStatusOK(getStatusFromWriteCommandReply(res));
                        }
                    }

                    return SemiFuture<void>::makeReady();
                });

            return TestInternalTransactionsReply(std::move(sharedBlock->responses));
        };

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

    std::string help() const override {
        return "Internal command for testing internal transactions";
    }

    // This command can use the transaction API to run commands on different databases, so a single
    // user database doesn't apply and we restrict this to only the admin database.
    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_TEST_COMMAND(InternalTransactionsTestCommand);

}  // namespace
}  // namespace mongo
