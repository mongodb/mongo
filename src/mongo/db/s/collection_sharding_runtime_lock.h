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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/stdx/variant.h"

namespace mongo {

class CollectionShardingState;

/**
 * RAII-style class that locks the CollectionShardingRuntime using the CollectionShardingRuntime's
 * ResourceMutex. The lock will be created and acquired on construction. The lock will be dismissed
 * upon destruction of the CollectionShardingRuntimeLock object.
 *
 * The CollectionShardingRuntimeLock currently ensures concurrent access to the following sections:
 *
 * - The collection's critical section
 * - Attaching and detaching the MigrationSoourceManager from the CollectionShardingRuntime and
 *   accessing the MigrationSourceManager from the CollectionShardingRuntime.
 */
class CollectionShardingRuntimeLock {

public:
    using CSRLock = stdx::variant<Lock::SharedLock, Lock::ExclusiveLock>;

    /**
     * Locks the sharding runtime state for the specified collection with the
     * CollectionShardingRuntime object's ResourceMutex in MODE_IS. When the object goes out of
     * scope, the ResourceMutex will be unlocked.
     */
    static CollectionShardingRuntimeLock lock(OperationContext* opCtx,
                                              CollectionShardingState* csr);

    /**
     * Follows the same functionality as the CollectionShardingRuntimeLock lock method, except
     * that lockExclusive takes the ResourceMutex in MODE_X.
     */
    static CollectionShardingRuntimeLock lockExclusive(OperationContext* opCtx,
                                                       CollectionShardingState* csr);

private:
    CollectionShardingRuntimeLock(OperationContext* opCtx,
                                  CollectionShardingState* csr,
                                  LockMode lockMode);

    // The lock created and locked upon construction of a CollectionShardingRuntimeLock object.
    // It locks the ResourceMutex taken from the CollectionShardingRuntime class, passed in on
    // construction.
    CSRLock _lock;
};

}  // namespace mongo