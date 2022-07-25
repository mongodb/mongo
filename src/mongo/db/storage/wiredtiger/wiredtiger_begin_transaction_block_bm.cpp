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

#include <benchmark/benchmark.h>

#include "mongo/base/checked_cast.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class WiredTigerConnection {
public:
    WiredTigerConnection(StringData dbpath, StringData extraStrings) : _conn(nullptr) {
        std::stringstream ss;
        ss << "create,";
        ss << extraStrings;
        std::string config = ss.str();
        int ret = wiredtiger_open(dbpath.toString().c_str(), nullptr, config.c_str(), &_conn);
        invariant(wtRCToStatus(ret, nullptr).isOK());
    }
    ~WiredTigerConnection() {
        _conn->close(_conn, nullptr);
    }
    WT_CONNECTION* getConnection() const {
        return _conn;
    }

private:
    WT_CONNECTION* _conn;
};

class WiredTigerTestHelper {
public:
    WiredTigerTestHelper()
        : _dbpath("wt_test"),
          _connection(_dbpath.path(), ""),
          _sessionCache(_connection.getConnection(), &_clockSource) {
        _opCtx.reset(newOperationContext());
        auto ru = WiredTigerRecoveryUnit::get(_opCtx.get());
        _wtSession = ru->getSession()->getSession();
        invariant(wtRCToStatus(_wtSession->create(_wtSession, "table:mytable", nullptr), _wtSession)
                      .isOK());
        ru->abandonSnapshot();
    }

    WiredTigerSessionCache* getSessionCache() {
        return &_sessionCache;
    }

    WiredTigerOplogManager* getOplogManager() {
        return &_oplogManager;
    }

    WT_SESSION* wtSession() const {
        return _wtSession;
    }

    OperationContext* newOperationContext() {
        return new OperationContextNoop(
            new WiredTigerRecoveryUnit(getSessionCache(), &_oplogManager));
    }

    OperationContext* getOperationContext() const {
        return _opCtx.get();
    }

private:
    unittest::TempDir _dbpath;
    WiredTigerConnection _connection;
    ClockSourceMock _clockSource;
    WiredTigerSessionCache _sessionCache;
    WiredTigerOplogManager _oplogManager;
    std::unique_ptr<OperationContext> _opCtx;
    WT_SESSION* _wtSession;
};

void BM_WiredTigerBeginTxnBlock(benchmark::State& state) {
    WiredTigerTestHelper helper;
    for (auto _ : state) {
        WiredTigerBeginTxnBlock beginTxn(helper.wtSession(), nullptr);
    }
}

using mongo::WiredTigerBeginTxnBlock;

template <PrepareConflictBehavior behavior, RoundUpPreparedTimestamps round>
void BM_WiredTigerBeginTxnBlockWithArgs(benchmark::State& state) {
    WiredTigerTestHelper helper;
    for (auto _ : state) {
        WiredTigerBeginTxnBlock beginTxn(
            helper.wtSession(),
            behavior,
            round,
            RoundUpReadTimestamp::kNoRoundError,
            WiredTigerBeginTxnBlock::UntimestampedWriteAssertion::kEnforce);
    }
}


void BM_setTimestamp(benchmark::State& state) {
    WiredTigerTestHelper helper;
    for (auto _ : state) {
        WiredTigerBeginTxnBlock beginTxn(helper.wtSession(), nullptr);
        ASSERT_OK(beginTxn.setReadSnapshot(Timestamp(1)));
    }
}

BENCHMARK(BM_WiredTigerBeginTxnBlock);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs,
                   PrepareConflictBehavior::kEnforce,
                   RoundUpPreparedTimestamps::kNoRound);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs,
                   PrepareConflictBehavior::kEnforce,
                   RoundUpPreparedTimestamps::kRound);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs,
                   PrepareConflictBehavior::kIgnoreConflicts,
                   RoundUpPreparedTimestamps::kNoRound);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs,
                   PrepareConflictBehavior::kIgnoreConflicts,
                   RoundUpPreparedTimestamps::kRound);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs,
                   PrepareConflictBehavior::kIgnoreConflictsAllowWrites,
                   RoundUpPreparedTimestamps::kNoRound);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs,
                   PrepareConflictBehavior::kIgnoreConflictsAllowWrites,
                   RoundUpPreparedTimestamps::kRound);

BENCHMARK(BM_setTimestamp);

}  // namespace
}  // namespace mongo
