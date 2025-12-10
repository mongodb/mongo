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

#include "mongo/s/query/exec/establish_cursors.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/client.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/async_multicaster.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <set>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

MONGO_FAIL_POINT_DEFINE(throwDuringCursorResponseValidation);

namespace {

constexpr StringData kOperationKeyField = "clientOperationKey"_sd;

/**
 * This class wraps logic for establishing cursors using a MultiStatementTransactionRequestsSender.
 */
class CursorEstablisher {
public:
    CursorEstablisher(OperationContext* opCtx,
                      RoutingContext* routingCtx,
                      std::shared_ptr<executor::TaskExecutor> executor,
                      const NamespaceString& nss,
                      bool allowPartialResults,
                      std::vector<OperationKey> providedOpKeys,
                      const AsyncRequestsSender::ShardHostMap& designatedHostsMap)
        : _opCtx(opCtx),
          _routingCtx(routingCtx),
          _executor{std::move(executor)},
          _nss(nss),
          _defaultOpKey{UUID::gen()},
          _providedOpKeys(std::move(providedOpKeys)),
          _allowPartialResults(allowPartialResults),
          _designatedHostsMap(designatedHostsMap) {}

    /**
     * Make a RequestSender and thus send requests.
     */
    void sendRequests(const ReadPreferenceSetting& readPref,
                      const std::vector<AsyncRequestsSender::Request>& remotes,
                      Shard::RetryPolicy retryPolicy);

    /**
     * Wait for all responses via the RequestSender.
     */
    void waitForResponses() {
        while (!_ars->done()) {
            _waitForResponse();
        }
    }

    /**
     * If any request received a non-retriable error response and partial results are not allowed,
     * cancel any requests that may have succeeded and throw the first such error encountered.
     */
    void checkForFailedRequests();

    /**
     * Take all cursors currently tracked by the CursorEstablisher.
     */
    std::vector<RemoteCursor> takeCursors() {
        return std::exchange(_remoteCursors, {});
    }

    static void killOpOnShards(ServiceContext* srvCtx,
                               std::shared_ptr<executor::TaskExecutor> executor,
                               std::vector<OperationKey> opKeys,
                               std::set<HostAndPort> remotes) noexcept;

private:
    /**
     * Wait for a single response via the RequestSender.
     */
    void _waitForResponse();

    void _handleFailure(const boost::optional<AsyncRequestsSender::Response>& response,
                        Status status,
                        bool isInterruption = false);
    bool _canSkipForPartialResults(const AsyncRequestsSender::Response& response,
                                   const Status& status);

    /**
     * Favors interruption/unyield failures > UUID mismatch error with actual ns > UUID mismatch
     * error > other errors > retargeting errors
     */
    void _prioritizeFailures(Status newError, bool isInterruption);

    OperationContext* const _opCtx;
    RoutingContext* _routingCtx;
    const std::shared_ptr<executor::TaskExecutor> _executor;
    const NamespaceString _nss;

    // Callers may provide an array of OperationKeys already included in the given requests and
    // those will be used to clean up cursors on failure, otherwise one key will be generated and
    // appended to all requests.
    const OperationKey _defaultOpKey;
    const std::vector<OperationKey> _providedOpKeys;

    const bool _allowPartialResults;

    bool _wasInterrupted = false;

    boost::optional<MultiStatementTransactionRequestsSender> _ars;

    boost::optional<Status> _maybeFailure;
    std::vector<RemoteCursor> _remoteCursors;
    std::vector<HostAndPort> _remotesToClean;

