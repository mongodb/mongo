/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/otel/metrics/instrumentation/disk_metrics.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using otel::metrics::DynamicMetricNameMaker;
using otel::metrics::DynamicMetricNameTestPasskeyMaker;
using otel::metrics::OtelMetricsCapturer;

constexpr std::string_view kSda = "sda"sv;
constexpr std::string_view kSdb = "sdb"sv;
constexpr std::string_view kSdc = "sdc"sv;

// All tests register DiskMetrics with this fixed device set. The global MetricsService retains
// registrations across tests within the same binary, so every test must use the same device set.
const std::vector<std::string> kTestDevices = {std::string(kSda), std::string(kSdb)};

constexpr std::string_view kSdaReads = "systemMetrics.disks.sda.reads"sv;
constexpr std::string_view kSdaReadSectors = "systemMetrics.disks.sda.read_sectors"sv;
constexpr std::string_view kSdaReadTimeMs = "systemMetrics.disks.sda.read_time_ms"sv;
constexpr std::string_view kSdaWrites = "systemMetrics.disks.sda.writes"sv;
constexpr std::string_view kSdaWriteSectors = "systemMetrics.disks.sda.write_sectors"sv;
constexpr std::string_view kSdaWriteTimeMs = "systemMetrics.disks.sda.write_time_ms"sv;
constexpr std::string_view kSdaIoTimeMs = "systemMetrics.disks.sda.io_time_ms"sv;
constexpr std::string_view kSdaIoQueuedMs = "systemMetrics.disks.sda.io_queued_ms"sv;
constexpr std::string_view kSdbWrites = "systemMetrics.disks.sdb.writes"sv;
constexpr std::string_view kSdcWrites = "systemMetrics.disks.sdc.writes"sv;

// A disk whose name will not resolve to a valid otel metric name
constexpr std::string_view kInvalidDisk = "dm-0"sv;
constexpr std::string_view kInvalidDiskMetricName = "systemMetrics.disks.dm-0.writes"sv;

BSONObj makeDiskBson(std::string_view device,
                     long long reads,
                     long long readSectors,
                     long long readTimeMs,
                     long long writes,
                     long long writeSectors,
                     long long writeTimeMs,
                     long long ioTimeMs,
                     long long ioQueuedMs) {
    BSONObjBuilder b;
    {
        BSONObjBuilder sub(b.subobjStart(device));
        sub.appendNumber("reads", reads);
        sub.appendNumber("read_sectors", readSectors);
        sub.appendNumber("read_time_ms", readTimeMs);
        sub.appendNumber("writes", writes);
        sub.appendNumber("write_sectors", writeSectors);
        sub.appendNumber("write_time_ms", writeTimeMs);
        sub.appendNumber("io_time_ms", ioTimeMs);
        sub.appendNumber("io_queued_ms", ioQueuedMs);
    }
    return b.obj();
}

class DiskOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    OtelMetricsCapturer _capturer;
    DiskMetrics _metrics{kTestDevices};
};

TEST_F(DiskOtelMetricsTest, FirstUpdateSetsBaseline) {
    ASSERT_DOES_NOT_THROW(
        _metrics.update(makeDiskBson(kSda, 10, 20, 50, 50, 100, 300, 1000, 1200)));
}

TEST_F(DiskOtelMetricsTest, SecondUpdateEmitsDeltas) {
    _metrics.update(makeDiskBson(kSda,
                                 /*reads=*/10,
                                 /*readSectors=*/20,
                                 /*readTimeMs=*/50,
                                 /*writes=*/50,
                                 /*writeSectors=*/100,
                                 /*writeTimeMs=*/300,
                                 /*ioTimeMs=*/1000,
                                 /*ioQueuedMs=*/1200));

    _metrics.update(makeDiskBson(kSda,
                                 /*reads=*/25,
                                 /*readSectors=*/50,
                                 /*readTimeMs=*/120,
                                 /*writes=*/70,
                                 /*writeSectors=*/130,
                                 /*writeTimeMs=*/500,
                                 /*ioTimeMs=*/1500,
                                 /*ioQueuedMs=*/1800));

    auto passkey = DynamicMetricNameTestPasskeyMaker::make();
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaReads, passkey)), 15);
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaReadSectors, passkey)),
              30);
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaReadTimeMs, passkey)),
              70);
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaWrites, passkey)), 20);
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaWriteSectors, passkey)),
              30);
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaWriteTimeMs, passkey)),
              200);
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaIoTimeMs, passkey)), 500);
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaIoQueuedMs, passkey)),
              600);
}

TEST_F(DiskOtelMetricsTest, MultipleDeltasAccumulate) {
    _metrics.update(makeDiskBson(kSda, 50, 100, 200, 100, 200, 500, 1000, 1500));
    _metrics.update(makeDiskBson(kSda, 60, 120, 260, 105, 210, 560, 1100, 1650));
    _metrics.update(makeDiskBson(kSda, 80, 160, 380, 115, 230, 680, 1300, 1950));

    auto passkey = DynamicMetricNameTestPasskeyMaker::make();

    // reads delta: (60-50) + (80-60) = 10 + 20 = 30
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaReads, passkey)), 30);
    // read_sectors delta: (120-100) + (160-120) = 20 + 40 = 60
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaReadSectors, passkey)),
              60);
    // read_time_ms delta: (260-200) + (380-260) = 60 + 120 = 180
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaReadTimeMs, passkey)),
              180);
    // writes delta: (105-100) + (115-105) = 5 + 10 = 15
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaWrites, passkey)), 15);
    // write_sectors delta: (210-200) + (230-210) = 10 + 20 = 30
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaWriteSectors, passkey)),
              30);
    // write_time_ms delta: (560-500) + (680-560) = 60 + 120 = 180
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaWriteTimeMs, passkey)),
              180);
    // io_time_ms delta: (1100-1000) + (1300-1100) = 100 + 200 = 300
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaIoTimeMs, passkey)), 300);
    // io_queued_ms delta: (1650-1500) + (1950-1650) = 150 + 300 = 450
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaIoQueuedMs, passkey)),
              450);
}

