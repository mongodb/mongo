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


// IWYU pragma: no_include "cxxabi.h"
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <string>
#include <type_traits>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/s/transaction_coordinator_futures_util.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_state.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction


namespace mongo {
namespace txn {
namespace {

MONGO_FAIL_POINT_DEFINE(failRemoteTransactionCommand);
MONGO_FAIL_POINT_DEFINE(hangWhileTargetingRemoteHost);
MONGO_FAIL_POINT_DEFINE(hangWhileTargetingLocalHost);

using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using ResponseStatus = executor::TaskExecutor::ResponseStatus;

}  // namespace

AsyncWorkScheduler::AsyncWorkScheduler(ServiceContext* serviceContext)
    : _serviceContext(serviceContext),
      _executor(Grid::get(_serviceContext)->getExecutorPool()->getFixedExecutor()) {}

AsyncWorkScheduler::~AsyncWorkScheduler() {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        invariant(_quiesced(lg));
    }

    if (!_parent)
        return;

    stdx::lock_guard<Latch> lg(_parent->_mutex);
    _parent->_childSchedulers.erase(_itToRemove);
    _parent->_notifyAllTasksComplete(lg);
    _parent = nullptr;
}

Future<executor::TaskExecutor::ResponseStatus> AsyncWorkScheduler::scheduleRemoteCommand(
    const ShardId& shardId,
    const ReadPreferenceSetting& readPref,
    const BSONObj& commandObj,
    OperationContextFn operationContextFn) {

    const bool isSelfShard = (shardId == getLocalShardId(_serviceContext));

    int failPointErrorCode = 0;
    if (MONGO_unlikely(failRemoteTransactionCommand.shouldFail([&](const BSONObj& data) -> bool {
            invariant(data.hasField("code"));
            invariant(data.hasField("command"));
            failPointErrorCode = data.getIntField("code");
            if (commandObj.hasField(data.getStringField("command"))) {
                LOGV2_DEBUG(5141702,
                            1,
                            "Fail point matched the command and will inject failure",
                            "shardId"_attr = shardId,
                            "failData"_attr = data);
                return true;
            }
            return false;
        }))) {
        return ResponseStatus{BSON("code" << failPointErrorCode << "ok" << false << "errmsg"
                                          << "fail point"),
                              Milliseconds(1)};
    }

    if (isSelfShard) {
        // If sending a command to the same shard as this node is in, send it directly to this node
        // rather than going through the host targeting below. This ensures that the state changes
        // for the participant and coordinator occur sequentially on a single branch of replica set
        // history. See SERVER-38142 for details.
        return scheduleWork([this, shardId, operationContextFn, commandObj = commandObj.getOwned()](
                                OperationContext* opCtx) {
            operationContextFn(opCtx);

            // Note: This internal authorization is tied to the lifetime of the client, which will
            // be destroyed by 'scheduleWork' immediately after this lambda ends
            AuthorizationSession::get(opCtx->getClient())
                ->grantInternalAuthorization(opCtx->getClient());

            if (MONGO_unlikely(hangWhileTargetingLocalHost.shouldFail())) {
                LOGV2(22449, "Hit hangWhileTargetingLocalHost failpoint");
                hangWhileTargetingLocalHost.pauseWhileSet(opCtx);
            }

            const auto service = opCtx->getServiceContext();
            auto start = _executor->now();

            auto requestOpMsg =
                OpMsgRequestBuilder::create(
                    auth::ValidatedTenancyScope::kNotRequired, DatabaseName::kAdmin, commandObj)
                    .serialize();
            const auto replyOpMsg = OpMsg::parseOwned(service->getService(ClusterRole::ShardServer)
                                                          ->getServiceEntryPoint()
                                                          ->handleRequest(opCtx, requestOpMsg)
                                                          .get()
                                                          .response);

            // Document sequences are not yet being used for responses.
            invariant(replyOpMsg.sequences.empty());

            // 'ResponseStatus' is the response format of a remote request sent over the network
            // so we simulate that format manually here, since we sent the request over the
            // loopback.
            return ResponseStatus{replyOpMsg.body.getOwned(), _executor->now() - start};
        });
    }

    return _targetHostAsync(shardId, readPref, operationContextFn)
        .then([this, shardId, commandObj = commandObj.getOwned(), readPref](
                  HostAndShard hostAndShard) mutable {
            executor::RemoteCommandRequest request(hostAndShard.hostTargeted,
                                                   DatabaseName::kAdmin,
                                                   commandObj,
                                                   readPref.toContainingBSON(),
                                                   nullptr);

            auto pf = makePromiseFuture<ResponseStatus>();

            stdx::unique_lock<Latch> ul(_mutex);
            uassertStatusOK(_shutdownStatus);

            auto scheduledCommandHandle = uassertStatusOK(_executor->scheduleRemoteCommand(
                request,
                [this,
                 commandObj = std::move(commandObj),
                 shardId = shardId,
                 hostTargeted = std::move(hostAndShard.hostTargeted),
                 shard = std::move(hostAndShard.shard),
                 promise = std::make_shared<Promise<ResponseStatus>>(std::move(pf.promise))](
                    const RemoteCommandCallbackArgs& args) mutable noexcept {
                    auto status = args.response.status;
                    shard->updateReplSetMonitor(hostTargeted, status);

                    // Only consider actual failures to send the command as errors.
                    if (status.isOK()) {
                        auto commandStatus = getStatusFromCommandResult(args.response.data);
                        shard->updateReplSetMonitor(hostTargeted, commandStatus);

                        auto writeConcernStatus =
                            getWriteConcernStatusFromCommandResult(args.response.data);
                        shard->updateReplSetMonitor(hostTargeted, writeConcernStatus);

                        promise->emplaceValue(args.response);
                    } else {
                        promise->setError([&] {
                            if (status == ErrorCodes::CallbackCanceled) {
                                stdx::unique_lock<Latch> ul(_mutex);
                                return _shutdownStatus.isOK() ? status : _shutdownStatus;
                            }
                            return status;
                        }());
                    }
                }));

            auto it =
                _activeHandles.emplace(_activeHandles.begin(), std::move(scheduledCommandHandle));

            ul.unlock();

            return std::move(pf.future).tapAll(
                [this, it = std::move(it)](StatusWith<ResponseStatus> s) {
                    stdx::lock_guard<Latch> lg(_mutex);
                    _activeHandles.erase(it);
                    _notifyAllTasksComplete(lg);
                });
        });
}

std::unique_ptr<AsyncWorkScheduler> AsyncWorkScheduler::makeChildScheduler() {
    auto child = std::make_unique<AsyncWorkScheduler>(_serviceContext);

    stdx::lock_guard<Latch> lg(_mutex);
    if (!_shutdownStatus.isOK())
        child->shutdown(_shutdownStatus);

    child->_parent = this;
    child->_itToRemove = _childSchedulers.emplace(_childSchedulers.begin(), child.get());

    return child;
}

void AsyncWorkScheduler::shutdown(Status status) {
    invariant(!status.isOK());

    stdx::lock_guard<Latch> lg(_mutex);
    if (!_shutdownStatus.isOK())
        return;

    _shutdownStatus = std::move(status);

    for (const auto& it : _activeOpContexts) {
        stdx::lock_guard<Client> clientLock(*it->getClient());
        _serviceContext->killOperation(clientLock, it.get(), _shutdownStatus.code());
    }

    for (const auto& it : _activeHandles) {
        _executor->cancel(it);
    }

    for (auto& child : _childSchedulers) {
        child->shutdown(_shutdownStatus);
    }
}

void AsyncWorkScheduler::join() {
    stdx::unique_lock<Latch> ul(_mutex);
    _allListsEmptyCV.wait(ul, [&] {
        return _activeOpContexts.empty() && _activeHandles.empty() && _childSchedulers.empty();
    });
}

Future<AsyncWorkScheduler::HostAndShard> AsyncWorkScheduler::_targetHostAsync(
    const ShardId& shardId,
    const ReadPreferenceSetting& readPref,
    OperationContextFn operationContextFn) {
    return scheduleWork([this, shardId, readPref, operationContextFn](OperationContext* opCtx) {
        operationContextFn(opCtx);
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));

        if (MONGO_unlikely(hangWhileTargetingRemoteHost.shouldFail())) {
            LOGV2(22450, "Hit hangWhileTargetingRemoteHost failpoint", "shardId"_attr = shardId);
            hangWhileTargetingRemoteHost.pauseWhileSet(opCtx);
        }

        auto targeter = shard->getTargeter();
        return targeter->findHost(readPref, CancellationToken::uncancelable())
            .thenRunOn(_executor)
            .unsafeToInlineFuture()
            .then([shard = std::move(shard)](HostAndPort host) mutable -> HostAndShard {
                return {std::move(host), std::move(shard)};
            });
    });
}

bool AsyncWorkScheduler::_quiesced(WithLock) const {
    return _activeOpContexts.empty() && _activeHandles.empty() && _childSchedulers.empty();
}

void AsyncWorkScheduler::_notifyAllTasksComplete(WithLock wl) {
    if (_quiesced(wl))
        _allListsEmptyCV.notify_all();
}

ShardId getLocalShardId(ServiceContext* service) {
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        return ShardId::kConfigServerId;
    }
    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        return ShardingState::get(service)->shardId();
    }

    // Only sharded systems should use the two-phase commit path
    MONGO_UNREACHABLE;
}

Future<void> whenAll(std::vector<Future<void>>& futures) {
    std::vector<Future<int>> dummyFutures;
    dummyFutures.reserve(futures.size());
    for (auto&& f : futures) {
        dummyFutures.push_back(std::move(f).then([]() { return 0; }));
    }
    return collect(
               std::move(dummyFutures), 0, [](int, const int&) { return ShouldStopIteration::kNo; })
        .ignoreValue();
}

}  // namespace txn
}  // namespace mongo