    // The hosts map is only stored temporarily before the '_ars' member is instantiated. It will be
    // empty after the '_ars' member instantiation.
    AsyncRequestsSender::ShardHostMap _designatedHostsMap;
    static logv2::SeveritySuppressor _logSeveritySuppressor;
};

logv2::SeveritySuppressor CursorEstablisher::_logSeveritySuppressor{
    Seconds{1}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(2)};

void CursorEstablisher::sendRequests(const ReadPreferenceSetting& readPref,
                                     const std::vector<AsyncRequestsSender::Request>& remotes,
                                     Shard::RetryPolicy retryPolicy) {
    // Construct the requests
    std::vector<AsyncRequestsSender::Request> requests;
    requests.reserve(remotes.size());

    // Make sure there is enough room in '_remotesToClean' so that inserting a cursor into the clean
    // up list later cannot throw an OOM exception.
    _remotesToClean.reserve(remotes.size());

    // TODO SERVER-47261 management of the opKey should move to the ARS.
    for (const auto& remote : remotes) {
        if (_providedOpKeys.size()) {
            // Caller provided their own keys so skip appending the default key.
            dassert(remote.cmdObj.hasField(kOperationKeyField));
            requests.emplace_back(remote);
        } else {
            BSONObjBuilder newCmd(remote.cmdObj);
            appendOpKey(_defaultOpKey, &newCmd);
            requests.emplace_back(remote.shardId, newCmd.obj());
        }
    }

    tassert(9282602,
            "Invalid number of objects in CursorEstablisher",
            requests.size() == remotes.size());
    tassert(9282603,
            "Invalid _remotesToClean capacity in CursorEstablisher",
            _remotesToClean.capacity() == remotes.size());

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
    _ars.emplace(_opCtx,
                 _executor,
                 _nss.dbName(),
                 std::move(requests),
                 readPref,
                 retryPolicy,
                 std::move(_designatedHostsMap));

    if (_routingCtx && _routingCtx->hasNss(_nss)) {
        _routingCtx->onRequestSentForNss(_nss);
    }
}

void CursorEstablisher::_waitForResponse() {
    boost::optional<AsyncRequestsSender::Response> maybeResponse;
    BSONObj responseData;
    try {
        // Fetch the next response, without validating it yet.
        maybeResponse = _ars->nextResponse();
        uassert(
            9282600, "Response in CursorEstablisher must have a value", maybeResponse.has_value());
        if (maybeResponse->shardHostAndPort) {
            // Add cursor to our cleanup list. This should not throw, as we reserved enough capacity
            // ahead of time in 'sendRequests()'.
            _remotesToClean.push_back(*maybeResponse->shardHostAndPort);
        }

        // Intentionally throw a 'FailedToParse' exception here in case the fail point is active.
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Hit failpoint '" << throwDuringCursorResponseValidation.getName()
                              << "'",
                !throwDuringCursorResponseValidation.shouldFail());

        // Validate the response. If this throws, we still have the cursor in the cleanup list.
        _ars->validateResponse(*maybeResponse, false /* forMergeCursors */);
        responseData = uassertStatusOK(std::move(maybeResponse->swResponse)).data;
    } catch (const DBException& ex) {
        _handleFailure(maybeResponse, ex.toStatus(), /* isInterruption */ true);
        return;
    }
    auto response = *maybeResponse;
    try {
        auto cursors = CursorResponse::parseFromBSONMany(std::move(responseData));

        bool hasCursorToClean = false;
        for (auto& cursor : cursors) {
            if (!cursor.isOK()) {
                _handleFailure(response, cursor.getStatus());
                continue;
            }

            auto& cursorValue = cursor.getValue();

            // Only persisted remote cursors need cleanup. If the returned cursor has a "cursorId"
            // value of 0, it means that it is not a persisted cursor and does not need cleanup.
            hasCursorToClean |= cursorValue.getCursorId() != 0;

            if (const auto& cursorMetrics = cursorValue.getCursorMetrics()) {
                CurOp::get(_opCtx)->debug().additiveMetrics.aggregateCursorMetrics(*cursorMetrics);
            }

            // If we have already received an error back and are going to abort the operation
            // anyway, avoid storing the cursor response to reduce memory usage.
            if (!_maybeFailure) {
                _remoteCursors.emplace_back(response.shardId.toString(),
                                            *response.shardHostAndPort,
                                            std::move(cursorValue));
            }
        }

        if (response.shardHostAndPort && !hasCursorToClean) {
            // If we never got a valid cursor, we do not need to clean the host.
            _remotesToClean.pop_back();
        }
    } catch (const DBException& ex) {
        _handleFailure(response, ex.toStatus());
    }

    // In case any error occurred, the already-received cursor responses should have been cleared by
    // '_handleFailure()' to reduce memory usage.
    tassert(11401200,
            "expecting _remoteCursors responses to be empty in case of failure",
            !_maybeFailure || _remoteCursors.empty());
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

