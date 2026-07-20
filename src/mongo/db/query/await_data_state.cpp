// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/await_data_state.h"

namespace mongo {

const OperationContext::Decoration<AwaitDataState> awaitDataState =
    OperationContext::declareDecoration<AwaitDataState>();

void setAwaitDataDeadline(OperationContext* opCtx, Date_t deadline) {
    awaitDataState(opCtx).waitForInsertsDeadline = deadline;
}

}  // namespace mongo
