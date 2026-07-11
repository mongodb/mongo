// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Allows reading from the latest durable timestamp on replicated collections on secondaries.
 *
 * Resets to original value when leaving scope.
 */
class [[MONGO_MOD_PUBLIC]] AllowReadFromLatestOnSecondaryBlock_UNSAFE {
    AllowReadFromLatestOnSecondaryBlock_UNSAFE(const AllowReadFromLatestOnSecondaryBlock_UNSAFE&) =
        delete;
    AllowReadFromLatestOnSecondaryBlock_UNSAFE& operator=(
        const AllowReadFromLatestOnSecondaryBlock_UNSAFE&) = delete;

public:
    AllowReadFromLatestOnSecondaryBlock_UNSAFE(OperationContext* opCtx);
    ~AllowReadFromLatestOnSecondaryBlock_UNSAFE();

private:
    OperationContext* const _opCtx;
    const bool _initialState;
};

[[MONGO_MOD_PUBLIC]] bool allowReadFromLatestOnSecondary(const OperationContext* opCtx);

};  // namespace mongo