    if (_maybeFailure->code() != ErrorCodes::CommandOnShardedViewNotSupportedOnMongod) {
        // LOGV2_DEBUG sets the log severity level based on the value defined by
        // _logSeveritySuppressor().toInt(). This severity level is not restricted to DEBUG and can
        // be any defined level.
        LOGV2_DEBUG(4625501,
                    _logSeveritySuppressor().toInt(),
                    "Unable to establish remote cursors",
                    "error"_attr = *_maybeFailure,
                    "nRemotes"_attr = _remotesToClean.size());
    } else {
        // If this is a shard acting as a sub-router, clear the pending participant.
        auto txnRouter = TransactionRouter::get(_opCtx);
        if (txnRouter && _opCtx->isActiveTransactionParticipant()) {
            txnRouter.onViewResolutionError(_opCtx, _nss);
        }
    }

    if (!_remotesToClean.empty()) {
        // Filter out duplicate hosts.
        auto remotes = std::set<HostAndPort>(_remotesToClean.begin(), _remotesToClean.end());

        uassertStatusOK(scheduleCursorCleanup(
            _executor,
            _opCtx->getServiceContext(),
            _providedOpKeys.size() ? _providedOpKeys : std::vector<OperationKey>{_defaultOpKey},
            std::move(remotes)));
    }

    // Throw our failure.
    uassertStatusOK(*_maybeFailure);
}

void CursorEstablisher::_prioritizeFailures(Status newError, bool isInterruption) {
    tassert(11052339, "Expected error", !newError.isOK());
    tassert(11052340, "Expected failure", !_maybeFailure->isOK());

    // Prefer interruptions above all else.
    if (_wasInterrupted) {
        return;
    }
    if (isInterruption) {
        _wasInterrupted = true;
        _maybeFailure = newError;
        return;
    }

    // Prefer other non-retargeting related errors that could be operation-fatal.
    if (_maybeFailure->isA<ErrorCategory::StaleShardVersionError>() ||
        _maybeFailure->code() == ErrorCodes::StaleDbVersion ||
        _maybeFailure->code() == ErrorCodes::CommandOnShardedViewNotSupportedOnMongod) {
        _maybeFailure = std::move(newError);
        return;
    }

    if (newError.code() != ErrorCodes::CollectionUUIDMismatch) {
        return;
    }

    if (_maybeFailure->code() != ErrorCodes::CollectionUUIDMismatch) {
        _maybeFailure = std::move(newError);
        return;
    }

    // Favor 'CollectionUUIDMismatchError' that has a non empty 'actualNamespace'.
    auto errorInfo = _maybeFailure->extraInfo<CollectionUUIDMismatchInfo>();
    tassert(11052341, "Expected extraInfo of type CollectionUUIDMismatchInfo", errorInfo);
    if (!errorInfo->actualCollection()) {
        _maybeFailure = std::move(newError);
    }
}

void CursorEstablisher::_handleFailure(
    const boost::optional<AsyncRequestsSender::Response>& response,
    Status status,
    bool isInterruption) {
    LOGV2_DEBUG(
        8846900, 3, "Experienced a failure while establishing cursors", "error"_attr = status);
    if (_wasInterrupted) {
        return;
    }
    if (_maybeFailure) {
        _prioritizeFailures(std::move(status), isInterruption);
        return;
    }
    if (response && _canSkipForPartialResults(*response, status)) {
        return;
    }
    if (isInterruption) {
        _wasInterrupted = true;
    }

    // Release memory for received responses as early as possible.
    decltype(_remoteCursors)().swap(_remoteCursors);

    _maybeFailure = status;
    _ars->stopRetrying();
}

bool CursorEstablisher::_canSkipForPartialResults(const AsyncRequestsSender::Response& response,
                                                  const Status& status) {
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
                                   boost::none,
                                   boost::none,
                                   true}});
        return true;
    }
    return false;
}

