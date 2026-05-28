/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/shard_role/shard_catalog/shard_catalog_history_cleanup_control.h"

#include "mongo/util/decorable.h"

namespace mongo {

namespace {

const auto serviceDecoration =
    ServiceContext::declareDecoration<ShardCatalogHistoryCleanupControl>();

}  // namespace


ShardCatalogHistoryCleanupRunGuard::~ShardCatalogHistoryCleanupRunGuard() {
    if (_ctrl) {
        _ctrl->_releaseRun();
    }
}

ShardCatalogHistoryCleanupControl& ShardCatalogHistoryCleanupControl::get(
    ServiceContext* serviceContext) {
    return serviceDecoration(serviceContext);
}

ShardCatalogHistoryCleanupControl& ShardCatalogHistoryCleanupControl::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void ShardCatalogHistoryCleanupControl::pause() {
    std::unique_lock<std::mutex> lock(_mutex);
    _paused = true;
    _cv.wait(lock, [this] { return !_running; });
}

void ShardCatalogHistoryCleanupControl::resume() {
    std::unique_lock<std::mutex> lock(_mutex);
    _paused = false;
}

ShardCatalogHistoryCleanupRunGuard ShardCatalogHistoryCleanupControl::tryAcquireRun() {
    std::unique_lock<std::mutex> lock(_mutex);
    if (_paused) {
        return ShardCatalogHistoryCleanupRunGuard{};
    }
    _running = true;
    return ShardCatalogHistoryCleanupRunGuard{this};
}

void ShardCatalogHistoryCleanupControl::_releaseRun() {
    std::lock_guard<std::mutex> lock(_mutex);
    _running = false;
    _cv.notify_all();
}
}  // namespace mongo
