// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/generic_cursor_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/platform/random.h"
#include "mongo/util/modules.h"

#include <functional>

namespace mongo::generic_cursor {

/**
 * Allocates a positive CursorId that satisfies 'pred', which checks that the CursorId is not
 * already in use.
 *
 * The caller of this function is responsible for synchronization between the check of whether a
 * cursor is already allocated in 'pred' and the creation of new cursors.
 */
CursorId allocateCursorId(const std::function<bool(CursorId)>& pred, PseudoRandom& random);

/**
 * Cursors in a session can kill other session's cursors. Cursors in a transaction can't.
 */
void validateKillInTransaction(OperationContext* opCtx,
                               CursorId cursorId,
                               boost::optional<LogicalSessionId> lsid,
                               boost::optional<TxnNumber> txnNumber);

}  // namespace mongo::generic_cursor
