// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_event_handler.h"

#include "mongo/logv2/log.h"

namespace mongo {

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWiredTiger

void WiredTigerEventHandler::setWtConnReady(WT_CONNECTION* conn) {
    std::unique_lock<std::mutex> lock(_mutex);
    _wtConn = conn;
    if (_activeReaders == 0 || conn) {
        return;
    }
    LOGV2(7003100,
          "WiredTiger connection close is waiting for active statistics readers to finish",
          "activeReaders"_attr = _activeReaders);
    _idleCondition.wait(lock, [this]() { return _activeReaders == 0; });
}

WT_CONNECTION* WiredTigerEventHandler::getStatsCollectionPermit() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_wtConn) {
        _activeReaders++;
        return _wtConn;
    }
    return nullptr;
}

void WiredTigerEventHandler::releaseStatsCollectionPermit() {
    std::unique_lock<std::mutex> lock(_mutex);
    _activeReaders--;
    if (_activeReaders == 0 && !_wtConn) {
        _idleCondition.notify_all();
        return;
    }
}
}  // namespace mongo
