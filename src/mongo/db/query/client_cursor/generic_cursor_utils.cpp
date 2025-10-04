/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