TEST_F(DiskOtelMetricsTest, UndeclaredDevicesAreIgnored) {
    _metrics.update(makeDiskBson(kSda, 0, 0, 0, 0, 0, 0, 0, 0));

    BSONObjBuilder both;
    {
        BSONObjBuilder sub(both.subobjStart(kSda));
        sub.appendNumber("reads", 10LL);
        sub.appendNumber("read_sectors", 20LL);
        sub.appendNumber("read_time_ms", 50LL);
        sub.appendNumber("writes", 20LL);
        sub.appendNumber("write_sectors", 40LL);
        sub.appendNumber("write_time_ms", 100LL);
        sub.appendNumber("io_time_ms", 200LL);
        sub.appendNumber("io_queued_ms", 300LL);
    }
    {
        // sdc was never registered — its data must be ignored.
        BSONObjBuilder sub(both.subobjStart(kSdc));
        sub.appendNumber("reads", 100LL);
        sub.appendNumber("read_sectors", 200LL);
        sub.appendNumber("read_time_ms", 500LL);
        sub.appendNumber("writes", 500LL);
        sub.appendNumber("write_sectors", 1000LL);
        sub.appendNumber("write_time_ms", 3000LL);
        sub.appendNumber("io_time_ms", 5000LL);
        sub.appendNumber("io_queued_ms", 7000LL);
    }
    _metrics.update(both.obj());

    auto passkey = DynamicMetricNameTestPasskeyMaker::make();
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaWrites, passkey)), 20);

    // sdc was never registered, so its metric name does not exist in the service.
    ASSERT_THROWS_CODE(
        _capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdcWrites, passkey)),
        DBException,
        ErrorCodes::KeyNotFound);
}

TEST_F(DiskOtelMetricsTest, MultipleRegisteredDevicesTrackedIndependently) {
    BSONObjBuilder baseline;
    {
        BSONObjBuilder sub(baseline.subobjStart(kSda));
        sub.appendNumber("reads", 40LL);
        sub.appendNumber("read_sectors", 80LL);
        sub.appendNumber("read_time_ms", 200LL);
        sub.appendNumber("writes", 100LL);
        sub.appendNumber("write_sectors", 200LL);
        sub.appendNumber("write_time_ms", 500LL);
        sub.appendNumber("io_time_ms", 1000LL);
        sub.appendNumber("io_queued_ms", 1500LL);
    }
    {
        BSONObjBuilder sub(baseline.subobjStart(kSdb));
        sub.appendNumber("reads", 20LL);
        sub.appendNumber("read_sectors", 40LL);
        sub.appendNumber("read_time_ms", 100LL);
        sub.appendNumber("writes", 50LL);
        sub.appendNumber("write_sectors", 80LL);
        sub.appendNumber("write_time_ms", 200LL);
        sub.appendNumber("io_time_ms", 400LL);
        sub.appendNumber("io_queued_ms", 600LL);
    }
    _metrics.update(baseline.obj());

    BSONObjBuilder next;
    {
        BSONObjBuilder sub(next.subobjStart(kSda));
        sub.appendNumber("reads", 50LL);
        sub.appendNumber("read_sectors", 100LL);
        sub.appendNumber("read_time_ms", 250LL);
        sub.appendNumber("writes", 110LL);
        sub.appendNumber("write_sectors", 220LL);
        sub.appendNumber("write_time_ms", 560LL);
        sub.appendNumber("io_time_ms", 1100LL);
        sub.appendNumber("io_queued_ms", 1650LL);
    }
    {
        BSONObjBuilder sub(next.subobjStart(kSdb));
        sub.appendNumber("reads", 23LL);
        sub.appendNumber("read_sectors", 46LL);
        sub.appendNumber("read_time_ms", 115LL);
        sub.appendNumber("writes", 55LL);
        sub.appendNumber("write_sectors", 90LL);
        sub.appendNumber("write_time_ms", 230LL);
        sub.appendNumber("io_time_ms", 440LL);
        sub.appendNumber("io_queued_ms", 660LL);
    }
    _metrics.update(next.obj());

    auto passkey = DynamicMetricNameTestPasskeyMaker::make();
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaReads, passkey)), 10);
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaWrites, passkey)), 10);
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdbWrites, passkey)), 5);
}

TEST_F(DiskOtelMetricsTest, SkipDisksWithInvalidNames) {
    // The valid disk (kSda) should create a metric, and the invalid disk
    // (kInvalidDisk) should be skipped.
    DiskMetrics metrics{{std::string(kSda), std::string(kInvalidDisk)}};

    metrics.update(makeDiskBson(kSda, 0, 0, 0, 0, 0, 0, 0, 0));
    metrics.update(makeDiskBson(kSda, 10, 20, 50, 20, 40, 100, 200, 300));

    auto passkey = DynamicMetricNameTestPasskeyMaker::make();
    ASSERT_EQ(_capturer.readInt64Counter(DynamicMetricNameMaker::make(kSdaWrites, passkey)), 20);

    // The metric name should never have been registered.
    ASSERT_THROWS_CODE(
        _capturer.readInt64Counter(DynamicMetricNameMaker::make(kInvalidDiskMetricName, passkey)),
        DBException,
        ErrorCodes::KeyNotFound);
}

}  // namespace
}  // namespace mongo
