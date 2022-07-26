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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/internal_transactions_test_command_gen.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/transaction_api.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/future.h"

namespace mongo {

template <typename Impl>
class InternalTransactionsTestCommandBase : public TypedCommand<Impl> {
public:
    using Request = TestInternalTransactions;

    class Invocation final : public TypedCommand<Impl>::InvocationBase {
    public:
        using Base = typename TypedCommand<Impl>::InvocationBase;
        using Base::Base;

        TestInternalTransactionsCommandReply typedRun(OperationContext* opCtx) {
            struct SharedBlock {
                SharedBlock(std::vector<TestInternalTransactionsCommandInfo> commandInfos_)
                    : commandInfos(commandInfos_) {}

                std::vector<TestInternalTransactionsCommandInfo> commandInfos;
                std::vector<BSONObj> responses;
            };

            auto sharedBlock = std::make_shared<SharedBlock>(Base::request().getCommandInfos());

            const auto executor = Grid::get(opCtx)->isShardingInitialized()
                ? Grid::get(opCtx)->getExecutorPool()->getFixedExecutor()
                : getTransactionExecutor();

            // If internalTransactionsTestCommand is received by a mongod, it should be instantiated
            // with the TransactionParticipant's resource yielder. If on a mongos, txn should be
            // instantiated with the TransactionRouter's resource yielder.
            auto txn = Impl::getTxn(opCtx,
                                    std::move(executor),
                                    Base::request().kCommandName,
                                    Base::request().getUseClusterClient());

            txn.run(
                opCtx,
                [sharedBlock](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
                    sharedBlock->responses.clear();

                    // Iterate through commands and record responses for each. Return immediately if
                    // we encounter a response with a retriedStmtId. This field indicates that the
                    // command and everything following it have already been executed.
                    for (const auto& commandInfo : sharedBlock->commandInfos) {
                        const auto& dbName = commandInfo.getDbName();
                        const auto& command = commandInfo.getCommand();
                        auto exhaustCursor = commandInfo.getExhaustCursor();

                        if (exhaustCursor == boost::optional<bool>(true)) {
                            // We can't call a getMore without knowing its cursor's id, so we
                            // use the exhaustiveFind helper to test getMores. Make an OpMsgRequest
                            // from the command to append $db, which FindCommandRequest expects.
                            auto findOpMsgRequest = OpMsgRequest::fromDBAndBody(dbName, command);
                            auto findCommand = FindCommandRequest::parse(
                                IDLParserContext("FindCommandRequest", false /* apiStrict */),
                                findOpMsgRequest.body);

                            auto docs = txnClient.exhaustiveFind(findCommand).get();

                            BSONObjBuilder resBob;
                            resBob.append("docs", std::move(docs));
                            sharedBlock->responses.emplace_back(resBob.obj());
                            continue;
                        }

                        const auto res = txnClient.runCommand(dbName, command).get();
                        sharedBlock->responses.emplace_back(
                            CommandHelpers::filterCommandReplyForPassthrough(
                                res.removeField("recoveryToken")));

                        uassertStatusOK(getStatusFromWriteCommandReply(res));

                        // Exit if we are reexecuting commands in a retryable write, identified by a
                        // populated retriedStmtId. eoo() is false if field is found.
                        const auto isRetryStmt = !(res.getField("retriedStmtIds").eoo() &&
                                                   res.getField("retriedStmtId").eoo());
                        if (isRetryStmt) {
                            break;
                        }
                    }
                    return SemiFuture<void>::makeReady();
                });

            return TestInternalTransactionsCommandReply(std::move(sharedBlock->responses));
        };

        NamespaceString ns() const override {
            return NamespaceString(Base::request().getDbName(), "");
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }

        std::shared_ptr<executor::TaskExecutor> getTransactionExecutor() {
            static Mutex mutex =
                MONGO_MAKE_LATCH("InternalTransactionsTestCommandExecutor::_mutex");
            static std::shared_ptr<executor::ThreadPoolTaskExecutor> executor;

            stdx::lock_guard<Latch> lg(mutex);
            if (!executor) {
                ThreadPool::Options options;
                options.poolName = "InternalTransaction";
                options.minThreads = 0;
                options.maxThreads = 4;
                executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
                    std::make_unique<ThreadPool>(std::move(options)),
                    executor::makeNetworkInterface("InternalTransactionNetwork"));
                executor->startup();
            }
            return executor;
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

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }
};

}  // namespace mongo
