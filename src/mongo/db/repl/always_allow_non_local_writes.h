// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace repl {

/**
 * Allows non-local writes despite _canAcceptNonLocalWrites being false on a single OperationContext
 * while in scope.
 *
 * Resets to original value when leaving scope so it is safe to nest.
 */
class [[MONGO_MOD_PUBLIC]] AllowNonLocalWritesBlock {
    AllowNonLocalWritesBlock(const AllowNonLocalWritesBlock&) = delete;
    AllowNonLocalWritesBlock& operator=(const AllowNonLocalWritesBlock&) = delete;

public:
    AllowNonLocalWritesBlock(OperationContext* opCtx);
    ~AllowNonLocalWritesBlock();

private:
    OperationContext* const _opCtx;
    const bool _initialState;
};

[[MONGO_MOD_PUBLIC]] bool alwaysAllowNonLocalWrites(const OperationContext* opCtx);

}  // namespace repl
}  // namespace mongo
