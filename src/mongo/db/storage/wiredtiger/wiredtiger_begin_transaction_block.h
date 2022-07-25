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

#include <string>

#include <wiredtiger.h>

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/recovery_unit.h"

namespace mongo {

/**
 * When constructed, this object begins a WiredTiger transaction on the provided session. The
 * transaction will be rolled back if done() is not called before the object is destructed.
 */
class WiredTigerBeginTxnBlock {
public:
    // Whether or not to round up to the oldest timestamp when the read timestamp is behind it.
    enum class RoundUpReadTimestamp {
        kNoRoundError,  // Do not round to the oldest timestamp. BadValue error may be returned.
        kRound          // Round the read timestamp up to the oldest timestamp when it is behind.
    };

    // Dictates whether to round up prepare and commit timestamp of a prepared transaction.
    // 'kNoRound' - Does not round up prepare and commit timestamp of a prepared transaction.
    // 'kRound' - The prepare timestamp will be rounded up to the oldest timestamp if found to be
    // earlier; and the commit timestamp will be rounded up to the prepare timestamp if found to be
    // earlier.
    enum class RoundUpPreparedTimestamps { kNoRound, kRound };

    enum class UntimestampedWriteAssertion { kSuppress, kEnforce };

    WiredTigerBeginTxnBlock(WT_SESSION* session,
                            PrepareConflictBehavior prepareConflictBehavior,
                            RoundUpPreparedTimestamps roundUpPreparedTimestamps,
                            RoundUpReadTimestamp roundUpReadTimestamp,
                            UntimestampedWriteAssertion allowUntimestampedWrite);
    WiredTigerBeginTxnBlock(WT_SESSION* session, const char* config);
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
    WT_SESSION* _session;
    bool _rollback = false;
};

}  // namespace mongo
