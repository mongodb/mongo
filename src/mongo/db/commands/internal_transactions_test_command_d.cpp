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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/cluster_transaction_api.h"
#include "mongo/db/commands.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/commands/internal_transactions_test_command.h"

#include <memory>
#include <string>
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
        StringData commandName,
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
