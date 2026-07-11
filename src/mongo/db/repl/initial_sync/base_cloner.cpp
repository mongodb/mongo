// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/initial_sync/base_cloner.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <string_view>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeClonerStage);
MONGO_FAIL_POINT_DEFINE(hangBeforeRetryingClonerStage);
MONGO_FAIL_POINT_DEFINE(hangAfterClonerStage);
}  // namespace
using executor::TaskExecutor;

namespace repl {

BaseCloner::BaseCloner(std::string_view clonerName,
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
        std::lock_guard<std::mutex> lk(_mutex);
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
        std::lock_guard<std::mutex> lk(_mutex);
        _active = false;
        if (!_status.isOK()) {
            return _status;
        }
    }
    std::lock_guard<ReplSyncSharedData> lk(*_sharedData);
    if (!_sharedData->getStatus(lk).isOK()) {
        LOGV2_OPTIONS(21065,
                      {getLogComponent()},
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
                              "Non-retryable error occurred during cloner stage",
                              "cloner"_attr = getClonerName(),
                              "stage"_attr = stage->getName(),
                              "error"_attr = lastError);
                throw;
            }
            LOGV2_DEBUG_OPTIONS(21078,
                                1,
                                {getLogComponent()},
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
        std::lock_guard<std::mutex> lk(_mutex);
        invariant(!_active);
        invariant(!_startedAsync);
        _startedAsync = true;
    }
    auto pf = makePromiseFuture<void>();
    // The promise has to be a class variable to correctly return the error code in the case
    // where executor->scheduleWork fails (i.e. when shutting down)
    _promise = std::move(pf.promise);
    auto callback = [this](const TaskExecutor::CallbackArgs& args) mutable {
        if (!args.status.isOK()) {
            {
                std::lock_guard<std::mutex> lk(_mutex);
                _startedAsync = false;
            }
            // The _promise can run the error callback on this thread, so we must not hold the lock
            // when we set it.
            _promise.setError(args.status);
            return;
        }
        _promise.setWith([this] {
            Status status = run();
            std::lock_guard<std::mutex> lk(_mutex);
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
            std::lock_guard<ReplSyncSharedData> lk(*_sharedData);
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
        std::lock_guard<std::mutex> lk(_mutex);
        _status = status;
    }
    std::lock_guard<ReplSyncSharedData> lk(*_sharedData);
    _sharedData->setStatusIfOK(lk, status);
}

bool BaseCloner::mustExit() {
    std::lock_guard<ReplSyncSharedData> lk(*_sharedData);
    return !_sharedData->getStatus(lk).isOK();
}

}  // namespace repl
}  // namespace mongo