void CursorEstablisher::killOpOnShards(ServiceContext* srvCtx,
                                       std::shared_ptr<executor::TaskExecutor> executor,
                                       std::vector<OperationKey> opKeys,
                                       std::set<HostAndPort> remotes) noexcept try {
    ThreadClient tc("establishCursors cleanup", srvCtx->getService());
    auto opCtx = tc->makeOperationContext();

    for (auto&& host : remotes) {
        BSONArrayBuilder opKeyArrayBuilder;
        for (auto&& opKey : opKeys) {
            opKey.appendToArrayBuilder(&opKeyArrayBuilder);
        }

        executor::RemoteCommandRequest request(
            host,
            DatabaseName::kAdmin,
            BSON("_killOperations" << 1 << "operationKeys" << opKeyArrayBuilder.arr()),
            opCtx.get(),
            executor::RemoteCommandRequest::kNoTimeout,
            true /* fireAndForget */);

        // We do not process the response to the killOperations request (we make a good-faith
        // attempt at cleaning up the cursors, but ignore any returned errors).
        uassertStatusOK(executor->scheduleRemoteCommand(request, [host](auto const& args) {
            if (args.response.isOK()) {
                LOGV2_DEBUG(
                    8928400, 2, "killOperations succeeded", "remoteHost"_attr = host.toString());
            } else {
                LOGV2_DEBUG(4625504,
                            2,
                            "killOperations failed",
                            "remoteHost"_attr = host.toString(),
                            "error"_attr = args.response);
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
    cmdWithReadPrefBob.append("$readPreference", BSON("mode" << "nearest"));
    return cmdWithReadPrefBob.obj();
}

}  // namespace

// Attach our OperationKey to a request. This will allow us to kill any outstanding
// requests in case we're interrupted or one of the remotes returns an error. Note that although
// the opCtx may have an OperationKey set on it already, do not inherit it here because we may
// target ourselves which implies the same node receiving multiple operations with the same
// opKey.
void appendOpKey(const OperationKey& opKey, BSONObjBuilder* cmdBuilder) {
    opKey.appendToBuilder(cmdBuilder, kOperationKeyField);
}

std::vector<RemoteCursor> establishCursors(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    const NamespaceString& nss,
    const ReadPreferenceSetting readPref,
    const std::vector<AsyncRequestsSender::Request>& remotes,
    bool allowPartialResults,
    RoutingContext* routingCtx,
    Shard::RetryPolicy retryPolicy,
    std::vector<OperationKey> providedOpKeys,
    const AsyncRequestsSender::ShardHostMap& designatedHostsMap) {
    auto establisher = CursorEstablisher(opCtx,
                                         routingCtx,
                                         executor,
                                         nss,
                                         allowPartialResults,
                                         std::move(providedOpKeys),
                                         designatedHostsMap);
    establisher.sendRequests(readPref, remotes, retryPolicy);
    establisher.waitForResponses();
    establisher.checkForFailedRequests();
    return establisher.takeCursors();
}

void killRemoteCursor(OperationContext* opCtx,
                      executor::TaskExecutor* executor,
                      RemoteCursor&& cursor,
                      const NamespaceString& nss) {
    const auto& host = cursor.getHostAndPort();
    BSONObj cmdObj =
        KillCursorsCommandRequest(nss, {cursor.getCursorResponse().getCursorId()}).toBSON();
    executor::RemoteCommandRequest request(host, nss.dbName(), cmdObj, opCtx);

    // We do not process the response to the killCursors request (we make a good-faith
    // attempt at cleaning up the cursors, but ignore any returned errors).
    executor
        ->scheduleRemoteCommand(request,
                                [host](auto const& args) {
                                    if (args.response.isOK()) {
                                        LOGV2_DEBUG(8928415,
                                                    2,
                                                    "killCursors succeeded",
                                                    "remoteHost"_attr = host.toString());
                                    } else {
                                        LOGV2_DEBUG(8928414,
                                                    2,
                                                    "killCursors failed",
                                                    "remoteHost"_attr = host.toString(),
                                                    "error"_attr = args.response);
                                    }
                                })
        .getStatus()
        .ignore();
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
    RoutingContext& routingCtx,
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
    BSONObjBuilder newCmd(std::move(cmdObj));
    appendOpKey(opKey, &newCmd);
    newCmd.append("$readPreference", BSON("mode" << "nearest"));

    executor::AsyncMulticaster::Options options;
    options.maxConcurrency = internalQueryAggMulticastMaxConcurrency.loadRelaxed();
    auto results = executor::AsyncMulticaster(executor, options)
                       .multicast(servers,
                                  nss.dbName(),
                                  newCmd.obj(),
                                  opCtx,
                                  Milliseconds(internalQueryAggMulticastTimeoutMS.loadRelaxed()));

    if (routingCtx.hasNss(nss)) {
        routingCtx.onRequestSentForNss(nss);
    }

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

                auto& cursorValue = cursor.getValue();
                if (const auto& cursorMetrics = cursorValue.getCursorMetrics()) {
                    CurOp::get(opCtx)->debug().additiveMetrics.aggregateCursorMetrics(
                        *cursorMetrics);
                }

                remoteCursors.emplace_back(
                    RemoteCursor(shardId.toString(), hostAndPort, std::move(cursorValue)));
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
