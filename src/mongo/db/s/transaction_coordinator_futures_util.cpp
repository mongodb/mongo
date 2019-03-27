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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/db/s/transaction_coordinator_futures_util.h"

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/grid.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace txn {
namespace {

MONGO_FAIL_POINT_DEFINE(hangWhileTargetingRemoteHost);
MONGO_FAIL_POINT_DEFINE(hangWhileTargetingLocalHost);

using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using ResponseStatus = executor::TaskExecutor::ResponseStatus;

}  // namespace

AsyncWorkScheduler::AsyncWorkScheduler(ServiceContext* serviceContext)
    : _serviceContext(serviceContext),
      _executor(Grid::get(_serviceContext)->getExecutorPool()->getFixedExecutor()) {}

AsyncWorkScheduler::~AsyncWorkScheduler() {
    join();

    if (!_parent)
        return;

    stdx::lock_guard<stdx::mutex> lg(_parent->_mutex);
    _parent->_childSchedulers.erase(_itToRemove);
    _parent->_notifyAllTasksComplete(lg);
    _parent = nullptr;
}

Future<executor::TaskExecutor::ResponseStatus> AsyncWorkScheduler::scheduleRemoteCommand(
    const ShardId& shardId, const ReadPreferenceSetting& readPref, const BSONObj& commandObj) {

    const bool isSelfShard = (shardId == getLocalShardId(_serviceContext));

    if (isSelfShard) {
        // If sending a command to the same shard as this node is in, send it directly to this node
        // rather than going through the host targeting below. This ensures that the state changes
        // for the participant and coordinator occur sequentially on a single branch of replica set
        // history. See SERVER-38142 for details.
        return scheduleWork([ this, shardId, commandObj = commandObj.getOwned() ](OperationContext *
                                                                                  opCtx) {
            // Note: This internal authorization is tied to the lifetime of the client, which will
            // be destroyed by 'scheduleWork' immediately after this lambda ends
            AuthorizationSession::get(opCtx->getClient())
                ->grantInternalAuthorization(opCtx->getClient());

            if (MONGO_FAIL_POINT(hangWhileTargetingLocalHost)) {
                LOG(0) << "Hit hangWhileTargetingLocalHost failpoint";
                MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx, hangWhileTargetingLocalHost);
            }

            const auto service = opCtx->getServiceContext();
            auto start = _executor->now();

            auto requestOpMsg =
                OpMsgRequest::fromDBAndBody(NamespaceString::kAdminDb, commandObj).serialize();
            const auto replyOpMsg = OpMsg::parseOwned(
                service->getServiceEntryPoint()->handleRequest(opCtx, requestOpMsg).response);

            // Document sequences are not yet being used for responses.
            invariant(replyOpMsg.sequences.empty());

            // 'ResponseStatus' is the response format of a remote request sent over the network
            // so we simulate that format manually here, since we sent the request over the
            // loopback.
            return ResponseStatus{replyOpMsg.body.getOwned(), _executor->now() - start};
        });
    }

    return _targetHostAsync(shardId, readPref)
        .then([ this, shardId, commandObj = commandObj.getOwned(), readPref ](
            HostAndPort shardHostAndPort) mutable {
            executor::RemoteCommandRequest request(shardHostAndPort,
                                                   NamespaceString::kAdminDb.toString(),
                                                   commandObj,
                                                   readPref.toContainingBSON(),
                                                   nullptr);

            auto pf = makePromiseFuture<ResponseStatus>();

            stdx::unique_lock<stdx::mutex> ul(_mutex);
            uassertStatusOK(_shutdownStatus);

            auto scheduledCommandHandle =
                uassertStatusOK(_executor->scheduleRemoteCommand(request, [
                    this,
                    commandObj = std::move(commandObj),
                    shardId = std::move(shardId),
                    promise = std::make_shared<Promise<ResponseStatus>>(std::move(pf.promise))
                ](const RemoteCommandCallbackArgs& args) mutable noexcept {
                    auto status = args.response.status;
                    // Only consider actual failures to send the command as errors.
                    if (status.isOK()) {
                        promise->emplaceValue(std::move(args.response));
                    } else {
                        promise->setError([&] {
                            if (status == ErrorCodes::CallbackCanceled) {
                                stdx::unique_lock<stdx::mutex> ul(_mutex);
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
                [ this, it = std::move(it) ](StatusWith<ResponseStatus> s) {
                    stdx::lock_guard<stdx::mutex> lg(_mutex);
                    _activeHandles.erase(it);
                    _notifyAllTasksComplete(lg);
                });
        });
}

std::unique_ptr<AsyncWorkScheduler> AsyncWorkScheduler::makeChildScheduler() {
    auto child = stdx::make_unique<AsyncWorkScheduler>(_serviceContext);

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    if (!_shutdownStatus.isOK())
        child->shutdown(_shutdownStatus);

    child->_parent = this;
    child->_itToRemove = _childSchedulers.emplace(_childSchedulers.begin(), child.get());

    return child;
}

void AsyncWorkScheduler::shutdown(Status status) {
    invariant(!status.isOK());

    stdx::lock_guard<stdx::mutex> lg(_mutex);
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
    stdx::unique_lock<stdx::mutex> ul(_mutex);
    _allListsEmptyCV.wait(ul, [&] {
        return _activeOpContexts.empty() && _activeHandles.empty() && _childSchedulers.empty();
    });
}

Future<HostAndPort> AsyncWorkScheduler::_targetHostAsync(const ShardId& shardId,
                                                         const ReadPreferenceSetting& readPref) {
    return scheduleWork([shardId, readPref](OperationContext* opCtx) {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        const auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));

        if (MONGO_FAIL_POINT(hangWhileTargetingRemoteHost)) {
            LOG(0) << "Hit hangWhileTargetingRemoteHost failpoint for shard " << shardId;
            MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(opCtx, hangWhileTargetingRemoteHost);
        }

        // TODO (SERVER-35678): Return a SemiFuture<HostAndPort> rather than using a blocking call
        return shard->getTargeter()->findHostWithMaxWait(readPref, Seconds(20)).get(opCtx);
    });
}

void AsyncWorkScheduler::_notifyAllTasksComplete(WithLock) {
    if (_activeOpContexts.empty() && _activeHandles.empty() && _childSchedulers.empty())
        _allListsEmptyCV.notify_all();
}

ShardId getLocalShardId(ServiceContext* service) {
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        return ShardRegistry::kConfigServerShardId;
    }
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        return ShardingState::get(service)->shardId();
    }

    // Only sharded systems should use the two-phase commit path
    MONGO_UNREACHABLE;
}

Future<void> whenAll(std::vector<Future<void>>& futures) {
    std::vector<Future<int>> dummyFutures;
    for (auto&& f : futures) {
        dummyFutures.push_back(std::move(f).then([]() { return 0; }));
    }
    return collect(
               std::move(dummyFutures), 0, [](int, const int&) { return ShouldStopIteration::kNo; })
        .ignoreValue();
}

}  // namespace txn
}  // namespace mongo
