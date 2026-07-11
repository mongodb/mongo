// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

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

    using RefreshHook =
        std::function<SessionsCollection::RefreshSessionsResult(const LogicalSessionRecordSet&)>;
    using RemoveHook = std::function<void(const LogicalSessionIdSet&)>;

    // Set custom hooks to override default behavior
    void setRefreshHook(RefreshHook hook);
    void setRemoveHook(RemoveHook hook);

    // Reset all hooks to their defaults
    void clearHooks();

    // Forwarding methods from the MockSessionsCollection
    SessionsCollection::RefreshSessionsResult refreshSessions(
        const LogicalSessionRecordSet& sessions);
    void removeRecords(const LogicalSessionIdSet& sessions);

    // Test-side methods that operate on the _sessions map
    void add(LogicalSessionRecord record);
    void remove(LogicalSessionId lsid);
    bool has(LogicalSessionId lsid);
    void clearSessions();
    const SessionMap& sessions() const;

    LogicalSessionIdSet findRemovedSessions(OperationContext* opCtx,
                                            const LogicalSessionIdSet& sessions);

private:
    // Default implementations, may be overridden with custom hooks.
    SessionsCollection::RefreshSessionsResult _refreshSessions(
        const LogicalSessionRecordSet& sessions);
    void _removeRecords(const LogicalSessionIdSet& sessions);

    std::mutex _mutex;
    SessionMap _sessions;

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

    void setupSessionsCollection(OperationContext* opCtx) override {}

    void checkSessionsCollectionExists(OperationContext* opCtx) override {}

    SessionsCollection::RefreshSessionsResult refreshSessions(
        OperationContext* opCtx, const LogicalSessionRecordSet& sessions) override {
        return _impl->refreshSessions(sessions);
    }

    void removeRecords(OperationContext* opCtx, const LogicalSessionIdSet& sessions) override {
        _impl->removeRecords(sessions);
    }

    LogicalSessionIdSet findRemovedSessions(OperationContext* opCtx,
                                            const LogicalSessionIdSet& sessions) override {
        return _impl->findRemovedSessions(opCtx, sessions);
    }

private:
    std::shared_ptr<MockSessionsCollectionImpl> _impl;
};

}  // namespace mongo
