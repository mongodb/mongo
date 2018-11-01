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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_builds_manager.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/catalog/multi_index_block_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::shared_ptr;

IndexBuildsManager::~IndexBuildsManager() {
    invariant(_builders.empty());
}

Status IndexBuildsManager::setUpIndexBuild(OperationContext* opCtx,
                                           Collection* collection,
                                           const NamespaceString& nss,
                                           const std::vector<BSONObj>& specs,
                                           const UUID& buildUUID) {
    _registerIndexBuild(opCtx, collection, buildUUID);

    // TODO: Not yet implemented.

    return Status::OK();
}

StatusWith<IndexBuildRecoveryState> IndexBuildsManager::recoverIndexBuild(
    const NamespaceString& nss, const UUID& buildUUID, std::vector<std::string> indexNames) {

    // TODO: Not yet implemented.

    return IndexBuildRecoveryState::Building;
}

Status IndexBuildsManager::startBuildingIndex(const UUID& buildUUID) {
    auto multiIndexBlockPtr = _getBuilder(buildUUID);
    // TODO: verify that the index builder is in the expected state.

    // TODO: Not yet implemented.

    return Status::OK();
}

Status IndexBuildsManager::finishbBuildingPhase(const UUID& buildUUID) {
    auto multiIndexBlockPtr = _getBuilder(buildUUID);
    // TODO: verify that the index builder is in the expected state.

    // TODO: Not yet implemented.

    return Status::OK();
}

Status IndexBuildsManager::checkIndexConstraintViolations(const UUID& buildUUID) {
    auto multiIndexBlockPtr = _getBuilder(buildUUID);
    // TODO: verify that the index builder is in the expected state.

    // TODO: Not yet implemented.

    return Status::OK();
}

Status IndexBuildsManager::finishConstraintPhase(const UUID& buildUUID) {
    auto multiIndexBlockPtr = _getBuilder(buildUUID);
    // TODO: verify that the index builder is in the expected state.

    // TODO: Not yet implemented.

    return Status::OK();
}

Status IndexBuildsManager::commitIndexBuild(const UUID& buildUUID) {
    auto multiIndexBlockPtr = _getBuilder(buildUUID);
    // TODO: verify that the index builder is in the expected state.

    // TODO: Not yet implemented.

    return Status::OK();
}

bool IndexBuildsManager::abortIndexBuild(const UUID& buildUUID, const std::string& reason) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    if (builderIt == _builders.end()) {
        return false;
    }
    builderIt->second->abort(reason);
    return true;
}

bool IndexBuildsManager::interruptIndexBuild(const UUID& buildUUID, const std::string& reason) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    if (builderIt == _builders.end()) {
        return false;
    }

    // TODO: Not yet implemented.
    return true;
}

void IndexBuildsManager::tearDownIndexBuild(const UUID& buildUUID) {
    // TODO verify that the index builder is in a finished state before allowing its destruction.
    _unregisterIndexBuild(buildUUID);
}

void IndexBuildsManager::verifyNoIndexBuilds_forTestOnly() {
    invariant(_builders.empty());
}

void IndexBuildsManager::_registerIndexBuild(OperationContext* opCtx,
                                             Collection* collection,
                                             UUID buildUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    std::shared_ptr<MultiIndexBlockImpl> mib =
        std::make_shared<MultiIndexBlockImpl>(opCtx, collection);
    invariant(_builders.insert(std::make_pair(buildUUID, mib)).second);
}

void IndexBuildsManager::_unregisterIndexBuild(const UUID& buildUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    invariant(builderIt != _builders.end());
    _builders.erase(builderIt);
}

std::shared_ptr<MultiIndexBlock> IndexBuildsManager::_getBuilder(const UUID& buildUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto builderIt = _builders.find(buildUUID);
    invariant(builderIt != _builders.end());
    return builderIt->second;
}

}  // namespace mongo
