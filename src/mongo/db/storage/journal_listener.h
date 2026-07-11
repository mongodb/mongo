// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {
/**
 * This class allows for the storageEngine to alert the rest of the system about journaled write
 * progress.
 *
 * It has two methods. The first, getToken(), returns a token representing the current progress
 * applied to the node. It should be called just prior to making writes durable (usually, syncing a
 * journal entry to disk).
 *
 * The second method, onDurable(), takes this token as an argument and relays to the rest of the
 * system that writes through that point have been journaled.
 */
class [[MONGO_MOD_OPEN]] JournalListener {
public:
    class [[MONGO_MOD_OPEN]] Token {
    public:
        virtual ~Token() = default;
    };

    /**
     * Optional hints to getToken implementations about the calling context. Used so the listener
     * can choose between paths that may or may not require Global IX (e.g. on a primary the
     * default path writes the oplog truncate-after point, which requires IX).
     */
    enum class TokenMode {
        // Default: implementations are free to take any path, including ones that require Global
        // IX (e.g. writing the oplog truncate-after point on a primary). This is the normal mode
        // used by JournalFlusher's periodic cycle.
        kDefault,

        // Caller already holds the global lock in a mode incompatible with IX (typically MODE_S,
        // as held by fsyncLockWorker). Implementations must avoid taking IX. In particular,
        // they must NOT write the oplog truncate-after point on a primary. They should still
        // produce a token so onDurable can advance durableOpTime in-memory. The skipped
        // truncate-after point write will be picked up by JournalFlusher's next cycle after the
        // lock is released.
        //
        // Crash safety: Global S blocks all oplog writers (which require IX), so no oplog entry
        // is partially in-flight when the journal flush runs. Everything up to lastWritten is
        // fully formed on disk after the flush. Deferring the truncate-after-point write does
        // not create a crash-safety gap: on recovery, those entries are intact in the journal
        // and will not be incorrectly truncated.
        kReadLockHeld,
    };

    virtual ~JournalListener() = default;
    virtual std::unique_ptr<Token> getToken(OperationContext* opCtx,
                                            TokenMode mode = TokenMode::kDefault) = 0;
    virtual void onDurable(const Token& token) = 0;
};

}  // namespace mongo
