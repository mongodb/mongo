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

#include "mongo/s/query/establish_cursors.h"

#include <set>

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/kill_cursors_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/executor/async_multicaster.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {

constexpr StringData kOperationKeyField = "clientOperationKey"_sd;

/**
 * This class wraps logic for establishing cursors using a MultiStatementTransactionRequestsSender.
 */
class CursorEstablisher {
public:
    CursorEstablisher(OperationContext* opCtx,
                      std::shared_ptr<executor::TaskExecutor> executor,
                      const NamespaceString& nss,
                      bool allowPartialResults,
                      std::vector<OperationKey> providedOpKeys)
        : _opCtx(opCtx),
          _executor{std::move(executor)},
          _nss(nss),
          _allowPartialResults(allowPartialResults),
          _defaultOpKey{UUID::gen()},
          _providedOpKeys(std::move(providedOpKeys)) {}

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

    static void killOpOnShards(ServiceContext* srvCtx,
                               std::shared_ptr<executor::TaskExecutor> executor,
                               std::vector<OperationKey> opKeys,
                               std::set<HostAndPort> remotes) noexcept;

private:
    void _handleFailure(const AsyncRequestsSender::Response& response, Status status) noexcept;

    /**
     * Favors the status with 'CollectionUUIDMismatch' error to be saved in '_maybeFailure' to be
     * returned to caller.
     */
    void _favorCollectionUUIDMismatchError(Status newError) noexcept;

    OperationContext* const _opCtx;
    const std::shared_ptr<executor::TaskExecutor> _executor;
    const NamespaceString _nss;
    const bool _allowPartialResults;

    // Callers may provide an array of OperationKeys already included in the given requests and
    // those will be used to clean up cursors on failure, otherwise one key will be generated and
    // appended to all requests.
    const OperationKey _defaultOpKey;
    const std::vector<OperationKey> _providedOpKeys;

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

    // TODO SERVER-47261 management of the opKey should move to the ARS.
    for (const auto& remote : remotes) {
        if (_providedOpKeys.size()) {
            // Caller provided their own keys so skip appending the default key.
            dassert(remote.second.hasField(kOperationKeyField));
            requests.emplace_back(remote.first, remote.second);
        } else {
            requests.emplace_back(remote.first, appendOpKey(_defaultOpKey, remote.second));
        }
    }

    if (shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, logv2::LogSeverity::Debug(3))) {
        logv2::DynamicAttributes attrs;
        attrs.add("opId", _opCtx->getOpID());
        attrs.add("numRemotes", remotes.size());
        if (_providedOpKeys.size()) {
            BSONArrayBuilder bab;
            for (auto&& opKey : _providedOpKeys) {
                opKey.appendToArrayBuilder(&bab);
            }
            attrs.add("providedOpKeys", bab.arr());
        } else {
            attrs.add("defaultOpKey", _defaultOpKey);
        }
        LOGV2_DEBUG(4625502, 3, "Establishing cursors on remotes", attrs);
    }

    // Send the requests
    _ars.emplace(_opCtx, _executor, _nss.dbName(), std::move(requests), readPref, retryPolicy);
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

            _remoteCursors.emplace_back(RemoteCursor(response.shardId.toString(),
                                                     *response.shardHostAndPort,
                                                     std::move(cursor.getValue())));
        }

        if (response.shardHostAndPort && !hadValidCursor) {
            // If we never got a valid cursor, we do not need to clean the host.
            _remotesToClean.pop_back();
        }

    } catch (const DBException& ex) {
        _handleFailure(response, ex.toStatus());
    }
}

