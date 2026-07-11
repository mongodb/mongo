// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"

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
#include <string_view>

#include <wiredtiger.h>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

class WiredTigerConnectionTest {
public:
    WiredTigerConnectionTest(std::string_view dbpath, std::string_view extraStrings)
        : _conn(nullptr) {
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
