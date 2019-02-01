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

#include "mongo/platform/basic.h"

#include "mongo/db/collection_index_builds_tracker.h"

#include "mongo/db/catalog/index_builds_manager.h"

namespace mongo {

CollectionIndexBuildsTracker::~CollectionIndexBuildsTracker() {
    invariant(_buildStateByBuildUUID.empty());
    invariant(_buildStateByIndexName.empty());
}

void CollectionIndexBuildsTracker::addIndexBuild(
    WithLock, std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {
    // Ensure that a new entry is added.
    invariant(
        _buildStateByBuildUUID.emplace(replIndexBuildState->buildUUID, replIndexBuildState).second);

    invariant(replIndexBuildState->indexNames.size());
    for (auto& indexName : replIndexBuildState->indexNames) {
        // Ensure that a new entry is added.
        invariant(_buildStateByIndexName.emplace(indexName, replIndexBuildState).second,
                  str::stream() << "index build state for " << indexName
                                << " already exists. Collection: "
                                << replIndexBuildState->collectionUUID);
    }
}

void CollectionIndexBuildsTracker::removeIndexBuild(
    WithLock, std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {
    invariant(_buildStateByBuildUUID.find(replIndexBuildState->buildUUID) !=
              _buildStateByBuildUUID.end());
    _buildStateByBuildUUID.erase(replIndexBuildState->buildUUID);

    for (const auto& indexName : replIndexBuildState->indexNames) {
        invariant(_buildStateByIndexName.find(indexName) != _buildStateByIndexName.end());
        _buildStateByIndexName.erase(indexName);
    }

    if (_buildStateByBuildUUID.empty()) {
        _noIndexBuildsRemainCondVar.notify_all();
    }
}

std::shared_ptr<ReplIndexBuildState> CollectionIndexBuildsTracker::getIndexBuildState(
    WithLock, StringData indexName) const {
    auto it = _buildStateByIndexName.find(indexName.toString());
    invariant(it != _buildStateByIndexName.end());
    return it->second;
}

bool CollectionIndexBuildsTracker::hasIndexBuildState(WithLock, StringData indexName) const {
    auto it = _buildStateByIndexName.find(indexName.toString());
    if (it == _buildStateByIndexName.end()) {
        return false;
    }
    return true;
}

void CollectionIndexBuildsTracker::runOperationOnAllBuilds(
    WithLock lk,
    IndexBuildsManager* indexBuildsManager,
    std::function<void(WithLock,
                       IndexBuildsManager* indexBuildsManager,
                       std::shared_ptr<ReplIndexBuildState> replIndexBuildState,
                       const std::string& reason)> func,
    const std::string& reason) noexcept {
    for (auto it = _buildStateByBuildUUID.begin(); it != _buildStateByBuildUUID.end(); ++it) {
        func(lk, indexBuildsManager, it->second, reason);
    }
}

int CollectionIndexBuildsTracker::getNumberOfIndexBuilds(WithLock) const {
    return _buildStateByBuildUUID.size();
}

void CollectionIndexBuildsTracker::waitUntilNoIndexBuildsRemain(
    stdx::unique_lock<stdx::mutex>& lk) {
    _noIndexBuildsRemainCondVar.wait(lk, [&] { return _buildStateByBuildUUID.empty(); });
}

}  // namespace mongo
