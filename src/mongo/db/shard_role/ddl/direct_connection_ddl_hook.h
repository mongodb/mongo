// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <mutex>
#include <string>
#include <string_view>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] DirectConnectionDDLHook : public ReplicaSetDDLHook {
public:
    inline static const std::string kDirectConnectionDDLHookName = "DirectConnectionDDLHook";

    DirectConnectionDDLHook(const DirectConnectionDDLHook&) = delete;
    DirectConnectionDDLHook& operator=(const DirectConnectionDDLHook&) = delete;
    DirectConnectionDDLHook() = default;
    ~DirectConnectionDDLHook() override = default;

    static void create(ServiceContext* serviceContext);

    inline std::string_view getName() const final {
        return kDirectConnectionDDLHookName;
    }

    /**
     * Registers this operation (identified by the opId on the opCtx) as ongoing. Ignores operations
     * over the `config.system.sessions` namespace.
     */
    void onBeginDDL(OperationContext* opCtx, const std::vector<NamespaceString>& namespaces) final;

    /**
     * If this operation was registered at start time, unregisters it. If this is the last operation
     * ongoing, fulfills the promise for tracking when operations are fully drained.
     */
    void onEndDDL(OperationContext* opCtx, const std::vector<NamespaceString>& namespaces) final;

    /**
     * Checks if there are any ongoing operations and returns a ready future if not. If there are
     * ongoing operations, creates a promise and returns the paired future which will be fulfilled
     * when the last ongoing op completes.
     */
    SharedSemiFuture<void> getWaitForDrainedFuture(OperationContext* opCtx);

    /**
     * Returns the set of ongoing operation ids to be used for debugging purposes.
     */
    stdx::unordered_map<OperationId, int> getOngoingOperations() const;

private:
    mutable std::mutex _mutex;
    // Tracks the operation ID plus a counter of how many times register has been called for this
    // operation. This allows us to handle multiple calls to onBegin and onEnd for the same op.
    stdx::unordered_map<OperationId, int> _ongoingOps;
    boost::optional<SharedPromise<void>> _drainingPromise;
};
}  // namespace mongo
