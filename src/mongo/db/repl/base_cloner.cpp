/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include <mutex>

#include <boost/move/utility_core.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/repl/base_cloner.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeClonerStage);
MONGO_FAIL_POINT_DEFINE(hangBeforeRetryingClonerStage);
MONGO_FAIL_POINT_DEFINE(hangAfterClonerStage);
}  // namespace
using executor::TaskExecutor;

namespace repl {

BaseCloner::BaseCloner(StringData clonerName,
                       ReplSyncSharedData* sharedData,
                       HostAndPort source,
                       DBClientConnection* client,
                       StorageInterface* storageInterface,
                       ThreadPool* dbPool)
    : _clonerName(clonerName),
      _sharedData(sharedData),
      _client(client),
      _storageInterface(storageInterface),
      _dbPool(dbPool),
      _source(source) {
    invariant(sharedData);
    invariant(!source.empty());
    invariant(client);
    invariant(storageInterface);
    invariant(dbPool);
}

Status BaseCloner::run() {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _active = true;
    }
    try {
        preStage();
        auto afterStageBehavior = runStages();
        if (afterStageBehavior == kContinueNormally && _stopAfterStage.empty()) {
            postStage();
        }
    } catch (DBException& e) {
        setSyncFailedStatus(e.toStatus());
    }
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _active = false;
        if (!_status.isOK()) {
            return _status;
        }
    }
    stdx::lock_guard<ReplSyncSharedData> lk(*_sharedData);
    if (!_sharedData->getStatus(lk).isOK()) {
        LOGV2_OPTIONS(21065,
                      {getLogComponent()},
                      "Failing data clone because of failure outside data clone: "
                      "{error}",
                      "Failing data clone because of failure outside data clone",
                      "error"_attr = _sharedData->getStatus(lk));
    }
    return _sharedData->getStatus(lk);
}

bool BaseCloner::isMyFailPoint(const BSONObj& data) const {
    return data["cloner"].str() == getClonerName();
}

BaseCloner::AfterStageBehavior BaseCloner::runStage(BaseClonerStage* stage) {
    LOGV2_DEBUG_OPTIONS(21069,
                        1,
                        {getLogComponent()},
                        "Cloner {cloner} running stage {stage}",
                        "Cloner running stage",
                        "cloner"_attr = getClonerName(),
                        "stage"_attr = stage->getName());
    pauseForFuzzer(stage);
    auto isThisStageFailPoint = [this, stage](const BSONObj& data) {
        return data["stage"].str() == stage->getName() && isMyFailPoint(data);
    };
    hangBeforeClonerStage.executeIf(
        [&](const BSONObj& data) {
            LOGV2_OPTIONS(21070,
                          {getLogComponent()},
                          "Cloner {cloner} hanging before running stage {stage}",
                          "Cloner hanging before running stage",
                          "cloner"_attr = getClonerName(),
                          "stage"_attr = stage->getName());
            while (!mustExit() && hangBeforeClonerStage.shouldFail(isThisStageFailPoint)) {
                sleepmillis(100);
            }
        },
        isThisStageFailPoint);
    auto afterStageBehavior = runStageWithRetries(stage);
    hangAfterClonerStage.executeIf(
        [&](const BSONObj& data) {
            LOGV2_OPTIONS(21071,
                          {getLogComponent()},
                          "Cloner {cloner} hanging after running stage {stage}",
                          "Cloner hanging after running stage",
                          "cloner"_attr = getClonerName(),
                          "stage"_attr = stage->getName());
            while (!mustExit() && hangAfterClonerStage.shouldFail(isThisStageFailPoint)) {
                sleepmillis(100);
            }
        },
        isThisStageFailPoint);
    LOGV2_DEBUG_OPTIONS(21072,
                        1,
                        {getLogComponent()},
                        "Cloner {cloner} finished running stage {stage}",
                        "Cloner finished running stage",
                        "cloner"_attr = getClonerName(),
                        "stage"_attr = stage->getName());
    return afterStageBehavior;
}