// Schedule killOperations against all cursors that were established. Make sure to
// capture arguments by value since the cleanup work may get scheduled after
// returning from this function.
StatusWith<executor::TaskExecutor::CallbackHandle> scheduleCursorCleanup(
    std::shared_ptr<executor::TaskExecutor> executor,
    ServiceContext* svcCtx,
    std::vector<OperationKey> opKeys,
    std::set<HostAndPort>&& remotesToClean) {
    return executor->scheduleWork([svcCtx = svcCtx,
                                   executor = executor,
                                   opKeys = std::move(opKeys),
                                   remotesToClean = std::move(remotesToClean)](
                                      const executor::TaskExecutor::CallbackArgs& args) mutable {
        if (!args.status.isOK()) {
            LOGV2_WARNING(
                7355702, "Failed to schedule remote cursor cleanup", "error"_attr = args.status);
            return;
        }
        CursorEstablisher::killOpOnShards(
            svcCtx, std::move(executor), std::move(opKeys), std::move(remotesToClean));
    });
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

    uassertStatusOK(scheduleCursorCleanup(
        _executor,
        _opCtx->getServiceContext(),
        _providedOpKeys.size() ? _providedOpKeys : std::vector<OperationKey>{_defaultOpKey},
        std::move(remotes)));

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

    // If '_allowPartialResults' is true then swallow retriable errors, maxTimeMSExpired, and
    // FailedToSatisfyReadPreference errors we might get when targeting shard replica sets.
    bool isEligibleException = (isMongosRetriableError(status.code()) ||
                                status.code() == ErrorCodes::FailedToSatisfyReadPreference ||
                                status.code() == ErrorCodes::MaxTimeMSExpired);
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

void CursorEstablisher::killOpOnShards(ServiceContext* srvCtx,
                                       std::shared_ptr<executor::TaskExecutor> executor,
                                       std::vector<OperationKey> opKeys,
                                       std::set<HostAndPort> remotes) noexcept try {
    ThreadClient tc("establishCursors cleanup", srvCtx);
    auto opCtx = tc->makeOperationContext();

    for (auto&& host : remotes) {
        BSONArrayBuilder opKeyArrayBuilder;
        for (auto&& opKey : opKeys) {
            opKey.appendToArrayBuilder(&opKeyArrayBuilder);
        }

        executor::RemoteCommandRequest::Options options;
        options.fireAndForget = true;
        executor::RemoteCommandRequest request(
            host,
            "admin",
            BSON("_killOperations" << 1 << "operationKeys" << opKeyArrayBuilder.arr()),
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
/**
 * Returns a copy of 'cmdObj' with the $readPreference mode set to secondaryPreferred.
 */
BSONObj appendReadPreferenceNearest(BSONObj cmdObj) {
    BSONObjBuilder cmdWithReadPrefBob(std::move(cmdObj));
    cmdWithReadPrefBob.append("$readPreference",
                              BSON("mode"
                                   << "nearest"));
    return cmdWithReadPrefBob.obj();
}

}  // namespace

// Attach our OperationKey to a request. This will allow us to kill any outstanding
// requests in case we're interrupted or one of the remotes returns an error. Note that although
// the opCtx may have an OperationKey set on it already, do not inherit it here because we may
// target ourselves which implies the same node receiving multiple operations with the same
// opKey.
BSONObj appendOpKey(const OperationKey& opKey, const BSONObj& request) {
    BSONObjBuilder newCmd(request);
    opKey.appendToBuilder(&newCmd, kOperationKeyField);
    return newCmd.obj();
}

std::vector<RemoteCursor> establishCursors(OperationContext* opCtx,
                                           std::shared_ptr<executor::TaskExecutor> executor,
                                           const NamespaceString& nss,
                                           const ReadPreferenceSetting readPref,
                                           const std::vector<std::pair<ShardId, BSONObj>>& remotes,
                                           bool allowPartialResults,
                                           Shard::RetryPolicy retryPolicy,
                                           std::vector<OperationKey> providedOpKeys) {
    auto establisher =
        CursorEstablisher(opCtx, executor, nss, allowPartialResults, std::move(providedOpKeys));
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

std::pair<std::vector<HostAndPort>, StringMap<ShardId>> getHostInfos(
    OperationContext* opCtx, const std::set<ShardId>& shardIds) {
    std::vector<HostAndPort> servers;
    StringMap<ShardId> hostToShardId;

    // Get the host/port of every node in each shard.
    auto registry = Grid::get(opCtx)->shardRegistry();
    for (const auto& shardId : shardIds) {
        auto shard = uassertStatusOK(registry->getShard(opCtx, shardId));
        auto cs = shard->getConnString();
        auto shardServers = cs.getServers();
        for (auto& host : shardServers) {
            hostToShardId.emplace(host.toString(), shardId);
        }
        servers.insert(servers.end(), shardServers.begin(), shardServers.end());
    }
    return {std::move(servers), hostToShardId};
}

std::vector<RemoteCursor> establishCursorsOnAllHosts(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    const NamespaceString& nss,
    const std::set<ShardId>& shardIds,
    BSONObj cmdObj,
    bool allowPartialResults,
    Shard::RetryPolicy retryPolicy) {
    auto [servers, hostToShardId] = getHostInfos(opCtx, shardIds);
    OperationKey opKey = UUID::gen();

    // Operation key will allow us to kill any outstanding requests in case we're interrupted.
    // Secondaries will reject aggregation commands with a default read preference (primary). The
    // actual semantics of read preference don't make as much sense when broadcasting to all
    // shards, but we will set read preference to 'nearest' since it does not imply preference for
    // primary or secondary.
    BSONObj cmd = appendOpKey(opKey, appendReadPreferenceNearest(cmdObj));

    executor::AsyncMulticaster::Options options;
    options.maxConcurrency = internalQueryAggMulticastMaxConcurrency;
    auto results = executor::AsyncMulticaster(executor, options)
                       .multicast(servers,
                                  nss.db().toString(),
                                  cmd,
                                  opCtx,
                                  Milliseconds(internalQueryAggMulticastTimeoutMS));
    std::vector<RemoteCursor> remoteCursors;
    std::set<HostAndPort> remotesToClean;

    boost::optional<Status> failure;

    for (auto&& [hostAndPort, result] : results) {
        if (result.isOK()) {
            auto cursors = CursorResponse::parseFromBSONMany(result.data);
            bool hadValidCursor = false;

            auto it = hostToShardId.find(hostAndPort.toString());
            tassert(7355701, "Host must have shard ID.", it != hostToShardId.end());
            auto shardId = it->second;

            for (auto& cursor : cursors) {
                if (!cursor.isOK()) {
                    failure = cursor.getStatus();
                    continue;
                }
                hadValidCursor = true;

                remoteCursors.emplace_back(
                    RemoteCursor(shardId.toString(), hostAndPort, std::move(cursor.getValue())));
            }

            if (hadValidCursor) {
                remotesToClean.insert(hostAndPort);
            }
        } else {
            LOGV2_DEBUG(7355700,
                        3,
                        "Experienced a failure while establishing cursors",
                        "error"_attr = result.status);
            failure = result.status;
        }
    }
    if (failure.has_value() && !allowPartialResults) {
        LOGV2(7355705,
              "Unable to establish remote cursors",
              "error"_attr = *failure,
              "nRemotes"_attr = remoteCursors.size());

        if (!remotesToClean.empty()) {
            uassertStatusOK(scheduleCursorCleanup(
                executor, opCtx->getServiceContext(), {opKey}, std::move(remotesToClean)));
        }

        uassertStatusOK(failure.value());
    }
    return remoteCursors;
}

}  // namespace mongo
