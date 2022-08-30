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


#include "mongo/platform/basic.h"

#include "mongo/s/query/establish_cursors.h"

#include <set>

#include "mongo/client/connpool.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/kill_cursors_gen.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {

/**
 * This class wraps logic for establishing cursors using a MultiStatementTransactionRequestsSender.
 */
class CursorEstablisher {
public:
    CursorEstablisher(OperationContext* opCtx,
                      std::shared_ptr<executor::TaskExecutor> executor,
                      const NamespaceString& nss,
                      bool allowPartialResults)
        : _opCtx(opCtx),
          _executor{std::move(executor)},
          _nss(nss),
          _allowPartialResults(allowPartialResults),
          _opKey{UUID::gen()} {}

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
     * If any request received a non-retriable error response and partial results are not allowed,
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
    static void _killOpOnShards(ServiceContext* srvCtx,
                                std::shared_ptr<executor::TaskExecutor> executor,
                                OperationKey opKey,
                                std::set<HostAndPort> remotes) noexcept;

    /**
     * Favors the status with 'CollectionUUIDMismatch' error to be saved in '_maybeFailure' to be
     * returned to caller.
     */
    void _favorCollectionUUIDMismatchError(Status newError) noexcept;

    OperationContext* const _opCtx;
    const std::shared_ptr<executor::TaskExecutor> _executor;
    const NamespaceString _nss;
    const bool _allowPartialResults;

    const OperationKey _opKey;

    boost::optional<MultiStatementTransactionRequestsSender> _ars;

    boost::optional<Status> _maybeFailure;
    std::vector<RemoteCursor> _remoteCursors;
    std::vector<HostAndPort> _remotesToClean;
};

void CursorEstablisher::sendRequests(const ReadPreferenceSetting& readPref,
                                     const std::vector<std::pair<ShardId, BSONObj>>& remotes,
                                     Shard::RetryPolicy retryPolicy) {
    // Construct the requests
    std::vector<AsyncRequestsSender::Request> requests;

    // Attach our OperationKey to each remote request. This will allow us to kill any outstanding
    // requests in case we're interrupted or one of the remotes returns an error. Note that although
    // the opCtx may have an OperationKey set on it already, do not inherit it here because we may
    // target ourselves which implies the same node receiving multiple operations with the same
    // opKey.
    // TODO SERVER-47261 management of the opKey should move to the ARS.
    for (const auto& remote : remotes) {
        BSONObjBuilder requestWithOpKey(remote.second);
        _opKey.appendToBuilder(&requestWithOpKey, "clientOperationKey");
        requests.emplace_back(remote.first, requestWithOpKey.obj());
    }

    LOGV2_DEBUG(4625502,
                3,
                "Establishing cursors on remotes",
                "opId"_attr = _opCtx->getOpID(),
                "numRemotes"_attr = remotes.size(),
                "opKey"_attr = _opKey);

    // Send the requests
    _ars.emplace(
        _opCtx, _executor, _nss.db().toString(), std::move(requests), readPref, retryPolicy);
}

void CursorEstablisher::waitForResponse() noexcept {
    auto response = _ars->next();
    if (response.shardHostAndPort)
        _remotesToClean.push_back(*response.shardHostAndPort);

    try {
        // Note the shardHostAndPort may not be populated if there was an error, so be sure
        // to do this after parsing the cursor response to ensure the response was ok.
        // Additionally, be careful not to push into 'remoteCursors' until we are sure we
        // have a valid cursor, since the error handling path will attempt to clean up
        // anything in 'remoteCursors'
        auto responseData = uassertStatusOK(std::move(response.swResponse)).data;
        auto cursors = CursorResponse::parseFromBSONMany(std::move(responseData));

        bool hadValidCursor = false;
        for (auto& cursor : cursors) {
            if (!cursor.isOK()) {
                _handleFailure(response, cursor.getStatus());
                continue;
            }

            hadValidCursor = true;

            RemoteCursor remoteCursor;
            remoteCursor.setCursorResponse(std::move(cursor.getValue()));
            remoteCursor.setShardId(response.shardId);
            remoteCursor.setHostAndPort(*response.shardHostAndPort);
            _remoteCursors.emplace_back(std::move(remoteCursor));
        }

        if (response.shardHostAndPort && !hadValidCursor) {
            // If we never got a valid cursor, we do not need to clean the host.
            _remotesToClean.pop_back();
        }

    } catch (const DBException& ex) {
        _handleFailure(response, ex.toStatus());
    }
}

