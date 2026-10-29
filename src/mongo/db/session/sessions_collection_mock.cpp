// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/session/sessions_collection_mock.h"

#include <functional>
#include <mutex>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>

namespace mongo {

MockSessionsCollectionImpl::MockSessionsCollectionImpl()
    : _refresh(
          [=, this](const LogicalSessionRecordSet& sessions)
              -> SessionsCollection::RefreshSessionsResult { return _refreshSessions(sessions); }),
      _remove([=, this](const LogicalSessionIdSet& sessions) { _removeRecords(sessions); }) {}

void MockSessionsCollectionImpl::setRefreshHook(RefreshHook hook) {
    _refresh = std::move(hook);
}

void MockSessionsCollectionImpl::setRemoveHook(RemoveHook hook) {
    _remove = std::move(hook);
}

void MockSessionsCollectionImpl::clearHooks() {
    _refresh =
        [=, this](
            const LogicalSessionRecordSet& sessions) -> SessionsCollection::RefreshSessionsResult {
        return _refreshSessions(sessions);
    };
    _remove = [=, this](const LogicalSessionIdSet& sessions) {
        _removeRecords(sessions);
    };
}

SessionsCollection::RefreshSessionsResult MockSessionsCollectionImpl::refreshSessions(
    const LogicalSessionRecordSet& sessions) {
    return _refresh(sessions);
}

void MockSessionsCollectionImpl::removeRecords(const LogicalSessionIdSet& sessions) {
    _remove(std::move(sessions));
}

void MockSessionsCollectionImpl::add(LogicalSessionRecord record) {
    std::unique_lock<std::mutex> lk(_mutex);
    _sessions.insert({record.getId(), std::move(record)});
}

void MockSessionsCollectionImpl::remove(LogicalSessionId lsid) {
    std::unique_lock<std::mutex> lk(_mutex);
    _sessions.erase(lsid);
}

bool MockSessionsCollectionImpl::has(LogicalSessionId lsid) {
    std::unique_lock<std::mutex> lk(_mutex);
    return _sessions.find(lsid) != _sessions.end();
}

void MockSessionsCollectionImpl::clearSessions() {
    std::unique_lock<std::mutex> lk(_mutex);
    _sessions.clear();
}

const MockSessionsCollectionImpl::SessionMap& MockSessionsCollectionImpl::sessions() const {
    return _sessions;
}

SessionsCollection::RefreshSessionsResult MockSessionsCollectionImpl::_refreshSessions(
    const LogicalSessionRecordSet& sessions) {
    for (auto& record : sessions) {
        if (!has(record.getId())) {
            _sessions.insert({record.getId(), record});
        }
    }
    return {};
}

void MockSessionsCollectionImpl::_removeRecords(const LogicalSessionIdSet& sessions) {
    std::unique_lock<std::mutex> lk(_mutex);
    for (auto& lsid : sessions) {
        _sessions.erase(lsid);
    }
}

LogicalSessionIdSet MockSessionsCollectionImpl::findRemovedSessions(
    OperationContext* opCtx, const LogicalSessionIdSet& sessions) {
    LogicalSessionIdSet lsids;
    std::unique_lock<std::mutex> lk(_mutex);
    for (auto& lsid : sessions) {
        if (_sessions.find(lsid) == _sessions.end()) {
            lsids.emplace(lsid);
        }
    }
    return lsids;
}

}  // namespace mongo
