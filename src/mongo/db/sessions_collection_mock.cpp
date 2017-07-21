/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/sessions_collection_mock.h"
#include "mongo/platform/basic.h"
#include "mongo/stdx/functional.h"

namespace mongo {

MockSessionsCollectionImpl::MockSessionsCollectionImpl()
    : _sessions(),
      _fetch(stdx::bind(&MockSessionsCollectionImpl::_fetchRecord, this, stdx::placeholders::_1)),
      _insert(stdx::bind(&MockSessionsCollectionImpl::_insertRecord, this, stdx::placeholders::_1)),
      _refresh(
          stdx::bind(&MockSessionsCollectionImpl::_refreshSessions, this, stdx::placeholders::_1)),
      _remove(
          stdx::bind(&MockSessionsCollectionImpl::_removeRecords, this, stdx::placeholders::_1)) {}

void MockSessionsCollectionImpl::setFetchHook(FetchHook hook) {
    _fetch = std::move(hook);
}

void MockSessionsCollectionImpl::setInsertHook(InsertHook hook) {
    _insert = std::move(hook);
}

void MockSessionsCollectionImpl::setRefreshHook(RefreshHook hook) {
    _refresh = std::move(hook);
}

void MockSessionsCollectionImpl::setRemoveHook(RemoveHook hook) {
    _remove = std::move(hook);
}

void MockSessionsCollectionImpl::clearHooks() {
    _fetch = stdx::bind(&MockSessionsCollectionImpl::_fetchRecord, this, stdx::placeholders::_1);
    _insert = stdx::bind(&MockSessionsCollectionImpl::_insertRecord, this, stdx::placeholders::_1);
    _refresh =
        stdx::bind(&MockSessionsCollectionImpl::_refreshSessions, this, stdx::placeholders::_1);
    _remove = stdx::bind(&MockSessionsCollectionImpl::_removeRecords, this, stdx::placeholders::_1);
}

StatusWith<LogicalSessionRecord> MockSessionsCollectionImpl::fetchRecord(LogicalSessionId id) {
    return _fetch(std::move(id));
}

Status MockSessionsCollectionImpl::insertRecord(LogicalSessionRecord record) {
    return _insert(std::move(record));
}

LogicalSessionIdSet MockSessionsCollectionImpl::refreshSessions(LogicalSessionIdSet sessions) {
    return _refresh(std::move(sessions));
}

void MockSessionsCollectionImpl::removeRecords(LogicalSessionIdSet sessions) {
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

StatusWith<LogicalSessionRecord> MockSessionsCollectionImpl::_fetchRecord(LogicalSessionId id) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // If we do not have this record, return an error
    auto it = _sessions.find(id);
    if (it == _sessions.end()) {
        return {ErrorCodes::NoSuchSession, "No matching record in the sessions collection"};
    }

    return it->second;
}

Status MockSessionsCollectionImpl::_insertRecord(LogicalSessionRecord record) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto res = _sessions.insert({record.getId(), std::move(record)});

    // We should never try to insert the same record twice. In theory this could
    // happen because of a UUID conflict.
    if (!res.second) {
        return {ErrorCodes::DuplicateSession, "Session already exists in the sessions collection"};
    }

    return Status::OK();
}

LogicalSessionIdSet MockSessionsCollectionImpl::_refreshSessions(LogicalSessionIdSet sessions) {
    LogicalSessionIdSet notFound{};

    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        for (auto& lsid : sessions) {
            auto it = _sessions.find(lsid);
            if (it == _sessions.end()) {
                notFound.insert(lsid);
            }
        }
    }

    return notFound;
}

void MockSessionsCollectionImpl::_removeRecords(LogicalSessionIdSet sessions) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    for (auto& lsid : sessions) {
        _sessions.erase(lsid);
    }
}

}  // namespace mongo