void CursorEstablisher::checkForFailedRequests() {
    if (!_maybeFailure) {
        // If we saw no failures, there is nothing to do.
        return;
    }

    LOGV2(4625501,
          "Unable to establish remote cursors",
          "error"_attr = *_maybeFailure,
          "nRemotes"_attr = _remotesToClean.size());

    if (_remotesToClean.empty()) {
        // If we don't have any remotes to clean, throw early.
        uassertStatusOK(*_maybeFailure);
    }

    // Filter out duplicate hosts.
    auto remotes = std::set<HostAndPort>(_remotesToClean.begin(), _remotesToClean.end());

    // Schedule killOperations against all cursors that were established. Make sure to
    // capture arguments by value since the cleanup work may get scheduled after
    // returning from this function.
    uassertStatusOK(_executor->scheduleWork(
        [svcCtx = _opCtx->getServiceContext(),
         executor = _executor,
         opKey = _opKey,
         remotes = std::move(remotes)](const executor::TaskExecutor::CallbackArgs& args) mutable {
            if (!args.status.isOK()) {
                LOGV2_WARNING(
                    48038, "Failed to schedule remote cursor cleanup", "error"_attr = args.status);
                return;
            }
            _killOpOnShards(svcCtx, std::move(executor), std::move(opKey), std::move(remotes));
        }));

    // Throw our failure.
    uassertStatusOK(*_maybeFailure);
}

void CursorEstablisher::_favorCollectionUUIDMismatchError(Status newError) noexcept {
    invariant(!newError.isOK());
    invariant(!_maybeFailure->isOK());

    if (newError.code() != ErrorCodes::CollectionUUIDMismatch) {
        return;
    }

    if (_maybeFailure->code() != ErrorCodes::CollectionUUIDMismatch) {
        _maybeFailure = std::move(newError);
        return;
    }

    // Favor 'CollectionUUIDMismatchError' that has a non empty 'actualNamespace'.
    auto errorInfo = _maybeFailure->extraInfo<CollectionUUIDMismatchInfo>();
    invariant(errorInfo);
    if (!errorInfo->actualCollection()) {
        _maybeFailure = std::move(newError);
    }
}

void CursorEstablisher::_handleFailure(const AsyncRequestsSender::Response& response,
                                       Status status) noexcept {
    LOGV2_DEBUG(
        4674000, 3, "Experienced a failure while establishing cursors", "error"_attr = status);
    if (_maybeFailure) {
        _favorCollectionUUIDMismatchError(std::move(status));
        return;
    }

    // Retriable errors are swallowed if '_allowPartialResults' is true. Targeting shard replica
    // sets can also throw FailedToSatisfyReadPreference, so we swallow it too.
    bool isEligibleException = (isMongosRetriableError(status.code()) ||
                                status.code() == ErrorCodes::FailedToSatisfyReadPreference);
    if (_allowPartialResults && isEligibleException) {
        // This exception is eligible to be swallowed. Add an entry with a cursorID of 0, an
        // empty HostAndPort, and which has the 'partialResultsReturned' flag set to true.
        _remoteCursors.push_back({response.shardId.toString(),
                                  {},
                                  {_nss,
                                   CursorId{0},
                                   {},
                                   boost::none,
                                   boost::none,
                                   boost::none,
                                   boost::none,
                                   boost::none,
                                   true}});
        return;
    }

    // Do not schedule any new requests.
    _ars->stopRetrying();
    _maybeFailure = std::move(status);
}

void CursorEstablisher::_killOpOnShards(ServiceContext* srvCtx,
                                        std::shared_ptr<executor::TaskExecutor> executor,
                                        OperationKey opKey,
                                        std::set<HostAndPort> remotes) noexcept try {
    ThreadClient tc("establishCursors cleanup", srvCtx);
    auto opCtx = tc->makeOperationContext();

    for (auto&& host : remotes) {
        executor::RemoteCommandRequest::Options options;
        options.fireAndForget = true;
        executor::RemoteCommandRequest request(
            host,
            "admin",
            BSON("_killOperations" << 1 << "operationKeys" << BSON_ARRAY(opKey)),
            opCtx.get(),
            executor::RemoteCommandRequestBase::kNoTimeout,
            options);

        // We do not process the response to the killOperations request (we make a good-faith
        // attempt at cleaning up the cursors, but ignore any returned errors).
        uassertStatusOK(executor->scheduleRemoteCommand(request, [host](auto const& args) {
            if (!args.response.isOK()) {
                LOGV2_DEBUG(4625504,
                            2,
                            "killOperations failed",
                            "remoteHost"_attr = host.toString(),
                            "error"_attr = args.response);
                return;
            }
        }));
    }
} catch (const AssertionException& ex) {
    LOGV2_DEBUG(4625503, 2, "Failed to cleanup remote operations", "error"_attr = ex.toStatus());
}

}  // namespace

std::vector<RemoteCursor> establishCursors(OperationContext* opCtx,
                                           std::shared_ptr<executor::TaskExecutor> executor,
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

void killRemoteCursor(OperationContext* opCtx,
                      executor::TaskExecutor* executor,
                      RemoteCursor&& cursor,
                      const NamespaceString& nss) {
    BSONObj cmdObj = KillCursorsCommandRequest(nss, {cursor.getCursorResponse().getCursorId()})
                         .toBSON(BSONObj{});
    executor::RemoteCommandRequest request(
        cursor.getHostAndPort(), nss.db().toString(), cmdObj, opCtx);

    // We do not process the response to the killCursors request (we make a good-faith
    // attempt at cleaning up the cursors, but ignore any returned errors).
    executor->scheduleRemoteCommand(request, [](auto const&) {}).getStatus().ignore();
}

}  // namespace mongo
