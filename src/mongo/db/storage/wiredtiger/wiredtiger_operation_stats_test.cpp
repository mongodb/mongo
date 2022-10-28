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

#include "mongo/db/storage/wiredtiger/wiredtiger_operation_stats.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

#define ASSERT_WT_OK(result) ASSERT_EQ(result, 0) << wiredtiger_strerror(result)

class WiredTigerOperationStatsTest : public unittest::Test {
protected:
    void setUp() override {
        ASSERT_WT_OK(
            wiredtiger_open(_path.path().c_str(), nullptr, "create,statistics=(fast),", &_conn));
        ASSERT_WT_OK(_conn->open_session(_conn, nullptr, "isolation=snapshot", &_session));
        ASSERT_WT_OK(_session->create(
            _session, _uri.c_str(), "type=file,key_format=q,value_format=u,log=(enabled=false)"));
    }

    void tearDown() override {
        ASSERT_EQ(_conn->close(_conn, nullptr), 0);
    }

    /**
     * Writes the given data using WT. Causes the bytesWritten and timeWritingMicros stats to be
     * incremented.
     */
    void write(const std::string& data) {
        ASSERT_WT_OK(_session->begin_transaction(_session, nullptr));

        WT_CURSOR* cursor;
        ASSERT_WT_OK(_session->open_cursor(_session, _uri.c_str(), nullptr, nullptr, &cursor));

        cursor->set_key(cursor, _key++);

        WT_ITEM item{data.data(), data.size()};
        cursor->set_value(cursor, &item);

        ASSERT_WT_OK(cursor->insert(cursor));
        ASSERT_WT_OK(cursor->close(cursor));
        ASSERT_WT_OK(_session->commit_transaction(_session, nullptr));
        ASSERT_WT_OK(_session->checkpoint(_session, nullptr));
    }

    /**
     * Reads all of the previously written data from WT. Causes the bytesRead and timeReadingMicros
     * stats to be incremented.
     */
    void read() {
        tearDown();
        setUp();

        ASSERT_WT_OK(_session->begin_transaction(_session, nullptr));

        WT_CURSOR* cursor;
        ASSERT_WT_OK(_session->open_cursor(_session, _uri.c_str(), nullptr, nullptr, &cursor));

        for (int64_t i = 0; i < _key; ++i) {
            cursor->set_key(cursor, i);
            ASSERT_WT_OK(cursor->search(cursor));

            WT_ITEM value;
            ASSERT_WT_OK(cursor->get_value(cursor, &value));
        }

        ASSERT_WT_OK(cursor->close(cursor));
        ASSERT_WT_OK(_session->commit_transaction(_session, nullptr));
    }

    unittest::TempDir _path{"wiredtiger_operation_stats_test"};
    std::string _uri{"table:wiredtiger_operation_stats_test"};
    WT_CONNECTION* _conn;
    WT_SESSION* _session;
    int64_t _key = 0;
};

TEST_F(WiredTigerOperationStatsTest, Empty) {
    ASSERT_BSONOBJ_EQ(WiredTigerOperationStats{_session}.toBSON(), BSONObj{});
}

TEST_F(WiredTigerOperationStatsTest, Write) {
    write("a");

    auto statsObj = WiredTigerOperationStats{_session}.toBSON();

    auto dataSection = statsObj["data"];
    ASSERT_EQ(dataSection.type(), BSONType::Object) << statsObj;

    ASSERT(dataSection["bytesWritten"]) << statsObj;
    ASSERT(dataSection["timeWritingMicros"]) << statsObj;
    for (auto&& [name, value] : dataSection.Obj()) {
        ASSERT_EQ(value.type(), BSONType::NumberLong) << statsObj;
        ASSERT_GT(value.numberLong(), 0) << statsObj;
    }
}

TEST_F(WiredTigerOperationStatsTest, Read) {
    write("a");
    read();

    auto statsObj = WiredTigerOperationStats{_session}.toBSON();

    auto dataSection = statsObj["data"];
    ASSERT_EQ(dataSection.type(), BSONType::Object) << statsObj;

    ASSERT(dataSection["bytesRead"]) << statsObj;
    ASSERT(dataSection["timeReadingMicros"]) << statsObj;
    for (auto&& [name, value] : dataSection.Obj()) {
        ASSERT_EQ(value.type(), BSONType::NumberLong) << statsObj;
        ASSERT_GT(value.numberLong(), 0) << statsObj;
    }
}

TEST_F(WiredTigerOperationStatsTest, Large) {
    auto remaining = static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1;
    while (remaining > 0) {
        std::string data(1024 * 1024, 'a');
        remaining -= data.size();
        write(data);
    }

    auto statsObj = WiredTigerOperationStats{_session}.toBSON();
    ASSERT_GT(statsObj["data"]["bytesWritten"].Long(), std::numeric_limits<uint32_t>::max())
        << statsObj;

    read();

    statsObj = WiredTigerOperationStats{_session}.toBSON();
    ASSERT_GT(statsObj["data"]["bytesRead"].Long(), std::numeric_limits<uint32_t>::max())
        << statsObj;
}

TEST_F(WiredTigerOperationStatsTest, Add) {
    std::vector<std::unique_ptr<WiredTigerOperationStats>> stats;

    write("a");
    stats.push_back(std::make_unique<WiredTigerOperationStats>(_session));

    read();
    stats.push_back(std::make_unique<WiredTigerOperationStats>(_session));

    write("aa");
    stats.push_back(std::make_unique<WiredTigerOperationStats>(_session));

    read();
    stats.push_back(std::make_unique<WiredTigerOperationStats>(_session));

    long long bytesWritten = 0;
    long long timeWritingMicros = 0;
    long long bytesRead = 0;
    long long timeReadingMicros = 0;

    WiredTigerOperationStats combined;

    for (auto&& op : stats) {
        auto statsObj = op->toBSON();

        bytesWritten += statsObj["data"]["bytesWritten"].numberLong();
        timeWritingMicros += statsObj["data"]["timeWritingMicros"].numberLong();
        bytesRead += statsObj["data"]["bytesRead"].numberLong();
        timeReadingMicros += statsObj["data"]["timeReadingMicros"].numberLong();

        combined += *op;
    }

    auto combinedObj = combined.toBSON();
    auto dataSection = combinedObj["data"];
    ASSERT_EQ(dataSection.type(), BSONType::Object) << combinedObj;
    ASSERT_EQ(dataSection["bytesWritten"].Long(), bytesWritten) << combinedObj;
    ASSERT_EQ(dataSection["timeWritingMicros"].Long(), timeWritingMicros) << combinedObj;
    ASSERT_EQ(dataSection["bytesRead"].Long(), bytesRead) << combinedObj;
    ASSERT_EQ(dataSection["timeReadingMicros"].Long(), timeReadingMicros) << combinedObj;
}

TEST_F(WiredTigerOperationStatsTest, Clone) {
    write("a");

    WiredTigerOperationStats stats{_session};
    auto clone = stats.clone();

    ASSERT_BSONOBJ_EQ(stats.toBSON(), clone->toBSON());

    stats += *clone;
    ASSERT_BSONOBJ_NE(stats.toBSON(), clone->toBSON());
}

}  // namespace
}  // namespace mongo
