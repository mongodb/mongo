// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/db/cluster_transaction_api.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/resource_yielder.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/commands/internal_transactions_test_command.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {
namespace {

class InternalTransactionsTestCommandD
    : public InternalTransactionsTestCommandBase<InternalTransactionsTestCommandD> {
public:
    static txn_api::SyncTransactionWithRetries getTxn(
        OperationContext* opCtx,
        std::shared_ptr<executor::TaskExecutor> executor,
        std::string_view commandName,
        bool useClusterClient) {
        auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
        // If a sharded mongod is acting as a mongos, it will need special routing behaviors.
        if (useClusterClient) {
            auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);
            return txn_api::SyncTransactionWithRetries(
                opCtx,
                executor,
                TransactionParticipantResourceYielder::make(commandName),
                inlineExecutor,
                std::make_unique<txn_api::details::SEPTransactionClient>(
                    opCtx,
                    inlineExecutor,
                    sleepInlineExecutor,
                    executor,
                    std::make_unique<txn_api::details::ClusterSEPTransactionClientBehaviors>(
                        opCtx->getServiceContext())));
        }

        return txn_api::SyncTransactionWithRetries(
            opCtx,
            executor,
            TransactionParticipantResourceYielder::make(commandName),
            inlineExecutor);
    }
};

MONGO_REGISTER_COMMAND(InternalTransactionsTestCommandD).testOnly().forShard();
}  // namespace
}  // namespace mongo
