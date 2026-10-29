// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/replay/performance_reporter.h"

#include "mongo/unittest/unittest.h"

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
