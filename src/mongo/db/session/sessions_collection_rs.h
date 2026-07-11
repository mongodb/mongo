// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <mutex>
#include <type_traits>

namespace mongo {

class OperationContext;

/**
 * Accesses the sessions collection for replica set members.
 */
class SessionsCollectionRS final : public SessionsCollection {
public:
    /**
     * Ensures that the sessions collection exists and has the proper indexes.
     */
    void setupSessionsCollection(OperationContext* opCtx) override;

    /**
     * Checks if the sessions collection exists and has the proper indexes.
     */
    void checkSessionsCollectionExists(OperationContext* opCtx) override;

    /**
     * Updates the last-use times on the given sessions to be greater than
     * or equal to the current time.
     *
     * If a step-down happens on this node as this method is running, it may fail.
     */
    RefreshSessionsResult refreshSessions(OperationContext* opCtx,
                                          const LogicalSessionRecordSet& sessions) override;

    /**
     * Removes the authoritative records for the specified sessions.
     *
     * If a step-down happens on this node as this method is running, it may fail.
     */
    void removeRecords(OperationContext* opCtx, const LogicalSessionIdSet& sessions) override;

    /**
     * Returns the subset of sessions from the given set that do not have entries
     * in the sessions collection.
     *
     * If a step-down happens on this node as this method is running, it may
     * return stale results.
     */
    LogicalSessionIdSet findRemovedSessions(OperationContext* opCtx,
                                            const LogicalSessionIdSet& sessions) override;

private:
    auto _makePrimaryConnection(OperationContext* opCtx);

    bool _isStandaloneOrPrimary(const NamespaceString& ns, OperationContext* opCtx);

    template <typename LocalCallback, typename RemoteCallback>
    struct CommonResult {
        using LocalReturnType = std::invoke_result_t<LocalCallback>;
        using RemoteReturnType = std::invoke_result_t<RemoteCallback, DBClientBase*>;
        static_assert(std::is_same_v<LocalReturnType, RemoteReturnType>,
                      "LocalCallback and RemoteCallback must have the same return type");

        using Type =
            std::conditional_t<std::is_void<LocalReturnType>::value, void, LocalReturnType>;
    };
    template <typename LocalCallback, typename RemoteCallback>
    using CommonResultT = typename CommonResult<LocalCallback, RemoteCallback>::Type;

    template <typename LocalCallback, typename RemoteCallback>
    CommonResultT<LocalCallback, RemoteCallback> _dispatch(const NamespaceString& ns,
                                                           OperationContext* opCtx,
                                                           LocalCallback&& localCallback,
                                                           RemoteCallback&& remoteCallback);

    std::mutex _mutex;
    std::unique_ptr<RemoteCommandTargeter> _targeter;
};

}  // namespace mongo
