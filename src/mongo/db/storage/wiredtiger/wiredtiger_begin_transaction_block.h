// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/modules.h"

#include <wiredtiger.h>

namespace mongo {

class WiredTigerSession;
/**
 * When constructed, this object begins a WiredTiger transaction on the provided session. The
 * transaction will be rolled back if done() is not called before the object is destructed.
 */
class WiredTigerBeginTxnBlock {
public:
    // Whether or not to round up to the oldest timestamp when the read timestamp is behind it.
    enum class RoundUpReadTimestamp {
        kNoRoundError = 0,  // Do not round to the oldest timestamp. BadValue error may be returned.
        kRound,  // Round the read timestamp up to the oldest timestamp when it is behind.
        kMax     // kMax should always be last and is a counter of the number of enum values.
    };

    // Dictates whether to round up prepare and commit timestamp of a prepared transaction.
    // 'kNoRound' - Does not round up prepare and commit timestamp of a prepared transaction.
    // 'kRound' - The prepare timestamp will be rounded up to the oldest timestamp if found to be
    // earlier; and the commit timestamp will be rounded up to the prepare timestamp if found to be
    // earlier. kMax should always be last and is a counter of the number of enum values.
    enum class RoundUpPreparedTimestamps { kNoRound = 0, kRound, kMax };

    // kMax should always be last and is a counter of the number of enum values.
    enum class NoReadTimestamp { kFalse = 0, kTrue, kMax };

    WiredTigerBeginTxnBlock(WiredTigerSession* session,
                            PrepareConflictBehavior prepareConflictBehavior,
                            bool roundUpPreparedTimestamps,
                            RoundUpReadTimestamp roundUpReadTimestamp,
                            RecoveryUnit::UntimestampedWriteAssertionLevel allowUntimestampedWrite,
                            boost::optional<uint64_t> claimPreparedId = boost::none);
    WiredTigerBeginTxnBlock(WiredTigerSession* session, const char* config);
    ~WiredTigerBeginTxnBlock();

    /**
     * Sets the read timestamp on the opened transaction. Cannot be called after a call to done().
     */
    Status setReadSnapshot(Timestamp);

    /**
     * End the begin transaction block. Must be called to ensure the opened transaction
     * is not be rolled back.
     */
    void done();

private:
    WiredTigerSession* _session;
    bool _rollback = false;
};

}  // namespace mongo
