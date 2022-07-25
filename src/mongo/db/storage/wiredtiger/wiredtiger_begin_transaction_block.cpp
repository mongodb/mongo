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


#include "mongo/platform/basic.h"

#include <cstdio>
#include <fmt/format.h>

#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/errno_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
using namespace fmt::literals;

WiredTigerBeginTxnBlock::WiredTigerBeginTxnBlock(
    WT_SESSION* session,
    PrepareConflictBehavior prepareConflictBehavior,
    RoundUpPreparedTimestamps roundUpPreparedTimestamps,
    RoundUpReadTimestamp roundUpReadTimestamp,
    UntimestampedWriteAssertion allowUntimestampedWrite)
    : _session(session) {
    invariant(!_rollback);

    str::stream builder;
    if (prepareConflictBehavior == PrepareConflictBehavior::kIgnoreConflicts) {
        builder << "ignore_prepare=true,";
    } else if (prepareConflictBehavior == PrepareConflictBehavior::kIgnoreConflictsAllowWrites) {
        builder << "ignore_prepare=force,";
    }
    if (roundUpPreparedTimestamps == RoundUpPreparedTimestamps::kRound ||
        roundUpReadTimestamp == RoundUpReadTimestamp::kRound) {
        builder << "roundup_timestamps=(";
        if (roundUpPreparedTimestamps == RoundUpPreparedTimestamps::kRound) {
            builder << "prepared=true,";
        }
        if (roundUpReadTimestamp == RoundUpReadTimestamp::kRound) {
            builder << "read=true";
        }
        builder << "),";
    }
    if (allowUntimestampedWrite == UntimestampedWriteAssertion::kSuppress) {
        builder << "no_timestamp=true,";
    }

    const std::string beginTxnConfigString = builder;
    invariantWTOK(_session->begin_transaction(_session, beginTxnConfigString.c_str()), _session);
    _rollback = true;
}

WiredTigerBeginTxnBlock::WiredTigerBeginTxnBlock(WT_SESSION* session, const char* config)
    : _session(session) {
    invariant(!_rollback);
    invariantWTOK(_session->begin_transaction(_session, config), _session);
    _rollback = true;
}

WiredTigerBeginTxnBlock::~WiredTigerBeginTxnBlock() {
    if (_rollback) {
        invariant(_session->rollback_transaction(_session, nullptr) == 0);
    }
}

Status WiredTigerBeginTxnBlock::setReadSnapshot(Timestamp readTimestamp) {
    invariant(_rollback);
    return wtRCToStatus(
        _session->timestamp_transaction_uint(_session, WT_TS_TXN_TYPE_READ, readTimestamp.asULL()),
        _session);
}

void WiredTigerBeginTxnBlock::done() {
    invariant(_rollback);
    _rollback = false;
}

}  // namespace mongo
