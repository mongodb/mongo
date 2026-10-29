// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/always_allow_non_local_writes.h"

#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {
namespace repl {

// Overrides _canAcceptLocalWrites for the decorated OperationContext.
const OperationContext::Decoration<bool> alwaysAllowNonLocalWritesOnOpCtx =
    OperationContext::declareDecoration<bool>();

AllowNonLocalWritesBlock::AllowNonLocalWritesBlock(OperationContext* opCtx)
    : _opCtx(opCtx), _initialState(alwaysAllowNonLocalWritesOnOpCtx(_opCtx)) {
    alwaysAllowNonLocalWritesOnOpCtx(_opCtx) = true;
}

AllowNonLocalWritesBlock::~AllowNonLocalWritesBlock() {
    alwaysAllowNonLocalWritesOnOpCtx(_opCtx) = _initialState;
}

bool alwaysAllowNonLocalWrites(const OperationContext* opCtx) {
    return alwaysAllowNonLocalWritesOnOpCtx(opCtx);
}

}  // namespace repl
}  // namespace mongo
