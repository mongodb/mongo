/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"

#include "mongo/base/string_data.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_server_status.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/system_clock_source.h"

#include <sstream>
#include <string>

#include <wiredtiger.h>

namespace mongo {

using std::string;
using std::stringstream;

class WiredTigerConnectionTest {
public:
    WiredTigerConnectionTest(StringData dbpath, StringData extraStrings) : _conn(nullptr) {
        std::stringstream ss;
        ss << "create,";
        ss << extraStrings;
        string config = ss.str();
        _fastClockSource = std::make_unique<SystemClockSource>();
        int ret = wiredtiger_open(std::string{dbpath}.c_str(), nullptr, config.c_str(), &_conn);
        ASSERT_OK(wtRCToStatus(ret, nullptr));
        ASSERT(_conn);
    }
    ~WiredTigerConnectionTest() {
        _conn->close(_conn, nullptr);
    }
    WT_CONNECTION* getConnection() const {
        return _conn;
    }
    ClockSource* getClockSource() {
        return _fastClockSource.get();
    }

private:
    WT_CONNECTION* _conn;
    std::unique_ptr<ClockSource> _fastClockSource;
};

class WiredTigerConnectionHarnessHelper {
public:
    WiredTigerConnectionHarnessHelper(StringData extraStrings)
        : _dbpath("wt_test"),
          _connectionTest(_dbpath.path(), extraStrings),
          _connection(_connectionTest.getConnection(),
                      _connectionTest.getClockSource(),
                      /*sessionCacheMax=*/33000) {}

    WiredTigerConnectionHarnessHelper(StringData extraStrings, unsigned long sessionCacheMax)
        : _dbpath("wt_test"),
          _connectionTest(_dbpath.path(), extraStrings),
          _connection(
              _connectionTest.getConnection(), _connectionTest.getClockSource(), sessionCacheMax) {}


