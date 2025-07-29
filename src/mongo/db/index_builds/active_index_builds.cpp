/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <absl/container/node_hash_map.h>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/move/utility_core.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/index_builds/active_index_builds.h"
#include "mongo/db/index_builds/index_builds_manager.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

ActiveIndexBuilds::~ActiveIndexBuilds() {
    invariant(_allIndexBuilds.empty());
}

void ActiveIndexBuilds::waitForAllIndexBuildsToStopForShutdown() {
    waitForAllIndexBuildsToStop(OperationContext::notInterruptible());
}

void ActiveIndexBuilds::waitForAllIndexBuildsToStop(Interruptible* interruptible) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // All index builds should have been signaled to stop via the ServiceContext.

    if (_allIndexBuilds.empty()) {
        return;
    }

    auto indexBuildToUUID = [](const auto& indexBuild) {
        return indexBuild.first;
    };
    auto begin = boost::make_transform_iterator(_allIndexBuilds.begin(), indexBuildToUUID);
    auto end = boost::make_transform_iterator(_allIndexBuilds.end(), indexBuildToUUID);
    LOGV2(4725201,
          "Waiting until the following index builds are finished",
          "indexBuilds"_attr = logv2::seqLog(begin, end));

    // Wait for all the index builds to stop.
    auto pred = [this]() {
        return _allIndexBuilds.empty();
    };
    interruptible->waitForConditionOrInterrupt(_indexBuildsCondVar, lk, pred);
}

void ActiveIndexBuilds::assertNoIndexBuildInProgress() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (!_allIndexBuilds.empty()) {
        auto firstIndexBuild = _allIndexBuilds.cbegin()->second;
        uasserted(ErrorCodes::BackgroundOperationInProgressForDatabase,
                  fmt::format("cannot perform operation: there are currently {} index builds "
                              "running. Found index build: {}",
                              _allIndexBuilds.size(),
                              firstIndexBuild->buildUUID.toString()));
    }
}

void ActiveIndexBuilds::waitUntilAnIndexBuildFinishes(OperationContext* opCtx, Date_t deadline) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_allIndexBuilds.empty()) {
        return;
    }
    const auto generation = _indexBuildsCompletedGen;
    opCtx->waitForConditionOrInterruptUntil(
        _indexBuildsCondVar, lk, deadline, [&] { return _indexBuildsCompletedGen != generation; });
}

void ActiveIndexBuilds::sleepIndexBuilds_forTestOnly(bool sleep) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _sleepForTest = sleep;
}

void ActiveIndexBuilds::verifyNoIndexBuilds_forTestOnly() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_allIndexBuilds.empty());
}

void ActiveIndexBuilds::awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                                 const UUID& collectionUUID,
                                                                 IndexBuildProtocol protocol) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto noIndexBuildsPred = [&, this]() {
        auto indexBuilds = _filterIndexBuilds_inlock(lk, [&](const auto& replState) {
            return collectionUUID == replState.collectionUUID && protocol == replState.protocol;
        });
        return indexBuilds.empty();
    };
    opCtx->waitForConditionOrInterrupt(_indexBuildsCondVar, lk, noIndexBuildsPred);
}

void ActiveIndexBuilds::awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                                 const UUID& collectionUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto pred = [&, this]() {
        auto indexBuilds = _filterIndexBuilds_inlock(
            lk, [&](const auto& replState) { return collectionUUID == replState.collectionUUID; });
        return indexBuilds.empty();
    };
    _indexBuildsCondVar.wait(lk, pred);
}

StatusWith<std::shared_ptr<ReplIndexBuildState>> ActiveIndexBuilds::getIndexBuild(
    const UUID& buildUUID) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto it = _allIndexBuilds.find(buildUUID);
    if (it == _allIndexBuilds.end()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "No index build with UUID: " << buildUUID};
    }
    return it->second;
}

std::vector<std::shared_ptr<ReplIndexBuildState>> ActiveIndexBuilds::getAllIndexBuilds() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _filterIndexBuilds_inlock(lk, [](const auto& replState) { return true; });
}

void ActiveIndexBuilds::unregisterIndexBuild(
    IndexBuildsManager* indexBuildsManager,
    std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    invariant(_allIndexBuilds.erase(replIndexBuildState->buildUUID));

    LOGV2_DEBUG(4656004,
                1,
                "Index build: unregistering",
                "buildUUID"_attr = replIndexBuildState->buildUUID,
                "collectionUUID"_attr = replIndexBuildState->collectionUUID);

    indexBuildsManager->tearDownAndUnregisterIndexBuild(replIndexBuildState->buildUUID);
    _indexBuildsCompletedGen++;
    _indexBuildsCondVar.notify_all();
}

std::vector<std::shared_ptr<ReplIndexBuildState>> ActiveIndexBuilds::filterIndexBuilds(
    IndexBuildFilterFn indexBuildFilter) const {

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _filterIndexBuilds_inlock(lk, indexBuildFilter);
}

std::vector<std::shared_ptr<ReplIndexBuildState>> ActiveIndexBuilds::_filterIndexBuilds_inlock(
    WithLock lk, IndexBuildFilterFn indexBuildFilter) const {

    std::vector<std::shared_ptr<ReplIndexBuildState>> indexBuilds;
    for (const auto& pair : _allIndexBuilds) {
        auto replState = pair.second;
        if (!indexBuildFilter(*replState)) {
            continue;
        }
        indexBuilds.push_back(replState);
    }
    return indexBuilds;
}

void ActiveIndexBuilds::awaitNoBgOpInProgForDb(OperationContext* opCtx,
                                               const DatabaseName& dbName) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto indexBuildFilter = [dbName](const auto& replState) {
        return dbName == replState.dbName;
    };
    auto pred = [&, this]() {
        auto dbIndexBuilds = _filterIndexBuilds_inlock(lk, indexBuildFilter);
        return dbIndexBuilds.empty();
    };
    _indexBuildsCondVar.wait(lk, pred);
}

Status ActiveIndexBuilds::registerIndexBuild(
    std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    // Check whether any indexes are already being built with the same index name(s). (Duplicate
    // specs will be discovered by the index builder.)
    auto pred = [&](const auto& replState) {
        return replIndexBuildState->collectionUUID == replState.collectionUUID;
    };
    auto collIndexBuilds = _filterIndexBuilds_inlock(lk, pred);
    for (const auto& existingIndexBuild : collIndexBuilds) {
        for (const auto& name : toIndexNames(replIndexBuildState->getIndexes())) {
            auto existingIndexNames = toIndexNames(existingIndexBuild->getIndexes());
            if (existingIndexNames.end() !=
                std::find(existingIndexNames.begin(), existingIndexNames.end(), name)) {
                return existingIndexBuild->onConflictWithNewIndexBuild(*replIndexBuildState, name);
            }
        }
    }

    invariant(_allIndexBuilds.emplace(replIndexBuildState->buildUUID, replIndexBuildState).second);

    _indexBuildsCondVar.notify_all();

    return Status::OK();
}

size_t ActiveIndexBuilds::getActiveIndexBuildsCount() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _allIndexBuilds.size();
}

void ActiveIndexBuilds::appendBuildInfo(const UUID& buildUUID, BSONObjBuilder* builder) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto it = _allIndexBuilds.find(buildUUID);
    if (it == _allIndexBuilds.end()) {
        return;
    }
    it->second->appendBuildInfo(builder);
}

void ActiveIndexBuilds::sleepIfNecessary_forTestOnly() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (_sleepForTest) {
        lk.unlock();
        sleepmillis(100);
        lk.lock();
    }
}
}  // namespace mongo
