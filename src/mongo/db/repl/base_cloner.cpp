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
#include "mongo/util/log.h"

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeClonerStage);
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
                       ThreadPool* dbPool,
                       ClockSource* clock)
    : _clonerName(clonerName),
      _sharedData(sharedData),
      _client(client),
      _storageInterface(storageInterface),
      _dbPool(dbPool),
      _source(source),
      _clock(clock) {
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
    stdx::lock_guard<Latch> lk(_sharedData->mutex);
    if (!_sharedData->initialSyncStatus.isOK()) {
        log() << "Failing data clone because initial sync failed outside data clone: "
              << _sharedData->initialSyncStatus;
    }
    return _sharedData->initialSyncStatus;
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
            log() << "Collection Cloner scheduled a remote command on the "
                  << describeForFuzzer(stage);
            log() << "initialSyncFuzzerSynchronizationPoint1 fail point enabled.";
            initialSyncFuzzerSynchronizationPoint1.pauseWhileSet();

            if (MONGO_unlikely(initialSyncFuzzerSynchronizationPoint2.shouldFail())) {
                log() << "initialSyncFuzzerSynchronizationPoint2 fail point enabled.";
                initialSyncFuzzerSynchronizationPoint2.pauseWhileSet();
            }
        }
    }
}

BaseCloner::AfterStageBehavior BaseCloner::runStage(BaseClonerStage* stage) {
    // TODO(SERVER-43275): Implement retry logic here.  Alternately, do the initial connection
    // in the retry logic, but make sure not to count the initial attempt as a "re-" try.
    LOG(1) << "Cloner " << getClonerName() << " running stage " << stage->getName();
    pauseForFuzzer(stage);
    auto isThisStageFailPoint = [this, stage](const BSONObj& data) {
        return data["stage"].str() == stage->getName() && isMyFailPoint(data);
    };
    hangBeforeClonerStage.executeIf(
        [&](const BSONObj& data) {
            log() << "Cloner " << getClonerName() << " hanging before running stage "
                  << stage->getName();
            while (!mustExit() && hangBeforeClonerStage.shouldFail(isThisStageFailPoint)) {
                sleepmillis(100);
            }
        },
        isThisStageFailPoint);
    if (_client->isFailed()) {
        Status failed(ErrorCodes::HostUnreachable, "Client is disconnected");
        log() << "Failed because host " << getSource() << " is unreachable.";
        setInitialSyncFailedStatus(failed);
        uassertStatusOK(failed);
    }
    auto afterStageBehavior = stage->run();
    hangAfterClonerStage.executeIf(
        [&](const BSONObj& data) {
            log() << "Cloner " << getClonerName() << " hanging after running stage "
                  << stage->getName();
            while (!mustExit() && hangAfterClonerStage.shouldFail(isThisStageFailPoint)) {
                sleepmillis(100);
            }
        },
        isThisStageFailPoint);
    LOG(1) << "Cloner " << getClonerName() << " finished running stage " << stage->getName();
    return afterStageBehavior;
}

Future<void> BaseCloner::runOnExecutor(TaskExecutor* executor) {
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
            stdx::lock_guard<Latch> lk(_mutex);
            _startedAsync = false;
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
    auto cbhStatus = executor->scheduleWork(callback);
    if (!cbhStatus.isOK()) {
        _promise.setError(cbhStatus.getStatus());
    }
    return std::move(pf.future);
}


void BaseCloner::setStopAfterStage_forTest(std::string stage) {
    _stopAfterStage = stage;
}

BaseCloner::AfterStageBehavior BaseCloner::runStages() {
    AfterStageBehavior afterStageBehavior = kContinueNormally;
    for (auto* stage : getStages()) {
        {
            stdx::lock_guard<Latch> lk(_sharedData->mutex);
            if (!_sharedData->initialSyncStatus.isOK())
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
    stdx::lock_guard<Latch> lk(_sharedData->mutex);
    if (_sharedData->initialSyncStatus.isOK()) {
        _sharedData->initialSyncStatus = status;
    }
}

bool BaseCloner::mustExit() {
    stdx::lock_guard<Latch> lk(_sharedData->mutex);
    return !_sharedData->initialSyncStatus.isOK();
}

}  // namespace repl
}  // namespace mongo
