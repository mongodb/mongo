/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_stats.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include <wiredtiger.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWiredTiger

namespace mongo {
namespace {

#define ASSERT_WT_OK(result) ASSERT_EQ(result, 0) << wiredtiger_strerror(result)

class WiredTigerStatsTest : public unittest::Test {
protected:
    void setUp() override {
        openConnectionAndCreateSession();
        // Prepare data to be read by tests. Reading data written within the same transaction does
        // not count towards bytes read into cache.

        // Tests in the fixture do up to _maxReads reads.
        for (int64_t i = 0; i < _kMaxReads; ++i) {
            // Make the write big enough to span different pages.
            writeAtKey(std::string(20000, 'a'), i);
        }

        // Closing the connection will ensure that tests actually have to read into cache.
        closeConnection();
        openConnectionAndCreateSession();
    }

    void tearDown() override {
        closeConnection();
    }

    void openConnectionAndCreateSession() {
        WT_CONNECTION* wtConnection;
        ASSERT_WT_OK(wiredtiger_open(
            _path.path().c_str(), nullptr, "create,statistics=(fast),", &wtConnection));
        _conn = std::make_unique<WiredTigerConnection>(
            wtConnection, &_clockSource, /*sessionCacheMax=*/33000);
        _session = std::make_unique<WiredTigerSession>(_conn.get());
        _session->setTickSource_forTest(&tickSourceMock);
        ASSERT_WT_OK(_session->create(_uri.c_str(),
                                      "type=file,key_format=q,value_format=u,log=(enabled=false)"));
    }

    void closeConnection() {
        _session.reset();
        WT_CONNECTION* wtConnection = _conn->conn();
        _conn.reset();
        ASSERT_EQ(wtConnection->close(wtConnection, nullptr), 0);
    }

    /**
     * Writes some data using WT. Causes the bytesWritten stat to be incremented, and may also
     * increment timeWritingMicros.
     */
    void write() {
        writeAtKey(std::string(_writeKey + 1, 'a'), _writeKey);
        ++_writeKey;
    }

    /**
     * Writes the specified data using WT. Causes the bytesWritten stat to be incremented, and may
     * also increment timeWritingMicros.
     */
    void write(const std::string& data) {
        writeAtKey(data, _writeKey);
        ++_writeKey;
    }

    /**
     * Writes at the specified key to WT.
     */
    void writeAtKey(const std::string& data, int64_t key) {
        ASSERT_WT_OK(_session->begin_transaction(nullptr));

        WT_CURSOR* cursor;
        ASSERT_WT_OK(_session->open_cursor(_uri.c_str(), nullptr, nullptr, &cursor));

        cursor->set_key(cursor, key);

        WT_ITEM item{data.data(), data.size()};
        cursor->set_value(cursor, &item);

        ASSERT_WT_OK(cursor->insert(cursor));
        ASSERT_WT_OK(cursor->close(cursor));
        ASSERT_WT_OK(_session->commit_transaction(nullptr));

        // Without a checkpoint, an operation is not guaranteed to write to disk.
        ASSERT_WT_OK(_session->checkpoint(nullptr));
    }

    /**
     * Reads at the specified key from WT.
     */
    void readAtKey(int64_t key) {
        ASSERT_WT_OK(_session->begin_transaction(nullptr));

        WT_CURSOR* cursor;
        ASSERT_WT_OK(_session->open_cursor(_uri.c_str(), nullptr, nullptr, &cursor));

        cursor->set_key(cursor, key);
        ASSERT_WT_OK(cursor->search(cursor));

        WT_ITEM value;
        ASSERT_WT_OK(cursor->get_value(cursor, &value));

        ASSERT_WT_OK(cursor->close(cursor));
        ASSERT_WT_OK(_session->commit_transaction(nullptr));
    }

    /**
     * Reads fixture data from WT. Causes the bytesRead stat to be incremented. May also cause
     * timeReadingMicros to be incremented, but not always. This function can only be called up to
     * _kMaxReads times within a test.
     */
    void read() {
        ASSERT_LT(_readKey, _kMaxReads);
        readAtKey(_readKey++);
    }

