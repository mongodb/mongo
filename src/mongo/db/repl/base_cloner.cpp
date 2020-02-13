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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplicationInitialSync

#include "mongo/platform/basic.h"

#include "mongo/db/repl/base_cloner.h"
#include "mongo/logv2/log.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeClonerStage);
MONGO_FAIL_POINT_DEFINE(hangBeforeRetryingClonerStage);
MONGO_FAIL_POINT_DEFINE(hangBeforeCheckingRollBackIdClonerStage);
MONGO_FAIL_POINT_DEFINE(hangAfterClonerStage);
}  // namespace
using executor::TaskExecutor;

namespace repl {
// These failpoints are shared with initial_syncer and so must not be in the unnamed namespace.
MONGO_FAIL_POINT_DEFINE(initialSyncFuzzerSynchronizationPoint1);
MONGO_FAIL_POINT_DEFINE(initialSyncFuzzerSynchronizationPoint2);

BaseCloner::BaseCloner(StringData clonerName,
                       InitialSyncSharedData* sharedData,
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
        setInitialSyncFailedStatus(e.toStatus());
    }
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _active = false;
        if (!_status.isOK()) {
            return _status;
        }
    }
    stdx::lock_guard<InitialSyncSharedData> lk(*_sharedData);
    if (!_sharedData->getInitialSyncStatus(lk).isOK()) {
        LOGV2(21065,
              "Failing data clone because initial sync failed outside data clone: "
              "{sharedData_getInitialSyncStatus_lk}",
              "sharedData_getInitialSyncStatus_lk"_attr = _sharedData->getInitialSyncStatus(lk));
    }
    return _sharedData->getInitialSyncStatus(lk);
}

bool BaseCloner::isMyFailPoint(const BSONObj& data) const {
    return data["cloner"].str() == getClonerName();
}

void BaseCloner::pauseForFuzzer(BaseClonerStage* stage) {
    // These are the stages that the initial sync fuzzer expects to be able to pause on using the
    // syncronization fail points.
    static const auto initialSyncPauseStages =
        std::vector<std::string>{"listCollections", "listIndexes", "listDatabases"};

    if (MONGO_unlikely(initialSyncFuzzerSynchronizationPoint1.shouldFail())) {
        if (std::find(initialSyncPauseStages.begin(),
                      initialSyncPauseStages.end(),
                      stage->getName()) != initialSyncPauseStages.end()) {
            // These failpoints are set and unset by the InitialSyncTest fixture to cause initial
            // sync to pause so that the Initial Sync Fuzzer can run commands on the sync source.
            // nb: This log message is specifically checked for in
            // initial_sync_test_fixture_test.js, so if you change it here you will need to change
            // it there.
            LOGV2(21066,
                  "Collection Cloner scheduled a remote command on the {describeForFuzzer_stage}",
                  "describeForFuzzer_stage"_attr = describeForFuzzer(stage));
            LOGV2(21067, "initialSyncFuzzerSynchronizationPoint1 fail point enabled.");
            initialSyncFuzzerSynchronizationPoint1.pauseWhileSet();

            if (MONGO_unlikely(initialSyncFuzzerSynchronizationPoint2.shouldFail())) {
                LOGV2(21068, "initialSyncFuzzerSynchronizationPoint2 fail point enabled.");
                initialSyncFuzzerSynchronizationPoint2.pauseWhileSet();
            }
        }
    }
}

BaseCloner::AfterStageBehavior BaseCloner::runStage(BaseClonerStage* stage) {
    LOGV2_DEBUG(21069,
                1,
                "Cloner {getClonerName} running stage {stage_getName}",
                "getClonerName"_attr = getClonerName(),
                "stage_getName"_attr = stage->getName());
    pauseForFuzzer(stage);
    auto isThisStageFailPoint = [this, stage](const BSONObj& data) {
        return data["stage"].str() == stage->getName() && isMyFailPoint(data);
    };
    hangBeforeClonerStage.executeIf(
        [&](const BSONObj& data) {
            LOGV2(21070,
                  "Cloner {getClonerName} hanging before running stage {stage_getName}",
                  "getClonerName"_attr = getClonerName(),
                  "stage_getName"_attr = stage->getName());
            while (!mustExit() && hangBeforeClonerStage.shouldFail(isThisStageFailPoint)) {
                sleepmillis(100);
            }
        },
        isThisStageFailPoint);
    auto afterStageBehavior = runStageWithRetries(stage);
    hangAfterClonerStage.executeIf(
        [&](const BSONObj& data) {
            LOGV2(21071,
                  "Cloner {getClonerName} hanging after running stage {stage_getName}",
                  "getClonerName"_attr = getClonerName(),
                  "stage_getName"_attr = stage->getName());
            while (!mustExit() && hangAfterClonerStage.shouldFail(isThisStageFailPoint)) {
                sleepmillis(100);
            }
        },
        isThisStageFailPoint);
    LOGV2_DEBUG(21072,
                1,
                "Cloner {getClonerName} finished running stage {stage_getName}",
                "getClonerName"_attr = getClonerName(),
                "stage_getName"_attr = stage->getName());
    return afterStageBehavior;
}

