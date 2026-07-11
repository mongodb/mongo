// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/allow_read_from_latest_on_secondary.h"

#include "mongo/util/decorable.h"

namespace mongo {
const OperationContext::Decoration<bool> allowReadFromLatestOnSecondaryOnOpCtx =
    OperationContext::declareDecoration<bool>();

AllowReadFromLatestOnSecondaryBlock_UNSAFE::AllowReadFromLatestOnSecondaryBlock_UNSAFE(
    OperationContext* opCtx)
    : _opCtx(opCtx), _initialState(allowReadFromLatestOnSecondaryOnOpCtx(opCtx)) {
    allowReadFromLatestOnSecondaryOnOpCtx(opCtx) = true;
}

AllowReadFromLatestOnSecondaryBlock_UNSAFE::~AllowReadFromLatestOnSecondaryBlock_UNSAFE() {
    allowReadFromLatestOnSecondaryOnOpCtx(_opCtx) = _initialState;
}

bool allowReadFromLatestOnSecondary(const OperationContext* opCtx) {
    return allowReadFromLatestOnSecondaryOnOpCtx(opCtx);
}

};  // namespace mongo
