// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/read_preference.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

namespace mongo {

namespace transaction_request_sender_details {
[[MONGO_MOD_PUBLIC]] std::vector<AsyncRequestsSender::Request> attachTxnDetails(
    OperationContext* opCtx, const std::vector<AsyncRequestsSender::Request>& requests);

[[MONGO_MOD_PUBLIC]] void processReplyMetadata(OperationContext* opCtx,
                                               const AsyncRequestsSender::Response& response,
                                               bool forAsyncGetMore = false);

/**
 * Process metadata for an asynchronous getMore reply of type 'ParsedParticipantResponseMetadata'.
 * This will add additional transaction participants to the transaction router in case the reply
 * contains additional participants in its metadata.
 */
[[MONGO_MOD_PUBLIC]] void processReplyMetadataForAsyncGetMore(
    OperationContext* opCtx,
    const ShardId& shardId,
    const TransactionRouter::ParsedParticipantResponseMetadata& parsedResponse);

}  // namespace transaction_request_sender_details

/**
 * Wrapper for AsyncRequestSender that attaches multi-statement transaction related fields to
 * remote requests and also perform multi-statement transaction related post processing when
 * receiving responses.
 */
class [[MONGO_MOD_PUBLIC]] MultiStatementTransactionRequestsSender {
public:
    /**
     * Constructs a new MultiStatementTransactionRequestsSender. The OperationContext* and
     * TaskExecutor* must remain valid for the lifetime of the ARS.
     */
    MultiStatementTransactionRequestsSender(
        OperationContext* opCtx,
        std::shared_ptr<executor::TaskExecutor> executor,
        const DatabaseName& dbName,
        const std::vector<AsyncRequestsSender::Request>& requests,
        const ReadPreferenceSetting& readPreference,
        Shard::RetryPolicy retryPolicy,
        const AsyncRequestsSender::ShardHostMap& designatedHostsMap = {});

    ~MultiStatementTransactionRequestsSender();

    bool done() const;

    /**
     * Fetches the next response and validates it.
     * Internally calls 'nextResponse()', then 'validateResponse()'. Throws if the response is not
     * valid.
     */
    AsyncRequestsSender::Response next(bool forMergeCursors = false);

    /**
     * Fetches the next response, without validating it.
     */
    AsyncRequestsSender::Response nextResponse();

    /**
     * Validates a received response. Throws if the response is not valid.
     */
    void validateResponse(const AsyncRequestsSender::Response& response,
                          bool forMergeCursors) const;

    void stopRetrying();

private:
    OperationContext* _opCtx;
    std::unique_ptr<AsyncRequestsSender> _ars;
};

}  // namespace mongo