void BaseCloner::clearRetryingState() {
    _retryableOp = boost::none;
}

Status BaseCloner::checkRollBackIdIsUnchanged() {
    BSONObj info;
    try {
        getClient()->simpleCommand("admin", &info, "replSetGetRBID");
    } catch (DBException& e) {
        if (ErrorCodes::isRetriableError(e)) {
            auto status = e.toStatus().withContext(
                ": failed while attempting to retrieve rollBackId after re-connect");
            LOGV2_DEBUG(21073, 1, "{status}", "status"_attr = status);
            return status;
        }
        throw;
    }
    uassert(
        31298, "Sync source returned invalid result from replSetGetRBID", info["rbid"].isNumber());
    auto rollBackId = info["rbid"].numberInt();
    uassert(ErrorCodes::UnrecoverableRollbackError,
            str::stream() << "Rollback occurred on our sync source " << getSource()
                          << " during initial sync",
            rollBackId == _sharedData->getRollBackId());
    return Status::OK();
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
                        LOGV2(
                            21074,
                            "Cloner {getClonerName} hanging before retrying stage {stage_getName}",
                            "getClonerName"_attr = getClonerName(),
                            "stage_getName"_attr = stage->getName());
                        while (!mustExit() &&
                               hangBeforeRetryingClonerStage.shouldFail(isThisStageFailPoint)) {
                            sleepmillis(100);
                        }
                    },
                    isThisStageFailPoint);
                LOGV2(21075,
                      "Initial Sync retrying {getClonerName} stage {stage_getName} due to "
                      "{lastError}",
                      "getClonerName"_attr = getClonerName(),
                      "stage_getName"_attr = stage->getName(),
                      "lastError"_attr = lastError);
                bool shouldRetry = [&] {
                    stdx::lock_guard<InitialSyncSharedData> lk(*_sharedData);
                    return _sharedData->shouldRetryOperation(lk, &_retryableOp);
                }();
                if (!shouldRetry) {
                    auto status = lastError.withContext(
                        str::stream()
                        << ": Exceeded initialSyncTransientErrorRetryPeriodSeconds "
                        << _sharedData->getAllowedOutageDuration(
                               stdx::lock_guard<InitialSyncSharedData>(*_sharedData)));
                    setInitialSyncFailedStatus(status);
                    uassertStatusOK(status);
                }
                hangBeforeCheckingRollBackIdClonerStage.executeIf(
                    [&](const BSONObj& data) {
                        LOGV2(21076,
                              "Cloner {getClonerName} hanging before checking rollBackId for stage "
                              "{stage_getName}",
                              "getClonerName"_attr = getClonerName(),
                              "stage_getName"_attr = stage->getName());
                        while (!mustExit() &&
                               hangBeforeCheckingRollBackIdClonerStage.shouldFail(
                                   isThisStageFailPoint)) {
                            sleepmillis(100);
                        }
                    },
                    isThisStageFailPoint);
                if (stage->checkRollBackIdOnRetry()) {
                    // If checkRollBackIdIsUnchanged fails without throwing, it means a network
                    // error occurred and it's safe to continue (which will cause another retry).
                    if (!checkRollBackIdIsUnchanged().isOK())
                        continue;
                    // After successfully checking the rollback ID, the client should always be OK.
                    invariant(!getClient()->isFailed());
                }
            }
            return stage->run();
        } catch (DBException& e) {
            lastError = e.toStatus();
            if (!stage->isTransientError(lastError)) {
                LOGV2(21077,
                      "Non-retryable error occured during cloner "
                      "{getClonerName}{stage_stage_getName}: {lastError}",
                      "getClonerName"_attr = getClonerName(),
                      "stage_stage_getName"_attr = " stage " + stage->getName(),
                      "lastError"_attr = lastError);
                throw;
            }
            LOGV2_DEBUG(21078,
                        1,
                        "Transient error occured during cloner "
                        "{getClonerName}{stage_stage_getName}: {lastError}",
                        "getClonerName"_attr = getClonerName(),
                        "stage_stage_getName"_attr = " stage " + stage->getName(),
                        "lastError"_attr = lastError);
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
    for (auto* stage : getStages()) {
        {
            stdx::lock_guard<InitialSyncSharedData> lk(*_sharedData);
            if (!_sharedData->getInitialSyncStatus(lk).isOK())
                return kSkipRemainingStages;
        }
        afterStageBehavior = runStage(stage);
        if (afterStageBehavior == kSkipRemainingStages || _stopAfterStage == stage->getName())
            break;
    }
    return afterStageBehavior;
}

void BaseCloner::setInitialSyncFailedStatus(Status status) {
    invariant(!status.isOK());
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _status = status;
    }
    stdx::lock_guard<InitialSyncSharedData> lk(*_sharedData);
    _sharedData->setInitialSyncStatusIfOK(lk, status);
}

bool BaseCloner::mustExit() {
    stdx::lock_guard<InitialSyncSharedData> lk(*_sharedData);
    return !_sharedData->getInitialSyncStatus(lk).isOK();
}

}  // namespace repl
}  // namespace mongo
