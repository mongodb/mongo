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

#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"

#include <memory>
#include <ostream>
#include <string>

#include <wiredtiger.h>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

class WiredTigerConnectionTest {
public:
    WiredTigerConnectionTest(StringData dbpath, StringData extraStrings) : _conn(nullptr) {
        std::stringstream ss;
        ss << "create,";
        ss << extraStrings;
        std::string config = ss.str();
        int ret = wiredtiger_open(std::string{dbpath}.c_str(), nullptr, config.c_str(), &_conn);
        invariant(wtRCToStatus(ret, nullptr));
    }
    ~WiredTigerConnectionTest() {
        _conn->close(_conn, nullptr);
    }
    WT_CONNECTION* getConnection() const {
        return _conn;
    }

private:
    WT_CONNECTION* _conn;
};

class WiredTigerTestHelper : public ScopedGlobalServiceContextForTest {
public:
    WiredTigerTestHelper() {
        _ru = std::make_unique<WiredTigerRecoveryUnit>(&_connection, nullptr);
        _session = _ru->getSession();
        invariant(wtRCToStatus(_session->create("table:mytable", nullptr), *_session));
        _ru->abandonSnapshot();
    }

    WiredTigerSession* session() const {
        return _session;
    }

private:
    unittest::TempDir _dbpath{"wt_test"};
    WiredTigerConnectionTest _connectionTest{_dbpath.path(), ""};
    ClockSourceMock _clockSource;
    WiredTigerConnection _connection{
        _connectionTest.getConnection(), &_clockSource, /*sessionCacheMax=*/33000};
    std::unique_ptr<WiredTigerRecoveryUnit> _ru;

    WiredTigerSession* _session;
};

void BM_WiredTigerBeginTxnBlock(benchmark::State& state) {
    WiredTigerTestHelper helper;
    for (auto _ : state) {
        WiredTigerBeginTxnBlock beginTxn(helper.session(), nullptr);
    }
}

template <PrepareConflictBehavior behavior, bool round>
void BM_WiredTigerBeginTxnBlockWithArgs(benchmark::State& state) {
    WiredTigerTestHelper helper;
    for (auto _ : state) {
        WiredTigerBeginTxnBlock beginTxn(helper.session(),
                                         behavior,
                                         round,
                                         RoundUpReadTimestamp::kNoRoundError,
                                         RecoveryUnit::UntimestampedWriteAssertionLevel::kEnforce);
    }
}

void BM_setTimestamp(benchmark::State& state) {
    WiredTigerTestHelper helper;
    for (auto _ : state) {
        WiredTigerBeginTxnBlock beginTxn(helper.session(), nullptr);
        ASSERT_OK(beginTxn.setReadSnapshot(Timestamp(1)));
    }
}

BENCHMARK(BM_WiredTigerBeginTxnBlock);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs, PrepareConflictBehavior::kEnforce, false);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs, PrepareConflictBehavior::kEnforce, true);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs,
                   PrepareConflictBehavior::kIgnoreConflicts,
                   false);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs,
                   PrepareConflictBehavior::kIgnoreConflicts,
                   true);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs,
                   PrepareConflictBehavior::kIgnoreConflictsAllowWrites,
                   false);
BENCHMARK_TEMPLATE(BM_WiredTigerBeginTxnBlockWithArgs,
                   PrepareConflictBehavior::kIgnoreConflictsAllowWrites,
                   true);
BENCHMARK(BM_setTimestamp);

}  // namespace
}  // namespace mongo