BaseCloner::AfterStageBehavior BaseCloner::runStageWithRetries(BaseClonerStage* stage) {
    ON_BLOCK_EXIT([this] { clearRetryingState(); });
    Status lastError = Status::OK();
    auto isThisStageFailPoint = [this, stage](const BSONObj& data) {
        return data["stage"].str() == stage->getName() && isMyFailPoint(data);
    };
    while (true) {
        try {
            // mustExit is set when the clone has been canceled externally.
            if (mustExit())
                return kSkipRemainingStages;
            if (!lastError.isOK()) {
                // If lastError is set, this is a retry.
                hangBeforeRetryingClonerStage.executeIf(
                    [&](const BSONObj& data) {
                        LOGV2_OPTIONS(21074,
                                      {getLogComponent()},
                                      "Cloner {cloner} hanging before retrying stage {stage}",
                                      "Cloner hanging before retrying stage",
                                      "cloner"_attr = getClonerName(),
                                      "stage"_attr = stage->getName());
                        while (!mustExit() &&
                               hangBeforeRetryingClonerStage.shouldFail(isThisStageFailPoint)) {
                            sleepmillis(100);
                        }
                    },
                    isThisStageFailPoint);
                LOGV2_OPTIONS(21075,
                              {getLogComponent()},
                              "Sync process retrying {cloner} stage {stage} due to "
                              "{error}",
                              "Sync process retrying cloner stage due to error",
                              "cloner"_attr = getClonerName(),
                              "stage"_attr = stage->getName(),
                              "error"_attr = lastError);
                // Execute any per-retry logic needed by the cloner.
                handleStageAttemptFailed(stage, lastError);
            }
            return stage->run();
        } catch (DBException& e) {
            lastError = e.toStatus();
            if (!stage->isTransientError(lastError)) {
                LOGV2_OPTIONS(21077,
                              {getLogComponent()},
                              "Non-retryable error occurred during cloner "
                              "{cloner} stage {stage}: {error}",
                              "Non-retryable error occurred during cloner stage",
                              "cloner"_attr = getClonerName(),
                              "stage"_attr = stage->getName(),
                              "error"_attr = lastError);
                throw;
            }
            LOGV2_DEBUG_OPTIONS(21078,
                                1,
                                {getLogComponent()},
                                "Transient error occurred during cloner "
                                "{cloner} stage {stage}: {error}",
                                "Transient error occurred during cloner stage",
                                "cloner"_attr = getClonerName(),
                                "stage"_attr = stage->getName(),
                                "error"_attr = lastError);
        }
    }
}

std::pair<Future<void>, TaskExecutor::EventHandle> BaseCloner::runOnExecutorEvent(
    TaskExecutor* executor) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        invariant(!_active && !_startedAsync);
        _startedAsync = true;
    }
    auto pf = makePromiseFuture<void>();
    // The promise has to be a class variable to correctly return the error code in the case
    // where executor->scheduleWork fails (i.e. when shutting down)
    _promise = std::move(pf.promise);
    auto callback = [this](const TaskExecutor::CallbackArgs& args) mutable {
        if (!args.status.isOK()) {
            {
                stdx::lock_guard<Latch> lk(_mutex);
                _startedAsync = false;
            }
            // The _promise can run the error callback on this thread, so we must not hold the lock
            // when we set it.
            _promise.setError(args.status);
            return;
        }
        _promise.setWith([this] {
            Status status = run();
            stdx::lock_guard<Latch> lk(_mutex);
            _startedAsync = false;
            return status;
        });
    };
    TaskExecutor::EventHandle event;
    auto statusEvent = executor->makeEvent();
    if (!statusEvent.isOK()) {
        _promise.setError(statusEvent.getStatus());
    } else {
        event = statusEvent.getValue();
        auto cbhStatus = executor->onEvent(event, callback);
        if (!cbhStatus.isOK()) {
            _promise.setError(cbhStatus.getStatus());
        }
    }
    return std::make_pair(std::move(pf.future), event);
}


void BaseCloner::setStopAfterStage_forTest(std::string stage) {
    _stopAfterStage = stage;
}

BaseCloner::AfterStageBehavior BaseCloner::runStages() {
    AfterStageBehavior afterStageBehavior = kContinueNormally;
    if (_stopAfterStage == "preStage")
        return kSkipRemainingStages;
    for (auto* stage : getStages()) {
        {
            stdx::lock_guard<ReplSyncSharedData> lk(*_sharedData);
            if (!_sharedData->getStatus(lk).isOK())
                return kSkipRemainingStages;
        }
        afterStageBehavior = runStage(stage);
        if (afterStageBehavior == kSkipRemainingStages || _stopAfterStage == stage->getName())
            break;
    }
    return afterStageBehavior;
}

void BaseCloner::setSyncFailedStatus(Status status) {
    invariant(!status.isOK());
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _status = status;
    }
    stdx::lock_guard<ReplSyncSharedData> lk(*_sharedData);
    _sharedData->setStatusIfOK(lk, status);
}

bool BaseCloner::mustExit() {
    stdx::lock_guard<ReplSyncSharedData> lk(*_sharedData);
    return !_sharedData->getStatus(lk).isOK();
}

}  // namespace repl
}  // namespace mongo
