/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <cstdio>

#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/log.h"

namespace mongo {

WiredTigerBeginTxnBlock::WiredTigerBeginTxnBlock(WT_SESSION* session, IgnorePrepared ignorePrepare)
    : _session(session) {
    invariant(!_rollback);
    invariantWTOK(_session->begin_transaction(
        _session, (ignorePrepare == IgnorePrepared::kIgnore) ? "ignore_prepare=true" : nullptr));
    _rollback = true;
}

WiredTigerBeginTxnBlock::WiredTigerBeginTxnBlock(WT_SESSION* session, const char* config)
    : _session(session) {
    invariant(!_rollback);
    invariantWTOK(_session->begin_transaction(_session, config));
    _rollback = true;
}

WiredTigerBeginTxnBlock::~WiredTigerBeginTxnBlock() {
    if (_rollback) {
        invariant(_session->rollback_transaction(_session, nullptr) == 0);
    }
}

Status WiredTigerBeginTxnBlock::setTimestamp(Timestamp readTimestamp, RoundToOldest roundToOldest) {
    invariant(_rollback);
    char readTSConfigString[15 /* read_timestamp= */ + 16 /* 16 hexadecimal digits */ +
                            17 /* ,round_to_oldest= */ + 5 /* false */ + 1 /* trailing null */];
    auto size = std::snprintf(readTSConfigString,
                              sizeof(readTSConfigString),
                              "read_timestamp=%llx,round_to_oldest=%s",
                              readTimestamp.asULL(),
                              (roundToOldest == RoundToOldest::kRound) ? "true" : "false");
    if (size < 0) {
        int e = errno;
        error() << "error snprintf " << errnoWithDescription(e);
        fassertFailedNoTrace(40664);
    }
    invariant(static_cast<std::size_t>(size) < sizeof(readTSConfigString));

    auto status = wtRCToStatus(_session->timestamp_transaction(_session, readTSConfigString));
    return status;
}

void WiredTigerBeginTxnBlock::done() {
    invariant(_rollback);
    _rollback = false;
}

}  // namespace mongo
