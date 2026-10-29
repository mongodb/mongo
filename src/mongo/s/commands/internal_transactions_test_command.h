// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/internal_transactions_test_command_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/modules.h"

#include <future>

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

            txn.run(opCtx,
                    [sharedBlock, opCtx](const txn_api::TransactionClient& txnClient,
                                         ExecutorPtr txnExec) {
                        sharedBlock->responses.clear();

                        // Iterate through commands and record responses for each. Return
                        // immediately if we encounter a response with a retriedStmtId. This field
                        // indicates that the command and everything following it have already been
                        // executed.
                        for (const auto& commandInfo : sharedBlock->commandInfos) {
                            const auto& dbName = commandInfo.getDbName();
                            const auto& command = commandInfo.getCommand();
                            auto exhaustCursor = commandInfo.getExhaustCursor();

                            if (exhaustCursor == boost::optional<bool>(true)) {
                                // We can't call a getMore without knowing its cursor's id, so we
                                // use the exhaustiveFind helper to test getMores. Make an
                                // OpMsgRequest from the command to append $db, which
                                // FindCommandRequest expects.
                                auto findOpMsgRequest = OpMsgRequestBuilder::create(
                                    auth::ValidatedTenancyScope::get(opCtx), dbName, command);
                                auto findCommand = FindCommandRequest::parse(
                                    findOpMsgRequest.body, IDLParserContext("FindCommandRequest"));

                                auto docs = txnClient.exhaustiveFindSync(findCommand);

                                BSONObjBuilder resBob;
                                resBob.append("docs", std::move(docs));
                                sharedBlock->responses.emplace_back(resBob.obj());
                                continue;
                            }

                            const auto res = txnClient.runCommandSync(dbName, command);

                            sharedBlock->responses.emplace_back(
                                CommandHelpers::filterCommandReplyForPassthrough(
                                    res.removeField("recoveryToken")));

                            uassertStatusOK(getStatusFromWriteCommandReply(res));

                            // Exit if we are reexecuting commands in a retryable write, identified
                            // by a populated retriedStmtId. eoo() is false if field is found.
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
            return NamespaceString(Base::request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(Base::request().getDbName().tenantId()),
                        ActionType::internal));
        }

        std::shared_ptr<executor::TaskExecutor> getTransactionExecutor() {
            static std::mutex mutex;
            static std::shared_ptr<executor::ThreadPoolTaskExecutor> executor;

            std::lock_guard<std::mutex> lg(mutex);
            if (!executor) {
                executor = executor::ThreadPoolTaskExecutor::create(
                    ThreadPool::make({
                        .poolName = "InternalTransaction",
                        .minThreads = 0,
                        .maxThreads = 4,
                    }),
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
