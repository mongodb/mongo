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

#include "mongo/platform/basic.h"

#include <sstream>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/system_clock_source.h"

namespace mongo {

using std::string;
using std::stringstream;

class WiredTigerConnection {
public:
    WiredTigerConnection(StringData dbpath, StringData extraStrings) : _conn(NULL) {
        std::stringstream ss;
        ss << "create,";
        ss << extraStrings;
        string config = ss.str();
        _fastClockSource = std::make_unique<SystemClockSource>();
        int ret = wiredtiger_open(dbpath.toString().c_str(), NULL, config.c_str(), &_conn);
        ASSERT_OK(wtRCToStatus(ret));
        ASSERT(_conn);
    }
    ~WiredTigerConnection() {
        _conn->close(_conn, NULL);
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

class WiredTigerSessionCacheHarnessHelper {
public:
    WiredTigerSessionCacheHarnessHelper(StringData extraStrings)
        : _dbpath("wt_test"),
          _connection(_dbpath.path(), extraStrings),
          _sessionCache(_connection.getConnection(), _connection.getClockSource()) {}


    WiredTigerSessionCache* getSessionCache() {
        return &_sessionCache;
    }

private:
    unittest::TempDir _dbpath;
    WiredTigerConnection _connection;
    WiredTigerSessionCache _sessionCache;
};

TEST(WiredTigerSessionCacheTest, CheckSessionCacheCleanup) {
    WiredTigerSessionCacheHarnessHelper harnessHelper("");
    WiredTigerSessionCache* sessionCache = harnessHelper.getSessionCache();
    ASSERT_EQUALS(sessionCache->getIdleSessionsCount(), 0U);
    {
        UniqueWiredTigerSession _session = sessionCache->getSession();
        ASSERT_EQUALS(sessionCache->getIdleSessionsCount(), 0U);
    }
    // Destroying of a session puts it in the session cache
    ASSERT_EQUALS(sessionCache->getIdleSessionsCount(), 1U);

    // An idle timeout of 0 means never expire idle sessions
    sessionCache->closeExpiredIdleSessions(0);
    ASSERT_EQUALS(sessionCache->getIdleSessionsCount(), 1U);
    sleepmillis(10);

    // Expire sessions that have been idle for 10 secs
    sessionCache->closeExpiredIdleSessions(10000);
    ASSERT_EQUALS(sessionCache->getIdleSessionsCount(), 1U);
    // Expire sessions that have been idle for 2 millisecs
    sessionCache->closeExpiredIdleSessions(2);
    ASSERT_EQUALS(sessionCache->getIdleSessionsCount(), 0U);
}

}  // namespace mongo
