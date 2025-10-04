/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/local_catalog/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_id.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/future.h"

#include <string>

namespace mongo {

class DirectConnectionDDLHook : public ReplicaSetDDLHook {
public:
    inline static const std::string kDirectConnectionDDLHookName = "DirectConnectionDDLHook";

    DirectConnectionDDLHook(const DirectConnectionDDLHook&) = delete;
    DirectConnectionDDLHook& operator=(const DirectConnectionDDLHook&) = delete;
    DirectConnectionDDLHook() = default;
    ~DirectConnectionDDLHook() override = default;

    static void create(ServiceContext* serviceContext);

    inline StringData getName() const final {
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
    mutable stdx::mutex _mutex;
    // Tracks the operation ID plus a counter of how many times register has been called for this
    // operation. This allows us to handle multiple calls to onBegin and onEnd for the same op.
    stdx::unordered_map<OperationId, int> _ongoingOps;
    boost::optional<SharedPromise<void>> _drainingPromise;
};
}  // namespace mongo
