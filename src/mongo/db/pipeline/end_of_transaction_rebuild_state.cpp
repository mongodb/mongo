// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/end_of_transaction_rebuild_state.h"

namespace mongo::change_stream {

const OperationContext::Decoration<EndOfTransactionRebuildState> kEndOfTransactionRebuildState =
    OperationContext::declareDecoration<EndOfTransactionRebuildState>();

}  // namespace mongo::change_stream