    int64_t getStorageExecutionTime(WiredTigerSession& session) {
        // Querying stats also advances the tick source
        tickSourceMock.setAdvanceOnRead(Microseconds{0});
        WiredTigerStats stats{session};
        BSONObj statsObj = stats.toBSON();
        auto waitingObj = statsObj["timeWaitingMicros"];
        if (waitingObj.eoo()) {
            return 0LL;
        }

        auto storageExecutionTime = waitingObj["storageExecutionMicros"];
        if (storageExecutionTime.eoo()) {
            return 0LL;
        }

        return storageExecutionTime.Long();
    }

    unittest::TempDir _path{"wiredtiger_operation_stats_test"};
    std::string _uri{"table:wiredtiger_operation_stats_test"};
    ClockSourceMock _clockSource;
    TickSourceMock<Microseconds> tickSourceMock;
    std::unique_ptr<WiredTigerConnection> _conn;
    std::unique_ptr<WiredTigerSession> _session;
    /* Number of reads the fixture will prepare in setUp(), consequently max amount of times read()
     * can be called in a test.  */
    static constexpr int64_t _kMaxReads = 2;
    /* Next key to be used by read(), must be initialized at 0. */
    int64_t _readKey = 0;
    /* Next key to be used by write(), must be initialized >= _kMaxReads. */
    int64_t _writeKey = _kMaxReads;
};

TEST_F(WiredTigerStatsTest, SessionWithWrite) {
    write();

    auto statsObj = WiredTigerStats{*_session}.toBSON();
    auto dataSection = statsObj["data"];
    ASSERT_EQ(dataSection.type(), BSONType::object) << statsObj;

    ASSERT(dataSection["bytesWritten"]) << statsObj;
    for (auto&& [name, value] : dataSection.Obj()) {
        ASSERT_EQ(value.type(), BSONType::numberLong) << statsObj;
        ASSERT_GT(value.numberLong(), 0) << statsObj;
    }
}

TEST_F(WiredTigerStatsTest, SessionWithRead) {
    read();

    auto statsObj = WiredTigerStats{*_session}.toBSON();

    auto dataSection = statsObj["data"];
    ASSERT_EQ(dataSection.type(), BSONType::object) << statsObj;

    ASSERT(dataSection["bytesRead"]) << statsObj;
    for (auto&& [name, value] : dataSection.Obj()) {
        ASSERT_EQ(value.type(), BSONType::numberLong) << statsObj;
        ASSERT_GT(value.numberLong(), 0) << statsObj;
    }
}

TEST_F(WiredTigerStatsTest, OperationsAddToSessionStats) {
    std::vector<std::unique_ptr<WiredTigerStats>> operationStats;

    write();
    WiredTigerStats firstWrite(*_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(firstWrite - WiredTigerStats{}));
    read();
    WiredTigerStats firstRead(*_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(firstRead - firstWrite));
    write();
    WiredTigerStats secondWrite(*_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(secondWrite - firstRead));
    read();
    WiredTigerStats secondRead(*_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(secondRead - secondWrite));

    const WiredTigerStats& fetchedSessionStats = secondRead;

    long long bytesWritten = 0;
    long long timeWritingMicros = 0;
    long long bytesRead = 0;
    long long timeReadingMicros = 0;

    WiredTigerStats addedSessionStats;

    for (auto&& op : operationStats) {
        auto statsObj = op->toBSON();

        bytesWritten += statsObj["data"]["bytesWritten"].numberLong();
        timeWritingMicros += statsObj["data"]["timeWritingMicros"].numberLong();
        bytesRead += statsObj["data"]["bytesRead"].numberLong();
        timeReadingMicros += statsObj["data"]["timeReadingMicros"].numberLong();

        addedSessionStats += *op;
    }

    auto addedObj = addedSessionStats.toBSON();
    auto dataSection = addedObj["data"];
    ASSERT_EQ(dataSection.type(), BSONType::object) << addedObj;
    ASSERT_EQ(dataSection["bytesWritten"].numberLong(), bytesWritten) << addedObj;
    ASSERT_EQ(dataSection["timeWritingMicros"].numberLong(), timeWritingMicros) << addedObj;
    ASSERT_EQ(dataSection["bytesRead"].numberLong(), bytesRead) << addedObj;
    ASSERT_EQ(dataSection["timeReadingMicros"].numberLong(), timeReadingMicros) << addedObj;

    auto fetchedObj = fetchedSessionStats.toBSON();
    auto fetchedDataSection = fetchedObj["data"];
    ASSERT_EQ(fetchedDataSection.type(), BSONType::object) << fetchedObj;
    ASSERT_EQ(fetchedDataSection["bytesWritten"].numberLong(), bytesWritten) << fetchedObj;
    ASSERT_EQ(fetchedDataSection["timeWritingMicros"].numberLong(), timeWritingMicros)
        << fetchedObj;
    ASSERT_EQ(fetchedDataSection["bytesRead"].numberLong(), bytesRead) << fetchedObj;
    ASSERT_EQ(fetchedDataSection["timeReadingMicros"].numberLong(), timeReadingMicros)
        << fetchedObj;
}

TEST_F(WiredTigerStatsTest, OperationsSubtractToZero) {
    std::vector<std::unique_ptr<WiredTigerStats>> operationStats;

    write();
    WiredTigerStats firstWrite(*_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(firstWrite - WiredTigerStats{}));
    read();
    WiredTigerStats firstRead(*_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(firstRead - firstWrite));
    write();
    WiredTigerStats secondWrite(*_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(secondWrite - firstRead));
    read();
    WiredTigerStats secondRead(*_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(secondRead - secondWrite));

    WiredTigerStats& fetchedSessionStats = secondRead;

    // Assert fetchedSessionStats was not zero before checking subtract results in it being zero.
    // We ignore the time statistics as those might still be 0 from time to time.
    auto preSubtractObj = fetchedSessionStats.toBSON();
    auto preSubtract = preSubtractObj["data"];
    ASSERT_EQ(preSubtract.type(), BSONType::object) << preSubtractObj;
    ASSERT_GT(preSubtract["bytesWritten"].numberLong(), 0) << preSubtractObj;
    ASSERT_GT(preSubtract["bytesRead"].numberLong(), 0) << preSubtractObj;

    for (auto&& op : operationStats) {
        fetchedSessionStats -= *op;
    }

    auto subtractedObj = fetchedSessionStats.toBSON();
    ASSERT_BSONOBJ_EQ(subtractedObj, BSONObj{});
}

TEST_F(WiredTigerStatsTest, Clone) {
    write();

    WiredTigerStats stats{*_session};
    auto clone = stats.clone();

    ASSERT_BSONOBJ_EQ(stats.toBSON(), clone->toBSON());

    stats += *clone;
    ASSERT_BSONOBJ_NE(stats.toBSON(), clone->toBSON());
}

TEST_F(WiredTigerStatsTest, StorageExecutionTime) {
    ASSERT_EQ(getStorageExecutionTime(*_session), 0);
    tickSourceMock.setAdvanceOnRead(Microseconds{200});
    _session->checkpoint(nullptr);

    auto storageExecutionTime = getStorageExecutionTime(*_session);
    ASSERT_EQ(storageExecutionTime, 200);

    tickSourceMock.setAdvanceOnRead(Microseconds{200});
    _session->checkpoint(nullptr);

    storageExecutionTime = getStorageExecutionTime(*_session);
    ASSERT_EQ(storageExecutionTime, 400);
}

TEST_F(WiredTigerStatsTest, StorageExecutionTimeReuseCachedSession) {
    ASSERT_EQ(_conn->getIdleSessionsCount(), 0);

    {
        // Creates a session which will be cached.
        auto session = _conn->getUninterruptibleSession();
        session->setTickSource_forTest(&tickSourceMock);
        ASSERT_EQ(getStorageExecutionTime(*session), 0);

        tickSourceMock.setAdvanceOnRead(Microseconds{200});
        session->checkpoint(nullptr);

        auto storageExecutionTime = getStorageExecutionTime(*session);
        ASSERT_EQ(storageExecutionTime, 200);
    }

    // Ensure the session is cached.
    ASSERT_EQ(_conn->getIdleSessionsCount(), 1);

    {
        // Ensure we're reusing the cached session.
        auto session = _conn->getUninterruptibleSession();
        ASSERT_EQ(_conn->getIdleSessionsCount(), 0);

        ASSERT_EQ(getStorageExecutionTime(*session), 0);
        tickSourceMock.setAdvanceOnRead(Microseconds{200});
        session->checkpoint(nullptr);

        auto storageExecutionTime = getStorageExecutionTime(*session);
        ASSERT_EQ(storageExecutionTime, 200);
    }
}

}  // namespace
}  // namespace mongo
