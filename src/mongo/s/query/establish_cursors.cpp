
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

#include <set>

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/killcursors_request.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

/**
 * This class wraps logic for establishing cursors using a MultiStatementTransactionRequestsSender.
 */
class CursorEstablisher {
public:
    CursorEstablisher(OperationContext* opCtx,
                      executor::TaskExecutor* executor,
                      const NamespaceString& nss,
                      bool allowPartialResults)
        : _opCtx(opCtx),
          _executor{std::move(executor)},
          _nss(nss),
          _allowPartialResults(allowPartialResults) {}

    /**
     * Make a RequestSender and thus send requests.
     */
    void sendRequests(const ReadPreferenceSetting& readPref,
                      const std::vector<std::pair<ShardId, BSONObj>>& remotes,
                      Shard::RetryPolicy retryPolicy);

    /**
     * Wait for a single response via the RequestSender.
     */
    void waitForResponse() noexcept;

    /**
     * Wait for all responses via the RequestSender.
     */
    void waitForResponses() noexcept {
        while (!_ars->done()) {
            waitForResponse();
        }
    }

    /**
     * If any request recieved a non-retriable error response and partial results are not allowed,
     * cancel any requests that may have succeeded and throw the first such error encountered.
     */
    void checkForFailedRequests();

    /**
     * Take all cursors currently tracked by the CursorEstablsher.
     */
    std::vector<RemoteCursor> takeCursors() {
        return std::exchange(_remoteCursors, {});
    };

private:
    void _handleFailure(const AsyncRequestsSender::Response& response, Status status) noexcept;

    OperationContext* const _opCtx;
    executor::TaskExecutor* const _executor;
    const NamespaceString _nss;
    const bool _allowPartialResults;

    std::unique_ptr<AsyncRequestsSender> _ars;

    boost::optional<Status> _maybeFailure;
    std::vector<RemoteCursor> _remoteCursors;
};

void CursorEstablisher::sendRequests(const ReadPreferenceSetting& readPref,
                                     const std::vector<std::pair<ShardId, BSONObj>>& remotes,
                                     Shard::RetryPolicy retryPolicy) {
    // Construct the requests
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& remote : remotes) {
        requests.emplace_back(remote.first, remote.second);
    }

    LOG(3) << "Establishing cursors on remotes {"
           << "opId: " << _opCtx->getOpID() << ","
           << "numRemotes: " << remotes.size() << "}";

    // Send the requests
    _ars = std::make_unique<AsyncRequestsSender>(
        _opCtx, _executor, _nss.db().toString(), std::move(requests), readPref, retryPolicy);
}

void CursorEstablisher::waitForResponse() noexcept {
    auto response = _ars->next();
    try {
        // Note the shardHostAndPort may not be populated if there was an error, so be sure
        // to do this after parsing the cursor response to ensure the response was ok.
        // Additionally, be careful not to push into 'remoteCursors' until we are sure we
        // have a valid cursor, since the error handling path will attempt to clean up
        // anything in 'remoteCursors'
        auto responseData = uassertStatusOK(std::move(response.swResponse)).data;
        auto cursorResponse = CursorResponse::parseFromBSONThrowing(std::move(responseData));

        RemoteCursor cursor;
        cursor.setCursorResponse(std::move(cursorResponse));
        cursor.setShardId(std::move(response.shardId));
        cursor.setHostAndPort(*response.shardHostAndPort);
        _remoteCursors.push_back(std::move(cursor));
    } catch (const DBException& ex) {
        _handleFailure(response, ex.toStatus());
    }
}

void CursorEstablisher::checkForFailedRequests() {
    if (!_maybeFailure) {
        // If we saw no failures, there is nothing to do.
        return;
    }

    LOG(0) << "Unable to establish remote cursors - {"
           << "error: " << *_maybeFailure << ", "
           << "numActiveRemotes: " << _remoteCursors.size() << "}";

    // Schedule killCursors against all cursors that were established.
    for (const auto& remoteCursor : _remoteCursors) {
        BSONObj cmdObj =
            KillCursorsRequest(_nss, {remoteCursor.getCursorResponse().getCursorId()}).toBSON();
        executor::RemoteCommandRequest request(
            remoteCursor.getHostAndPort(), _nss.db().toString(), cmdObj, _opCtx);

        // We do not process the response to the killCursors request (we make a good-faith
        // attempt at cleaning up the cursors, but ignore any returned errors).
        auto swHandle = _executor->scheduleRemoteCommand(
            request, [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {});
        if (!swHandle.isOK()) {
            LOG(3) << "Unable to cancel remote cursor - " << swHandle.getStatus();
        }
    }


    // Throw our failure.
    uassertStatusOK(*_maybeFailure);
}

void CursorEstablisher::_handleFailure(const AsyncRequestsSender::Response& response,
                                       Status status) noexcept {
    LOG(3) << "Experienced a failure while establishing cursors - " << status;
    if (_maybeFailure) {
        // If we've already failed, just log and move on.
        return;
    }

    // Retriable errors are swallowed if '_allowPartialResults' is true. Targeting shard replica
    // sets can also throw FailedToSatisfyReadPreference, so we swallow it too.
    bool isEligibleException = (ErrorCodes::isRetriableError(status.code()) ||
                                status.code() == ErrorCodes::FailedToSatisfyReadPreference);
    if (_allowPartialResults && isEligibleException) {
        // This exception is eligible to be swallowed.
        return;
    }

    // Do not schedule any new requests.
    _ars->stopRetrying();
    _maybeFailure = std::move(status);
}

}  // namespace

std::vector<RemoteCursor> establishCursors(OperationContext* opCtx,
                                           executor::TaskExecutor* executor,
                                           const NamespaceString& nss,
                                           const ReadPreferenceSetting readPref,
                                           const std::vector<std::pair<ShardId, BSONObj>>& remotes,
                                           bool allowPartialResults,
                                           Shard::RetryPolicy retryPolicy) {
    auto establisher = CursorEstablisher(opCtx, executor, nss, allowPartialResults);
    establisher.sendRequests(readPref, remotes, retryPolicy);
    establisher.waitForResponses();
    establisher.checkForFailedRequests();
    return establisher.takeCursors();
}

}  // namespace mongo
