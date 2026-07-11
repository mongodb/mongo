// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/transaction/retryable_write_util.h"

#include "mongo/db/transaction/transaction_participant.h"

namespace mongo::retryable_write_util {

bool isRetryableWrite(OperationContext* opCtx) {
    if (!opCtx->writesAreReplicated() || !opCtx->isRetryableWrite()) {
        return false;
    }
    auto txnParticipant = TransactionParticipant::get(opCtx);
    return txnParticipant &&
        (!opCtx->inMultiDocumentTransaction() || txnParticipant.transactionIsOpen());
}

}  // namespace mongo::retryable_write_util
