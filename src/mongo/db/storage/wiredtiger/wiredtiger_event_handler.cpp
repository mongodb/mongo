/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_event_handler.h"

#include "mongo/logv2/log.h"

namespace mongo {

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWiredTiger

void WiredTigerEventHandler::setWtConnReady(WT_CONNECTION* conn) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
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
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (_wtConn) {
        _activeReaders++;
        return _wtConn;
    }
    return nullptr;
}

void WiredTigerEventHandler::releaseStatsCollectionPermit() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _activeReaders--;
    if (_activeReaders == 0 && !_wtConn) {
        _idleCondition.notify_all();
        return;
    }
}
}  // namespace mongo
