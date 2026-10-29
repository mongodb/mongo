// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>

namespace mongo {

class StorageEngineChangeContext {
public:
    static StorageEngineChangeContext* get(ServiceContext* service);

    /**
     * Start to change the storage engine for the associated ServiceContext.  This will kill all
     * OperationContexts that have a non-noop Recovery Unit with an InterruptedDueToStorageChange
     * code, free the existing storage engine, and block any new operation contexts from being
     * created while the returned lock is in scope.
     */
    WriteRarelyRWMutex::WriteLock killOpsForStorageEngineChange(ServiceContext* service);

    /**
     * Finish changing the storage engine for the associated ServiceContext.  This will change the
     * storage engine and allow operation contexts to again be created.
     */
    void changeStorageEngine(ServiceContext* service,
                             WriteRarelyRWMutex::WriteLock,
                             std::unique_ptr<StorageEngine> engine);

    /**
     * Called by the decorator's destructor to tell us that an opCtx with the old storage engine has
     * been destroyed.
     */
    void notifyOpCtxDestroyed() noexcept;

private:
    std::mutex _mutex;

    // Keeps track of opCtxs associated with a storage engine that is being replaced.
    // Protected by _mutex
    int _numOpCtxtsToWaitFor = 0;
    stdx::condition_variable _allOldStorageOperationContextsReleased;
};
}  // namespace mongo
