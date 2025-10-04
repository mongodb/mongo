/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/client/read_preference.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/transaction_router.h"

#include <memory>
#include <vector>

namespace mongo {

namespace transaction_request_sender_details {
std::vector<AsyncRequestsSender::Request> attachTxnDetails(
    OperationContext* opCtx, const std::vector<AsyncRequestsSender::Request>& requests);

void processReplyMetadata(OperationContext* opCtx,
                          const AsyncRequestsSender::Response& response,
                          bool forAsyncGetMore = false);

/**
 * Process metadata for an asynchronous getMore reply of type 'ParsedParticipantResponseMetadata'.
 * This will add additional transaction participants to the transaction router in case the reply
 * contains additional participants in its metadata.
 */
void processReplyMetadataForAsyncGetMore(
    OperationContext* opCtx,
    const ShardId& shardId,
    const TransactionRouter::ParsedParticipantResponseMetadata& parsedResponse);

}  // namespace transaction_request_sender_details

/**
 * Wrapper for AsyncRequestSender that attaches multi-statement transaction related fields to
 * remote requests and also perform multi-statement transaction related post processing when
 * receiving responses.
 */
class MultiStatementTransactionRequestsSender {
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
        AsyncRequestsSender::ShardHostMap designatedHostsMap = {});

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
