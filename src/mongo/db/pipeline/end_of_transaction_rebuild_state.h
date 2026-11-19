// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo::change_stream {

/**
 * Per-OperationContext state the router uses while it transparently rebuilds a change stream after
 * an end-of-transaction lookback. Grouped into a single decoration because each additional
 * decoration adds fixed per-request overhead to every OperationContext.
 *
 * Kept in its own low-level header so both the change stream pipeline helpers and the router
 * cursor manager can share the decoration without a dependency cycle.
 */
struct EndOfTransactionRebuildState {
    // Set while the router internally rebuilds a change stream after an end-of-transaction
    // lookback. Authorizes the internal-only 'backfillingStartAtOperationTime' field, which is
    // otherwise rejected for consumer input. Set on both the aggregate and getMore rebuild paths.
    bool rebuildInProgress = false;

    // Set only when the router rebuilds a change stream inside a getMore. The rebuilt aggregate
    // runs nested on the getMore's OperationContext and must not collect query stats or fold
    // metrics into the rebuilt cursor; the enclosing getMore records them once.
    bool rebuildInGetMore = false;

    // Number of kickback-driven end-of-transaction rebuilds this stream has performed. Seeded into
    // the registered cursor so that 'changeStreamEndOfTransactionMaxRebuilds' is a per-stream
    // lifetime budget shared across commands, not restarted per command.
    int rebuildCount = 0;

    // Start point resolved on the first attempt. A brand-new stream resolves its start point to
    // "now" only in the parsed stage's local spec; a retry after an end-of-transaction kickback
    // must reuse it, or it would re-resolve a later "now" and suppress the discarded first batch's
    // events.
    boost::optional<Timestamp> resolvedStartPoint;

    // The original aggregate command of a stream being rebuilt inside a getMore. The nested
    // aggregate runs on the getMore's OperationContext, whose 'CurOp::opDescription()' is the
    // getMore, so without this the replacement cursor would record the getMore as its originating
    // command and a later rebuild could not reparse it as an aggregate.
    BSONObj originatingCommand;

    // Cursor id the next registration must use instead of allocating a fresh one. Set only during
    // an end-of-transaction rebuild (see 'setForcedCursorIdForEndOfTransactionRebuild').
    boost::optional<CursorId> forcedCursorId;
};

extern const OperationContext::Decoration<EndOfTransactionRebuildState>
    kEndOfTransactionRebuildState;

}  // namespace mongo::change_stream
