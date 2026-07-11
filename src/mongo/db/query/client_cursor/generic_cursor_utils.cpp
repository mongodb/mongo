// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/client_cursor/generic_cursor_utils.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

#include <cstdlib>
#include <limits>

namespace mongo::generic_cursor {

CursorId allocateCursorId(const std::function<bool(CursorId)>& pred, PseudoRandom& random) {
    for (int i = 0; i < 10000; i++) {
        CursorId id = random.nextInt64();

        // A cursor id of zero is reserved to indicate that the cursor has been closed. If the
        // random number generator gives us zero, then try again.
        if (id == 0) {
            continue;
        }

        // Avoid negative cursor ids by taking the absolute value. If the cursor id is the minimum
        // representable negative number, then just generate another random id.
        if (id == std::numeric_limits<CursorId>::min()) {
            continue;
        }
        id = std::abs(id);

        if (pred(id)) {
            // The cursor id is not already in use, so return it.
            return id;
        }

        // The cursor id is already in use. Generate another random id.
    }

    // We failed to generate a unique cursor id.
    fassertFailed(17360);
}

void validateKillInTransaction(OperationContext* opCtx,
                               CursorId cursorId,
                               boost::optional<LogicalSessionId> lsid,
                               boost::optional<TxnNumber> txnNumber) {
    if (opCtx->inMultiDocumentTransaction()) {
        uassert(8912345,
                str::stream() << "tried to kill a cursor " << cursorId << " belonging to session "
                              << lsid << " while in txn " << txnNumber << " of session "
                              << opCtx->getLogicalSessionId(),
                lsid == opCtx->getLogicalSessionId());
        uassert(8912321,
                str::stream() << "tried to kill a cursor " << cursorId << " belonging to txn "
                              << txnNumber << " while in txn " << opCtx->getTxnNumber()
                              << " of session " << opCtx->getLogicalSessionId(),
                txnNumber == opCtx->getTxnNumber());
    }
}

}  // namespace mongo::generic_cursor
