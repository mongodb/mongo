
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

#include "mongo/db/transaction_coordinator_futures_util.h"

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/util/log.h"

namespace mongo {
namespace txn {
namespace {

using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using ResponseStatus = executor::TaskExecutor::ResponseStatus;

}  // namespace

AsyncWorkScheduler::AsyncWorkScheduler(ServiceContext* serviceContext)
    : _serviceContext(serviceContext),
      _executor(Grid::get(_serviceContext)->getExecutorPool()->getFixedExecutor()) {}

AsyncWorkScheduler::~AsyncWorkScheduler() = default;

Future<executor::TaskExecutor::ResponseStatus> AsyncWorkScheduler::scheduleRemoteCommand(
    const ShardId& shardId, const ReadPreferenceSetting& readPref, const BSONObj& commandObj) {
    auto promiseAndFuture = makePromiseFuture<ResponseStatus>();
    auto sharedPromise =
        std::make_shared<Promise<ResponseStatus>>(std::move(promiseAndFuture.promise));

    _targetHostAsync(shardId, readPref)
        .then([ this, shardId, sharedPromise, commandObj = commandObj.getOwned(), readPref ](
            HostAndPort shardHostAndPort) mutable {
            LOG(3) << "Coordinator sending command " << commandObj << " to shard " << shardId;

            executor::RemoteCommandRequest request(shardHostAndPort,
                                                   NamespaceString::kAdminDb.toString(),
                                                   commandObj,
                                                   readPref.toContainingBSON(),
                                                   nullptr);

            uassertStatusOK(_executor->scheduleRemoteCommand(
                request, [ commandObj = commandObj.getOwned(),
                           shardId,
                           sharedPromise ](const RemoteCommandCallbackArgs& args) mutable {
                    LOG(3) << "Coordinator shard got response " << args.response.data << " for "
                           << commandObj << " to " << shardId;
                    auto status = args.response.status;
                    // Only consider actual failures to send the command as errors.
                    if (status.isOK()) {
                        sharedPromise->emplaceValue(args.response);
                    } else {
                        sharedPromise->setError(status);
                    }
                }));
        })
        .onError([ shardId, commandObj = commandObj.getOwned(), sharedPromise ](Status s) {
            LOG(3) << "Coordinator shard failed to target command " << commandObj << " to shard "
                   << shardId << causedBy(s);

            sharedPromise->setError(s);
        })
        .getAsync([](Status) {});

    // Do not wait for the callback to run. The callback will reschedule the remote request on
    // the same executor if necessary.
    return std::move(promiseAndFuture.future);
}

Future<HostAndPort> AsyncWorkScheduler::_targetHostAsync(const ShardId& shardId,
                                                         const ReadPreferenceSetting& readPref) {
    return scheduleWork([shardId, readPref](OperationContext* opCtx) {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        const auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));

        // TODO (SERVER-35678): Return a SemiFuture<HostAndPort> rather than using a blocking call
        return shard->getTargeter()->findHostWithMaxWait(readPref, Seconds(20)).get(opCtx);
    });
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
