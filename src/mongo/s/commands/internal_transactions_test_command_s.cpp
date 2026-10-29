// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/resource_yielder.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/commands/internal_transactions_test_command.h"
#include "mongo/s/transaction_router_resource_yielder.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {
namespace {

class InternalTransactionsTestCommandS
    : public InternalTransactionsTestCommandBase<InternalTransactionsTestCommandS> {
public:
    static txn_api::SyncTransactionWithRetries getTxn(
        OperationContext* opCtx,
        std::shared_ptr<executor::TaskExecutor> executor,
        std::string_view commandName,
        bool useClusterClient) {
        auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

        return txn_api::SyncTransactionWithRetries(
            opCtx,
            executor,
            TransactionRouterResourceYielder::makeForLocalHandoff(),
            inlineExecutor);
    }
};

MONGO_REGISTER_COMMAND(InternalTransactionsTestCommandS).testOnly().forRouter();
}  // namespace
}  // namespace mongo
