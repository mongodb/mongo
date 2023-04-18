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
#include "mongo/unittest/assert.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include <memory>

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
        ASSERT_WT_OK(
            wiredtiger_open(_path.path().c_str(), nullptr, "create,statistics=(fast),", &_conn));
        ASSERT_WT_OK(_conn->open_session(_conn, nullptr, "isolation=snapshot", &_session));
        ASSERT_WT_OK(_session->create(
            _session, _uri.c_str(), "type=file,key_format=q,value_format=u,log=(enabled=false)"));
    }

    void closeConnection() {
        ASSERT_EQ(_conn->close(_conn, nullptr), 0);
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
        ASSERT_WT_OK(_session->begin_transaction(_session, nullptr));

        WT_CURSOR* cursor;
        ASSERT_WT_OK(_session->open_cursor(_session, _uri.c_str(), nullptr, nullptr, &cursor));

        cursor->set_key(cursor, key);

        WT_ITEM item{data.data(), data.size()};
        cursor->set_value(cursor, &item);

        ASSERT_WT_OK(cursor->insert(cursor));
        ASSERT_WT_OK(cursor->close(cursor));
        ASSERT_WT_OK(_session->commit_transaction(_session, nullptr));

        // Without a checkpoint, an operation is not guaranteed to write to disk.
        ASSERT_WT_OK(_session->checkpoint(_session, nullptr));
    }

    /**
     * Reads at the specified key from WT.
     */
    void readAtKey(int64_t key) {
        ASSERT_WT_OK(_session->begin_transaction(_session, nullptr));

        WT_CURSOR* cursor;
        ASSERT_WT_OK(_session->open_cursor(_session, _uri.c_str(), nullptr, nullptr, &cursor));

        cursor->set_key(cursor, key);
        ASSERT_WT_OK(cursor->search(cursor));

        WT_ITEM value;
        ASSERT_WT_OK(cursor->get_value(cursor, &value));

        ASSERT_WT_OK(cursor->close(cursor));
        ASSERT_WT_OK(_session->commit_transaction(_session, nullptr));
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

    /**
     * Reads data written by the test from WT. Causes the bytesRead stat to be incremented. May
     * also cause timeReadingMicros to be incremented, but not always.
     */
    void readTestWrites() {
        for (int64_t i = _kMaxReads; i < _writeKey; i++) {
            readAtKey(i);
        }
    }

    unittest::TempDir _path{"wiredtiger_operation_stats_test"};
    std::string _uri{"table:wiredtiger_operation_stats_test"};
    WT_CONNECTION* _conn;
    WT_SESSION* _session;
    /* Number of reads the fixture will prepare in setUp(), consequently max amount of times read()
     * can be called in a test.  */
    static constexpr int64_t _kMaxReads = 2;
    /* Next key to be used by read(), must be initialized at 0. */
    int64_t _readKey = 0;
    /* Next key to be used by write(), must be initialized >= _kMaxReads. */
    int64_t _writeKey = _kMaxReads;
};

TEST_F(WiredTigerStatsTest, EmptySession) {
    // Read and write statistics should be empty. Check "data" field does not exist. "wait" fields
    // such as the schemaLock might have some value.
    auto statsBson = WiredTigerStats{_session}.toBSON();
    ASSERT_FALSE(statsBson.hasField("data")) << statsBson;
}

TEST_F(WiredTigerStatsTest, SessionWithWrite) {
    write();

    auto statsObj = WiredTigerStats{_session}.toBSON();
    auto dataSection = statsObj["data"];
    ASSERT_EQ(dataSection.type(), BSONType::Object) << statsObj;

    ASSERT(dataSection["bytesWritten"]) << statsObj;
    for (auto&& [name, value] : dataSection.Obj()) {
        ASSERT_EQ(value.type(), BSONType::NumberLong) << statsObj;
        ASSERT_GT(value.numberLong(), 0) << statsObj;
    }
}

