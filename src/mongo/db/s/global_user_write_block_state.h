/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

class GlobalUserWriteBlockState {
public:
    GlobalUserWriteBlockState() = default;

    static GlobalUserWriteBlockState* get(ServiceContext* serviceContext);
    static GlobalUserWriteBlockState* get(OperationContext* opCtx);

    /**
     * Methods to control the global user write blocking state.
     */
    void enableUserWriteBlocking(OperationContext* opCtx);
    void disableUserWriteBlocking(OperationContext* opCtx);

    /**
     * Checks that user writes are allowed on the specified namespace. Callers must hold the
     * GlobalLock in any mode. Throws UserWritesBlocked if user writes are disallowed.
     */
    void checkUserWritesAllowed(OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Returns whether user write blocking is enabled, disregarding a specific namespace and the
     * state of WriteBlockBypass. Used for serverStatus.
     */
    bool isUserWriteBlockingEnabled(OperationContext* opCtx) const;

    /**
     * Methods to enable/disable blocking new sharded DDL operations.
     */
    void enableUserShardedDDLBlocking(OperationContext* opCtx);
    void disableUserShardedDDLBlocking(OperationContext* opCtx);

    /**
     * Checks that new sharded DDL operations are allowed to start. Throws UserWritesBlocked if
     * starting new sharded DDL operations is disallowed.
     */
    void checkShardedDDLAllowedToStart(OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Methods to enable/disable blocking new user index builds.
     */
    void enableUserIndexBuildBlocking(OperationContext* opCtx);
    void disableUserIndexBuildBlocking(OperationContext* opCtx);

    /**
     * Checks that an index build is allowed to start on the specified namespace. Returns
     * UserWritesBlocked if user index builds are disallowed, OK otherwise.
     */
    Status checkIfIndexBuildAllowedToStart(OperationContext* opCtx,
                                           const NamespaceString& nss) const;


private:
    AtomicWord<bool> _globalUserWritesBlocked{false};
    AtomicWord<bool> _userShardedDDLBlocked{false};
    AtomicWord<bool> _userIndexBuildsBlocked{false};
};

}  // namespace mongo
