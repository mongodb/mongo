// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/primary_driven/tearable_side_write_redo_state.h"

#include "mongo/util/decorable.h"

namespace mongo::index_builds::primary_driven {
namespace {
const auto getTearableSideWriteRedoStateDecoration =
    OperationContext::declareDecoration<TearableSideWriteRedoState>();
}  // namespace

TearableSideWriteRedoState& getTearableSideWriteRedoState(OperationContext* opCtx) {
    return getTearableSideWriteRedoStateDecoration(opCtx);
}

}  // namespace mongo::index_builds::primary_driven
