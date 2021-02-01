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

#include <boost/optional.hpp>

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/executor/task_executor.h"

namespace mongo {

/**
 * Tenant access blocking interface used by TenantMigrationDonorAccessBlocker.
 */
class TenantMigrationAccessBlocker {
public:
    TenantMigrationAccessBlocker() = default;
    virtual ~TenantMigrationAccessBlocker() = default;

    /**
     * The operation type determines the states during which we need to block.
     */
    enum OperationType { kWrite, kIndexBuild };

    //
    // Called by all writes and reads against the database.
    //

    virtual void checkIfCanWriteOrThrow() = 0;
    virtual Status waitUntilCommittedOrAborted(OperationContext* opCtx,
                                               OperationType operationType) = 0;

    virtual void checkIfLinearizableReadWasAllowedOrThrow(OperationContext* opCtx) = 0;
    virtual SharedSemiFuture<void> getCanReadFuture(OperationContext* opCtx) = 0;

    //
    // Called by index build user threads before acquiring an index build slot, and again right
    // after registering the build.
    //
    virtual Status checkIfCanBuildIndex() = 0;

    /**
     * If the given opTime is the commit or abort opTime and the completion promise has not been
     * fulfilled, calls _onMajorityCommitCommitOpTime or _onMajorityCommitAbortOpTime to transition
     * out of blocking and fulfill the promise.
     */
    virtual void onMajorityCommitPointUpdate(repl::OpTime opTime) = 0;

    virtual std::shared_ptr<executor::TaskExecutor> getAsyncBlockingOperationsExecutor() = 0;

    virtual void appendInfoForServerStatus(BSONObjBuilder* builder) const = 0;

    // Returns structured info with current tenant ID and connection string.
    virtual BSONObj getDebugInfo() const = 0;
};

}  // namespace mongo