    WiredTigerConnection* getConnection() {
        return &_connection;
    }

private:
    unittest::TempDir _dbpath;
    WiredTigerConnectionTest _connectionTest;
    WiredTigerConnection _connection;
};

TEST(WiredTigerConnectionTest, CheckSessionCacheCleanup) {
    WiredTigerConnectionHarnessHelper harnessHelper("");
    WiredTigerConnection* connection = harnessHelper.getConnection();
    ASSERT_EQUALS(connection->getIdleSessionsCount(), 0U);
    {
        WiredTigerManagedSession _session = connection->getUninterruptibleSession();
        ASSERT_EQUALS(connection->getIdleSessionsCount(), 0U);
    }
    // Destroying of a session puts it in the session cache
    ASSERT_EQUALS(connection->getIdleSessionsCount(), 1U);

    // An idle timeout of 0 means we never expire idle sessions
    connection->closeExpiredIdleSessions(0);
    ASSERT_EQUALS(connection->getIdleSessionsCount(), 1U);
    sleepmillis(10);

    // Expire sessions that have been idle for 10 secs
    connection->closeExpiredIdleSessions(10000);
    ASSERT_EQUALS(connection->getIdleSessionsCount(), 1U);
    // Expire sessions that have been idle for 2 millisecs
    connection->closeExpiredIdleSessions(2);
    ASSERT_EQUALS(connection->getIdleSessionsCount(), 0U);
}

TEST(WiredTigerConnectionTest, ReleaseCursorDuringShutdown) {
    WiredTigerConnectionHarnessHelper harnessHelper("");
    WiredTigerConnection* connection = harnessHelper.getConnection();
    WiredTigerManagedSession session = connection->getUninterruptibleSession();
    // Simulates the cursor already being deleted during shutdown.
    WT_CURSOR* cursor = nullptr;

    connection->shuttingDown();
    ASSERT(connection->isShuttingDown());

    auto tableIdWeDontCareAbout = WiredTigerUtil::genTableId();
    // Skips actually trying to release the cursor to avoid the segmentation fault.
    session->releaseCursor(tableIdWeDontCareAbout, cursor, "");
}

// Test that, in the event that we tried to release a session back into the session cache after
// the storage engine had shut down, we do not invariant and did not actually release the session
// back into the cache.
TEST(WiredTigerConnectionTest, ReleaseSessionAfterShutdown) {
    WiredTigerConnectionHarnessHelper harnessHelper("");
    WiredTigerConnection* connection = harnessHelper.getConnection();
    // Assert that there are no idle sessions in the cache to start off with.
    ASSERT_EQ(connection->getIdleSessionsCount(), 0);
    {
        WiredTigerManagedSession session = connection->getUninterruptibleSession();
        WT_CURSOR* cursor = nullptr;

        connection->shuttingDown();
        ASSERT(connection->isShuttingDown());
        connection->restart();

        // After the connection shut down, the outstanding session's epoch should be older than
        // the cache's current epoch. So, when we go to release it, we should see this and not
        // release it back into the cache. We should also not release its cursor.
        auto tableIdWeDontCareAbout = WiredTigerUtil::genTableId();
        session->releaseCursor(tableIdWeDontCareAbout, cursor, "");
    }
    // Check that the session was not added back into the cache.
    ASSERT_EQ(connection->getIdleSessionsCount(), 0);
}

// Test that, if a recovery unit reconfigures its session, the session will have its configuration
// reset to default values before it is released to the session cache where it can be used by
// another recovery unit.
TEST(WiredTigerConnectionTest, resetConfigurationBeforeReleasingSessionToCache) {
    WiredTigerConnectionHarnessHelper harnessHelper("");
    WiredTigerConnection* connection = harnessHelper.getConnection();

    // Assert that we start off with no sessions in the session cache.
    ASSERT_EQ(connection->getIdleSessionsCount(), 0U);
    {
        WiredTigerRecoveryUnit recoveryUnit(connection, nullptr);
        // Set cache max wait time to be a non-default value.
        recoveryUnit.setCacheMaxWaitTimeout(Milliseconds{100});

        WiredTigerSession* session = recoveryUnit.getSessionNoTxn();
        // Set ignore_cache_size to be true
        session->modifyConfiguration("ignore_cache_size=true", "ignore_cache_size=false");
        // Set isolation level to be read-uncommitted (by default it is snapshot)
        session->modifyConfiguration("isolation=read-uncommitted", "isolation=snapshot");
        // Set cache_cursors to be false
        session->modifyConfiguration("cache_cursors=false", "cache_cursors=true");
        auto undoConfigStringsSet = session->getUndoConfigStrings();

        // Check that all the expected undo config strings are present.
        ASSERT_EQ(undoConfigStringsSet.size(), 4);
        ASSERT(undoConfigStringsSet.find("cache_max_wait_ms=0") != undoConfigStringsSet.end());
        ASSERT(undoConfigStringsSet.find("ignore_cache_size=false") != undoConfigStringsSet.end());
        ASSERT(undoConfigStringsSet.find("isolation=snapshot") != undoConfigStringsSet.end());
        ASSERT(undoConfigStringsSet.find("cache_cursors=true") != undoConfigStringsSet.end());
    };
    // Destructing the recovery unit should put the session used by the recovery unit back into the
    // session cache.
    ASSERT_EQ(connection->getIdleSessionsCount(), 1U);
    {
        WiredTigerRecoveryUnit recoveryUnit(connection, nullptr);
        WiredTigerSession* session = recoveryUnit.getSessionNoTxn();
        // Assert that before it was released back into the session cache, the set of undo config
        // strings was cleared, which should indicate that the changes to the default settings of
        // the session were undone.
        ASSERT_EQ(session->getUndoConfigStrings().size(), 0);
    }
}

// Test that, if a recovery unit sets a non-default configuration value for its session and then
// reconfigures it back to the default value, that we do not store the undo config string (because
// we do not need to take any action to restore the session to its default configuration).
TEST(WiredTigerConnectionTest, resetConfigurationToDefault) {
    WiredTigerConnectionHarnessHelper harnessHelper("");
    WiredTigerConnection* connection = harnessHelper.getConnection();

    WiredTigerRecoveryUnit recoveryUnit(connection, nullptr);
    // Set cache max wait time to be a non-default value.
    recoveryUnit.setCacheMaxWaitTimeout(Milliseconds{100});

    WiredTigerSession* session = recoveryUnit.getSessionNoTxn();
    // Set ignore_cache_size to be true
    session->modifyConfiguration("ignore_cache_size=true", "ignore_cache_size=false");
    // Set isolation level to be read-uncommitted (by default it is snapshot)
    session->modifyConfiguration("isolation=read-uncommitted", "isolation=snapshot");
    // Set cache_cursors to be false
    session->modifyConfiguration("cache_cursors=false", "cache_cursors=true");
    auto undoConfigStringsSet = session->getUndoConfigStrings();

    // Check that all the expected undo config strings are present.
    ASSERT_EQ(undoConfigStringsSet.size(), 4);
    ASSERT(undoConfigStringsSet.find("cache_max_wait_ms=0") != undoConfigStringsSet.end());
    ASSERT(undoConfigStringsSet.find("ignore_cache_size=false") != undoConfigStringsSet.end());
    ASSERT(undoConfigStringsSet.find("isolation=snapshot") != undoConfigStringsSet.end());
    ASSERT(undoConfigStringsSet.find("cache_cursors=true") != undoConfigStringsSet.end());

    // Set all values back to their defaults.
    recoveryUnit.setCacheMaxWaitTimeout(Milliseconds{0});
    session->modifyConfiguration("ignore_cache_size=false", "ignore_cache_size=false");
    session->modifyConfiguration("isolation=snapshot", "isolation=snapshot");
    session->modifyConfiguration("cache_cursors=true", "cache_cursors=true");

    // Check that we do not store any undo config strings.
    ASSERT_EQ(session->getUndoConfigStrings().size(), 0);
}

TEST(WiredTigerConnectionTest, CheckSessionCacheMax) {
    auto maxSessionCacheSize = 6;
    WiredTigerConnectionHarnessHelper harnessHelper("", maxSessionCacheSize);
    WiredTigerConnection* connection = harnessHelper.getConnection();

    ASSERT_EQUALS(connection->getIdleSessionsCount(), 0U);

    // An idle timeout of 0 means never expire idle sessions.
    connection->closeExpiredIdleSessions(0);
    {
        std::array<WiredTigerManagedSession, 10> sessions;

        for (auto& session : sessions) {
            session = connection->getUninterruptibleSession();
        }
        // Check that the cache is empty here.
        ASSERT_EQUALS(connection->getIdleSessionsCount(), 0U);
    }
    // Destroying a session puts it in the session cache
    ASSERT_EQUALS(connection->getIdleSessionsCount(), 6U);
}

TEST(WiredTigerConnectionTest, SettingSessionCacheMaxToZeroDisablesSessionCaching) {
    auto maxSessionCacheSize = 0;
    WiredTigerConnectionHarnessHelper harnessHelper("", maxSessionCacheSize);
    WiredTigerConnection* connection = harnessHelper.getConnection();

    ASSERT_EQUALS(connection->getIdleSessionsCount(), 0U);

    // An idle timeout of 0 means never expire idle sessions.
    connection->closeExpiredIdleSessions(0);
    {
        std::array<WiredTigerManagedSession, 10> sessions;

        for (auto& session : sessions) {
            session = connection->getUninterruptibleSession();
        }
        // Check that the cache is empty here.
        ASSERT_EQUALS(connection->getIdleSessionsCount(), 0U);
    }
    // Destroying a session puts it in the session cache. Here, since we disabled session caching,
    // the session cache should still be empty.
    ASSERT_EQUALS(connection->getIdleSessionsCount(), 0U);
}

}  // namespace mongo