TEST_F(WiredTigerStatsTest, SessionWithRead) {
    read();

    auto statsObj = WiredTigerStats{_session}.toBSON();

    auto dataSection = statsObj["data"];
    ASSERT_EQ(dataSection.type(), BSONType::Object) << statsObj;

    ASSERT(dataSection["bytesRead"]) << statsObj;
    for (auto&& [name, value] : dataSection.Obj()) {
        ASSERT_EQ(value.type(), BSONType::NumberLong) << statsObj;
        ASSERT_GT(value.numberLong(), 0) << statsObj;
    }
}

TEST_F(WiredTigerStatsTest, SessionWithLargeWriteAndLargeRead) {
    auto remaining = static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1;
    while (remaining > 0) {
        std::string data(1024 * 1024, 'a');
        remaining -= data.size();
        write(data);
    }

    auto statsObj = WiredTigerStats{_session}.toBSON();
    ASSERT_GT(statsObj["data"]["bytesWritten"].numberLong(), std::numeric_limits<uint32_t>::max())
        << statsObj;

    // Closing the connection will ensure that tests actually have to read into cache.
    closeConnection();
    openConnectionAndCreateSession();

    readTestWrites();

    statsObj = WiredTigerStats{_session}.toBSON();
    ASSERT_GT(statsObj["data"]["bytesRead"].numberLong(), std::numeric_limits<uint32_t>::max())
        << statsObj;
}

TEST_F(WiredTigerStatsTest, OperationsAddToSessionStats) {
    std::vector<std::unique_ptr<WiredTigerStats>> operationStats;

    write();
    WiredTigerStats firstWrite(_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(firstWrite - WiredTigerStats{}));
    read();
    WiredTigerStats firstRead(_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(firstRead - firstWrite));
    write();
    WiredTigerStats secondWrite(_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(secondWrite - firstRead));
    read();
    WiredTigerStats secondRead(_session);
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
    ASSERT_EQ(dataSection.type(), BSONType::Object) << addedObj;
    ASSERT_EQ(dataSection["bytesWritten"].numberLong(), bytesWritten) << addedObj;
    ASSERT_EQ(dataSection["timeWritingMicros"].numberLong(), timeWritingMicros) << addedObj;
    ASSERT_EQ(dataSection["bytesRead"].numberLong(), bytesRead) << addedObj;
    ASSERT_EQ(dataSection["timeReadingMicros"].numberLong(), timeReadingMicros) << addedObj;

    auto fetchedObj = fetchedSessionStats.toBSON();
    auto fetchedDataSection = fetchedObj["data"];
    ASSERT_EQ(fetchedDataSection.type(), BSONType::Object) << fetchedObj;
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
    WiredTigerStats firstWrite(_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(firstWrite - WiredTigerStats{}));
    read();
    WiredTigerStats firstRead(_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(firstRead - firstWrite));
    write();
    WiredTigerStats secondWrite(_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(secondWrite - firstRead));
    read();
    WiredTigerStats secondRead(_session);
    operationStats.push_back(std::make_unique<WiredTigerStats>(secondRead - secondWrite));

    WiredTigerStats& fetchedSessionStats = secondRead;

    // Assert fetchedSessionStats was not zero before checking subtract results in it being zero.
    // We ignore the time statistics as those might still be 0 from time to time.
    auto preSubtractObj = fetchedSessionStats.toBSON();
    auto preSubtract = preSubtractObj["data"];
    ASSERT_EQ(preSubtract.type(), BSONType::Object) << preSubtractObj;
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

    WiredTigerStats stats{_session};
    auto clone = stats.clone();

    ASSERT_BSONOBJ_EQ(stats.toBSON(), clone->toBSON());

    stats += *clone;
    ASSERT_BSONOBJ_NE(stats.toBSON(), clone->toBSON());
}

}  // namespace
}  // namespace mongo
