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

#pragma once

#include "mongo/db/logical_session_id.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {

/**
 * To allow us to move a MockSessionCollection into the session cache while
 * maintaining a hold on it from within our unit tests, the MockSessionCollection
 * will have an internal pointer to a MockSessionsCollectionImpl object that the
 * test creates and controls.
 *
 * This class can operate in two modes:
 *
 * - if no custom hooks are set, then the test caller may add() and remove() items
 *   from the _sessions map, and this class will simply perform the required
 *   operations against that set.
 *
 * - if custom hooks are set, then the hooks will be run instead of the provided
 *   defaults, and the internal _sessions map will NOT be updated.
 */
class MockSessionsCollectionImpl {
public:
    using SessionMap =
        stdx::unordered_map<LogicalSessionId, LogicalSessionRecord, LogicalSessionIdHash>;

    MockSessionsCollectionImpl();

    using FetchHook = stdx::function<StatusWith<LogicalSessionRecord>(LogicalSessionId)>;
    using InsertHook = stdx::function<Status(LogicalSessionRecord)>;
    using RefreshHook = stdx::function<LogicalSessionIdSet(LogicalSessionIdSet)>;
    using RemoveHook = stdx::function<void(LogicalSessionIdSet)>;

    // Set custom hooks to override default behavior
    void setFetchHook(FetchHook hook);
    void setInsertHook(InsertHook hook);
    void setRefreshHook(RefreshHook hook);
    void setRemoveHook(RemoveHook hook);

    // Reset all hooks to their defaults
    void clearHooks();

    // Forwarding methods from the MockSessionsCollection
    StatusWith<LogicalSessionRecord> fetchRecord(LogicalSessionId id);
    Status insertRecord(LogicalSessionRecord record);
    LogicalSessionIdSet refreshSessions(LogicalSessionIdSet sessions);
    void removeRecords(LogicalSessionIdSet sessions);

    // Test-side methods that operate on the _sessions map
    void add(LogicalSessionRecord record);
    void remove(LogicalSessionId lsid);
    bool has(LogicalSessionId lsid);
    void clearSessions();
    const SessionMap& sessions() const;

private:
    // Default implementations, may be overridden with custom hooks.
    StatusWith<LogicalSessionRecord> _fetchRecord(LogicalSessionId id);
    Status _insertRecord(LogicalSessionRecord record);
    LogicalSessionIdSet _refreshSessions(LogicalSessionIdSet sessions);
    void _removeRecords(LogicalSessionIdSet sessions);

    stdx::mutex _mutex;
    SessionMap _sessions;

    FetchHook _fetch;
    InsertHook _insert;
    RefreshHook _refresh;
    RemoveHook _remove;
};

/**
 * To allow us to move this into the session cache while maintaining a hold
 * on it from the test side, the MockSessionCollection will have an internal pointer
 * to an impl that we maintain access to.
 */
class MockSessionsCollection : public SessionsCollection {
public:
    explicit MockSessionsCollection(std::shared_ptr<MockSessionsCollectionImpl> impl)
        : _impl(std::move(impl)) {}

    StatusWith<LogicalSessionRecord> fetchRecord(LogicalSessionId id) override {
        return _impl->fetchRecord(std::move(id));
    }

    Status insertRecord(LogicalSessionRecord record) override {
        return _impl->insertRecord(std::move(record));
    }

    LogicalSessionIdSet refreshSessions(LogicalSessionIdSet sessions) override {
        return _impl->refreshSessions(std::move(sessions));
    }

    void removeRecords(LogicalSessionIdSet sessions) override {
        return _impl->removeRecords(std::move(sessions));
    }

private:
    std::shared_ptr<MockSessionsCollectionImpl> _impl;
};

}  // namespace mongo
