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

#include "mongo/db/session/sessions_collection_mock.h"

#include <functional>
#include <mutex>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>

namespace mongo {

MockSessionsCollectionImpl::MockSessionsCollectionImpl()
    : _refresh([=, this](const LogicalSessionRecordSet& sessions) { _refreshSessions(sessions); }),
      _remove([=, this](const LogicalSessionIdSet& sessions) { _removeRecords(sessions); }) {}

void MockSessionsCollectionImpl::setRefreshHook(RefreshHook hook) {
    _refresh = std::move(hook);
}

void MockSessionsCollectionImpl::setRemoveHook(RemoveHook hook) {
    _remove = std::move(hook);
}

void MockSessionsCollectionImpl::clearHooks() {
    _refresh = [=, this](const LogicalSessionRecordSet& sessions) {
        _refreshSessions(sessions);
    };
    _remove = [=, this](const LogicalSessionIdSet& sessions) {
        _removeRecords(sessions);
    };
}

void MockSessionsCollectionImpl::refreshSessions(const LogicalSessionRecordSet& sessions) {
    _refresh(sessions);
}

void MockSessionsCollectionImpl::removeRecords(const LogicalSessionIdSet& sessions) {
    _remove(std::move(sessions));
}

void MockSessionsCollectionImpl::add(LogicalSessionRecord record) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _sessions.insert({record.getId(), std::move(record)});
}

void MockSessionsCollectionImpl::remove(LogicalSessionId lsid) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _sessions.erase(lsid);
}

bool MockSessionsCollectionImpl::has(LogicalSessionId lsid) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _sessions.find(lsid) != _sessions.end();
}

void MockSessionsCollectionImpl::clearSessions() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _sessions.clear();
}

const MockSessionsCollectionImpl::SessionMap& MockSessionsCollectionImpl::sessions() const {
    return _sessions;
}

void MockSessionsCollectionImpl::_refreshSessions(const LogicalSessionRecordSet& sessions) {
    for (auto& record : sessions) {
        if (!has(record.getId())) {
            _sessions.insert({record.getId(), record});
        }
    }
}

void MockSessionsCollectionImpl::_removeRecords(const LogicalSessionIdSet& sessions) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    for (auto& lsid : sessions) {
        _sessions.erase(lsid);
    }
}

LogicalSessionIdSet MockSessionsCollectionImpl::findRemovedSessions(
    OperationContext* opCtx, const LogicalSessionIdSet& sessions) {
    LogicalSessionIdSet lsids;
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    for (auto& lsid : sessions) {
        if (_sessions.find(lsid) == _sessions.end()) {
            lsids.emplace(lsid);
        }
    }
    return lsids;
}

}  // namespace mongo
