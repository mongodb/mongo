/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
class MONGO_MOD_OPEN JournalListener {
public:
    class MONGO_MOD_OPEN Token {
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
