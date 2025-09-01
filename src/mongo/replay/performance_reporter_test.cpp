/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/replay/performance_reporter.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/replay/test_packet.h"
#include "mongo/unittest/unittest.h"

#include <fstream>
#include <string>
#include <vector>

namespace mongo {

class PerformanceReporterTest : public unittest::Test {
protected:
    const char* perfFileName = "perf_file.bin";

    void setUp() override {}

    void tearDown() override {
        std::remove(perfFileName);
    }
};


TEST_F(PerformanceReporterTest, WriteSomeData) {
    ASSERT_FALSE(boost::filesystem::exists(perfFileName));
    PerformancePacket packet1{1, 1, 100, 1};
    PerformancePacket packet2{2, 2, 200, 2};
    PerformancePacket packet3{1, 3, 500, 5};
    std::vector<PerformancePacket> packets{packet1, packet2, packet3};
    const char* uri = "fake_uri";
    const size_t dumpToDiskThreshold = 1;
    {
        // Performance recorded follows the same RAII pattern of SessionHandler. When the dtor runs
        // the bg thread is joined, all the data left to write is dumped on disk and the file is
        // closed.
        PerformanceReporter reporter{uri, perfFileName, dumpToDiskThreshold};
        for (auto&& p : packets) {
            reporter.add(std::move(p));
        }
    }
    // read back the packets.
    ASSERT_TRUE(boost::filesystem::exists(perfFileName));
    auto testRecording = PerformanceReporter::read(perfFileName);
    ASSERT_EQ(testRecording.mongoURI, uri);
    ASSERT_EQ(testRecording.packets, packets);
}

}  // namespace mongo
