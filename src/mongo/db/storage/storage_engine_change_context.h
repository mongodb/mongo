/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <memory>
#include <shared_mutex>

#include "mongo/db/operation_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_change_lock.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_set.h"

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
    stdx::unique_lock<ServiceContext::StorageChangeMutexType> killOpsForStorageEngineChange(
        ServiceContext* service);

    /**
     * Finish changing the storage engine for the associated ServiceContext.  This will change the
     * storage engine and allow operation contexts to again be created.
     */
    void changeStorageEngine(ServiceContext* service,
                             stdx::unique_lock<ServiceContext::StorageChangeMutexType>,
                             std::unique_ptr<StorageEngine> engine);

    /**
     * Called by the decorator's destructor to tell us that an opCtx with the old storage engine has
     * been destroyed.
     */
    void notifyOpCtxDestroyed() noexcept;

private:
    Mutex _mutex = MONGO_MAKE_LATCH("StorageEngineChangeContext::_mutex");

    // Keeps track of opCtxs associated with a storage engine that is being replaced.
    // Protected by _mutex
    int _numOpCtxtsToWaitFor = 0;
    stdx::condition_variable _allOldStorageOperationContextsReleased;
};
}  // namespace mongo
