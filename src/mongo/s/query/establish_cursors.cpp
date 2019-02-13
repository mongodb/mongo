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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/establish_cursors.h"

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/killcursors_request.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

std::vector<RemoteCursor> establishCursors(OperationContext* opCtx,
                                           executor::TaskExecutor* executor,
                                           const NamespaceString& nss,
                                           const ReadPreferenceSetting readPref,
                                           const std::vector<std::pair<ShardId, BSONObj>>& remotes,
                                           bool allowPartialResults,
                                           Shard::RetryPolicy retryPolicy) {
    // Construct the requests
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& remote : remotes) {
        requests.emplace_back(remote.first, remote.second);
    }

    // Send the requests
    MultiStatementTransactionRequestsSender ars(
        opCtx, executor, nss.db().toString(), std::move(requests), readPref, retryPolicy);

    std::vector<RemoteCursor> remoteCursors;
    try {
        // Get the responses
        while (!ars.done()) {
            try {
                auto response = ars.next();
                // Note the shardHostAndPort may not be populated if there was an error, so be sure
                // to do this after parsing the cursor response to ensure the response was ok.
                // Additionally, be careful not to push into 'remoteCursors' until we are sure we
                // have a valid cursor, since the error handling path will attempt to clean up
                // anything in 'remoteCursors'
                auto cursors = CursorResponse::parseFromBSONMany(
                    uassertStatusOK(std::move(response.swResponse)).data);

                for (auto& cursor : cursors) {
                    if (cursor.isOK()) {
                        RemoteCursor remoteCursor;
                        remoteCursor.setCursorResponse(std::move(cursor.getValue()));
                        remoteCursor.setShardId(std::move(response.shardId));
                        remoteCursor.setHostAndPort(*response.shardHostAndPort);
                        remoteCursors.push_back(std::move(remoteCursor));
                    }
                }

                // Throw if there is any error and then the catch block below will do the cleanup.
                for (auto& cursor : cursors) {
                    uassertStatusOK(cursor.getStatus());
                }

            } catch (const DBException& ex) {
                // Retriable errors are swallowed if 'allowPartialResults' is true.
                if (allowPartialResults &&
                    std::find(RemoteCommandRetryScheduler::kAllRetriableErrors.begin(),
                              RemoteCommandRetryScheduler::kAllRetriableErrors.end(),
                              ex.code()) !=
                        RemoteCommandRetryScheduler::kAllRetriableErrors.end()) {
                    continue;
                }
                throw;  // Fail this loop.
            }
        }
        return remoteCursors;
    } catch (const DBException&) {
        // If one of the remotes had an error, we make a best effort to finish retrieving responses
        // for other requests that were already sent, so that we can send killCursors to any cursors
        // that we know were established.
        try {
            // Do not schedule any new requests.
            ars.stopRetrying();

            // Collect responses from all requests that were already sent.
            while (!ars.done()) {
                auto response = ars.next();

                // Check if the response contains an established cursor, and if so, store it.
                StatusWith<CursorResponse> swCursorResponse(
                    response.swResponse.isOK()
                        ? CursorResponse::parseFromBSON(response.swResponse.getValue().data)
                        : response.swResponse.getStatus());

                if (swCursorResponse.isOK()) {
                    RemoteCursor cursor;
                    cursor.setShardId(std::move(response.shardId));
                    cursor.setHostAndPort(*response.shardHostAndPort);
                    cursor.setCursorResponse(std::move(swCursorResponse.getValue()));
                    remoteCursors.push_back(std::move(cursor));
                }
            }

            // Schedule killCursors against all cursors that were established.
            killRemoteCursors(opCtx, executor, std::move(remoteCursors), nss);
        } catch (const DBException&) {
            // Ignore the new error and rethrow the original one.
        }

        throw;
    }
}

void killRemoteCursors(OperationContext* opCtx,
                       executor::TaskExecutor* executor,
                       std::vector<RemoteCursor>&& remoteCursors,
                       const NamespaceString& nss) {
    for (auto&& remoteCursor : remoteCursors) {
        killRemoteCursor(opCtx, executor, std::move(remoteCursor), nss);
    }
}

void killRemoteCursor(OperationContext* opCtx,
                      executor::TaskExecutor* executor,
                      RemoteCursor&& cursor,
                      const NamespaceString& nss) {
    BSONObj cmdObj = KillCursorsRequest(nss, {cursor.getCursorResponse().getCursorId()}).toBSON();
    executor::RemoteCommandRequest request(
        cursor.getHostAndPort(), nss.db().toString(), cmdObj, opCtx);

    // We do not process the response to the killCursors request (we make a good-faith
    // attempt at cleaning up the cursors, but ignore any returned errors).
    executor->scheduleRemoteCommand(request, [](auto const&) {}).getStatus().ignore();
}

}  // namespace mongo
