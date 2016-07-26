/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include "mongo/util/perfctr_collect.h"

#include <boost/filesystem.hpp>
#include <map>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
using StringMap = std::map<std::string, uint64_t>;

/**
 * Convert BSON document nested up to 3 levels into a map where keys are dot separated paths to
 * values.
 */
StringMap toNestedStringMap(BSONObj& obj) {
    StringMap map;

    for (const auto& parent : obj) {

        if (parent.isABSONObj()) {
            std::string parentNamePrefix = std::string(parent.fieldName()) + ".";

            for (const auto& child : parent.Obj()) {

                if (child.isABSONObj()) {
                    std::string childNamePrefix = parentNamePrefix + child.fieldName() + ".";

                    for (const auto& grandChild : child.Obj()) {
                        map[childNamePrefix + grandChild.fieldName()] = grandChild.numberLong();
                    }

                } else {
                    map[parentNamePrefix + child.fieldName()] = child.numberLong();
                }
            }
        } else {
            map[parent.fieldName()] = parent.numberLong();
        }
    }

    return map;
}

#define ASSERT_KEY(_key) ASSERT_TRUE(stringMap.find(_key) != stringMap.end());
#define ASSERT_NO_KEY(_key) ASSERT_TRUE(stringMap.find(_key) == stringMap.end());

#define ASSERT_TIMEBASE ASSERT_KEY("timebase");
#define ASSERT_NO_TIMEBASE ASSERT_NO_KEY("timebase");

#define ASSERT_GROUP_AND_RAW_COUNTER(g, c) \
    ASSERT_KEY(g "." c);                   \
    ASSERT_NO_KEY(g "." c " Base");

#define ASSERT_GROUP_AND_NON_RAW_COUNTER(g, c) \
    ASSERT_KEY(g "." c);                       \
    ASSERT_KEY(g "." c " Base");

#define ASSERT_NESTED_GROUP_AND_NON_RAW_COUNTER(g, p, c) \
    ASSERT_KEY(g "." p "." c);                           \
    ASSERT_KEY(g "." p "." c " Base");

#define ASSERT_NO_NESTED_GROUP_AND_NON_RAW_COUNTER(g, p, c) \
    ASSERT_NO_KEY(g "." p "." c);                           \
    ASSERT_NO_KEY(g "." p "." c " Base");

#define COLLECT_COUNTERS_VERBOSE                             \
    BSONObjBuilder builder;                                  \
    ASSERT_OK(collector->collect(&builder));                 \
    auto obj = builder.obj();                                \
    log() << "OBJ:" << obj;                                  \
    auto stringMap = toNestedStringMap(obj);                 \
    for (const auto& kvp : stringMap) {                      \
        log() << "kvp " << kvp.first << " - " << kvp.second; \
    }

#define COLLECT_COUNTERS_QUIET               \
    BSONObjBuilder builder;                  \
    ASSERT_OK(collector->collect(&builder)); \
    auto obj = builder.obj();                \
    auto stringMap = toNestedStringMap(obj);

#define COLLECT_COUNTERS COLLECT_COUNTERS_QUIET

size_t kDefaultCollectionCount = 2;

// Simple verification test
TEST(FTDCPerfCollector, TestSingleCounter) {

    PerfCounterCollection collection;
    // PERF_100NSEC_TIMER
    ASSERT_OK(collection.addCountersGroup("cpu", {"\\Processor(0)\\% Idle Time"}));

    auto swCollector = PerfCounterCollector::create(std::move(collection));
    ASSERT_OK(swCollector.getStatus());
    auto collector = std::move(swCollector.getValue());

    for (size_t i = 0; i < kDefaultCollectionCount; i++) {
        COLLECT_COUNTERS;

        ASSERT_NO_TIMEBASE;
        ASSERT_GROUP_AND_NON_RAW_COUNTER("cpu", "\\Processor\\% Idle Time");
    }
}


// Simple verification test
TEST(FTDCPerfCollector, TestSingleRawCounter) {

    PerfCounterCollection collection;
    // PERF_COUNTER_RAWCOUNT
    ASSERT_OK(collection.addCountersGroup("cpu", {"\\System\\Processes"}));

    auto swCollector = PerfCounterCollector::create(std::move(collection));
    ASSERT_OK(swCollector.getStatus());
    auto collector = std::move(swCollector.getValue());

    for (size_t i = 0; i < kDefaultCollectionCount; i++) {
        COLLECT_COUNTERS;

        ASSERT_NO_TIMEBASE;
        ASSERT_GROUP_AND_RAW_COUNTER("cpu", "\\System\\Processes");
    }
}

// Test negative cases for collection
TEST(FTDCPerfCollector, TestBadCollectionInput) {

    PerfCounterCollection collection;
    ASSERT_OK(collection.addCountersGroup("cpu", {"\\Processor(0)\\% Idle Time"}));

    // Duplicate group
    ASSERT_NOT_OK(collection.addCountersGroup("cpu", {"\\Processor(0)\\% Idle Time"}));

    // Duplicate counter
    ASSERT_NOT_OK(collection.addCountersGroup(
        "cpu2",
        {
            "\\Processor(0)\\% Idle Time", "\\Processor(0)\\% Idle Time",
        }));

    // Duplicate group
    ASSERT_NOT_OK(
        collection.addCountersGroupedByInstanceName("cpu", {"\\Processor(0)\\% Idle Time"}));

    // Duplicate counter
    ASSERT_NOT_OK(collection.addCountersGroupedByInstanceName(
        "cpu2",
        {
            "\\Processor(0)\\% Idle Time", "\\Processor(0)\\% Idle Time",
        }));
}

// Test negative collector input
TEST(FTDCPerfCollector, TestBadCollectorInput) {
    // Bad counter name
    {
        PerfCounterCollection collection;
        ASSERT_OK(collection.addCountersGroup("cpu", {"\\Processor(0)\\DOES NOT EXIST"}));

        auto swCollector = PerfCounterCollector::create(std::move(collection));
        ASSERT_NOT_OK(swCollector.getStatus());
    }

    // Bad wild card
    {
        PerfCounterCollection collection;
        ASSERT_OK(collection.addCountersGroup("cpu", {"\\Processor(0)\\DOES*"}));

        auto swCollector = PerfCounterCollector::create(std::move(collection));
        ASSERT_NOT_OK(swCollector.getStatus());
    }

    // Use addCounterGroup with instance wildcard
    {
        PerfCounterCollection collection;
        ASSERT_OK(collection.addCountersGroup("cpu", {"\\Processor(*)\\\\% Idle Time"}));

        auto swCollector = PerfCounterCollector::create(std::move(collection));
        ASSERT_NOT_OK(swCollector.getStatus());
    }

    // Use addCountersGroupedByInstanceName without instance name
    {
        PerfCounterCollection collection;
        ASSERT_OK(collection.addCountersGroupedByInstanceName("cpu", {"\\System\\Processes"}));

        auto swCollector = PerfCounterCollector::create(std::move(collection));
        ASSERT_NOT_OK(swCollector.getStatus());
    }
}

// Test all the different counter types we use in the MongoDB code
TEST(FTDCPerfCollector, TestCounterTypes) {

    PerfCounterCollection collection;
    ASSERT_OK(collection.addCountersGroup(
        "misc",
        {
            "\\Processor(0)\\% Idle Time",                          // PERF_100NSEC_TIMER
            "\\Processor(0)\\% Processor Time",                     // PERF_100NSEC_TIMER_INV
            "\\System\\Processes",                                  // PERF_COUNTER_RAWCOUNT
            "\\System\\System Up Time",                             // PERF_ELAPSED_TIME
            "\\Memory\\Available Bytes",                            // PERF_COUNTER_LARGE_RAWCOUNT
            "\\PhysicalDisk(_Total)\\% Disk Write Time",            // PERF_PRECISION_100NS_TIMER
            "\\PhysicalDisk(_Total)\\Avg. Disk Bytes/Write",        // PERF_AVERAGE_BULK
            "\\PhysicalDisk(_Total)\\Avg. Disk Read Queue Length",  // PERF_COUNTER_LARGE_QUEUELEN_TYPE
            "\\PhysicalDisk(_Total)\\Avg. Disk sec/Write",          // PERF_AVERAGE_TIMER
            "\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",         // PERF_COUNTER_BULK_COUNT
            "\\PhysicalDisk(_Total)\\Disk Writes/sec",              // PERF_COUNTER_COUNTER
        }));

    auto swCollector = PerfCounterCollector::create(std::move(collection));
    ASSERT_OK(swCollector.getStatus());
    auto collector = std::move(swCollector.getValue());

    for (size_t i = 0; i < kDefaultCollectionCount; i++) {
        COLLECT_COUNTERS;

        ASSERT_TIMEBASE
        ASSERT_GROUP_AND_NON_RAW_COUNTER("misc", "\\Processor\\% Idle Time");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("misc", "\\Processor\\% Processor Time");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("misc", "\\System\\System Up Time");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("misc", "\\PhysicalDisk\\% Disk Write Time");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("misc", "\\PhysicalDisk\\Avg. Disk Bytes/Write");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("misc", "\\PhysicalDisk\\Avg. Disk Read Queue Length");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("misc", "\\PhysicalDisk\\Avg. Disk sec/Write");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("misc", "\\PhysicalDisk\\Disk Write Bytes/sec");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("misc", "\\PhysicalDisk\\Disk Writes/sec");

        ASSERT_GROUP_AND_RAW_COUNTER("misc", "\\System\\Processes");
    }
}

// Test multiple counter groups
TEST(FTDCPerfCollector, TestMultipleCounterGroups) {

    PerfCounterCollection collection;
    ASSERT_OK(collection.addCountersGroup(
        "cpu", {"\\Processor(0)\\% Idle Time", "\\Processor(0)\\% Processor Time"}));
    ASSERT_OK(
        collection.addCountersGroup("sys", {"\\System\\Processes", "\\System\\System Up Time"}));

    auto swCollector = PerfCounterCollector::create(std::move(collection));
    ASSERT_OK(swCollector.getStatus());
    auto collector = std::move(swCollector.getValue());

    for (size_t i = 0; i < kDefaultCollectionCount; i++) {
        COLLECT_COUNTERS;

        ASSERT_TIMEBASE
        ASSERT_GROUP_AND_NON_RAW_COUNTER("cpu", "\\Processor\\% Idle Time");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("cpu", "\\Processor\\% Processor Time");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("sys", "\\System\\System Up Time");

        ASSERT_GROUP_AND_RAW_COUNTER("sys", "\\System\\Processes");
    }
}

// Test multiple nested counter groups
TEST(FTDCPerfCollector, TestMultipleNestedCounterGroups) {

    PerfCounterCollection collection;
    ASSERT_OK(collection.addCountersGroupedByInstanceName(
        "cpu", {"\\Processor(*)\\% Idle Time", "\\Processor(*)\\% Processor Time"}));
    ASSERT_OK(
        collection.addCountersGroup("sys", {"\\System\\Processes", "\\System\\System Up Time"}));

    auto swCollector = PerfCounterCollector::create(std::move(collection));
    ASSERT_OK(swCollector.getStatus());
    auto collector = std::move(swCollector.getValue());

    for (size_t i = 0; i < kDefaultCollectionCount; i++) {
        COLLECT_COUNTERS;
        ASSERT_TIMEBASE

        // We boldly assume that machines we test on have at least two processors
        ASSERT_NESTED_GROUP_AND_NON_RAW_COUNTER("cpu", "0", "\\Processor\\% Idle Time");
        ASSERT_NESTED_GROUP_AND_NON_RAW_COUNTER("cpu", "0", "\\Processor\\% Processor Time");

        ASSERT_NESTED_GROUP_AND_NON_RAW_COUNTER("cpu", "1", "\\Processor\\% Idle Time");
        ASSERT_NESTED_GROUP_AND_NON_RAW_COUNTER("cpu", "1", "\\Processor\\% Processor Time");

        ASSERT_NO_NESTED_GROUP_AND_NON_RAW_COUNTER("cpu", "_Total", "\\Processor\\% Idle Time");
        ASSERT_NO_NESTED_GROUP_AND_NON_RAW_COUNTER(
            "cpu", "_Total", "\\Processor\\% Processor Time");

        ASSERT_GROUP_AND_NON_RAW_COUNTER("sys", "\\System\\System Up Time");
        ASSERT_GROUP_AND_RAW_COUNTER("sys", "\\System\\Processes");
    }
}

// Test Counters we use in MongoDB
TEST(FTDCPerfCollector, TestLocalCounters) {

    PerfCounterCollection collection;
    ASSERT_OK(collection.addCountersGroup("cpu",
                                          {
                                              "\\Processor(_Total)\\% Idle Time",
                                              "\\Processor(_Total)\\% Interrupt Time",
                                              "\\Processor(_Total)\\% Privileged Time",
                                              "\\Processor(_Total)\\% Processor Time",
                                              "\\Processor(_Total)\\% User Time",
                                              "\\Processor(_Total)\\Interrupts/sec",
                                              "\\System\\Context Switches/sec",
                                              "\\System\\Processes",
                                              "\\System\\Processor Queue Length",
                                              "\\System\\System Up Time",
                                              "\\System\\Threads",
                                          }));

    // TODO: Should we capture the Heap Counters for the current process?
    ASSERT_OK(collection.addCountersGroup("memory",
                                          {
                                              "\\Memory\\Available Bytes",
                                              "\\Memory\\Cache Bytes",
                                              "\\Memory\\Cache Faults/sec",
                                              "\\Memory\\Committed Bytes",
                                              "\\Memory\\Commit Limit",
                                              "\\Memory\\Page Reads/sec",
                                              "\\Memory\\Page Writes/sec",
                                              "\\Memory\\Pages Input/sec",
                                              "\\Memory\\Pages Output/sec",
                                              "\\Memory\\Pool Nonpaged Bytes",
                                              "\\Memory\\Pool Paged Bytes",
                                              "\\Memory\\Pool Paged Resident Bytes",
                                              "\\Memory\\System Cache Resident Bytes",
                                              "\\Memory\\System Code Total Bytes",
                                          }));

    ASSERT_OK(collection.addCountersGroupedByInstanceName(
        "disks",
        {
            "\\PhysicalDisk(*)\\% Disk Read Time",
            "\\PhysicalDisk(*)\\% Disk Write Time",
            "\\PhysicalDisk(*)\\Avg. Disk Bytes/Read",
            "\\PhysicalDisk(*)\\Avg. Disk Bytes/Write",
            "\\PhysicalDisk(*)\\Avg. Disk Read Queue Length",
            "\\PhysicalDisk(*)\\Avg. Disk Write Queue Length",
            "\\PhysicalDisk(*)\\Avg. Disk sec/Read",
            "\\PhysicalDisk(*)\\Avg. Disk sec/Write",
            "\\PhysicalDisk(*)\\Disk Read Bytes/sec",
            "\\PhysicalDisk(*)\\Disk Write Bytes/sec",
            "\\PhysicalDisk(*)\\Disk Reads/sec",
            "\\PhysicalDisk(*)\\Disk Writes/sec",
            "\\PhysicalDisk(*)\\Current Disk Queue Length",
        }));

    auto swCollector = PerfCounterCollector::create(std::move(collection));
    ASSERT_OK(swCollector.getStatus());
    auto collector = std::move(swCollector.getValue());

    for (size_t i = 0; i < kDefaultCollectionCount; i++) {
        COLLECT_COUNTERS;
        ASSERT_TIMEBASE

        ASSERT_GROUP_AND_NON_RAW_COUNTER("cpu", "\\Processor\\% Idle Time");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("cpu", "\\Processor\\% Interrupt Time");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("cpu", "\\Processor\\% Privileged Time");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("cpu", "\\Processor\\% Processor Time");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("cpu", "\\Processor\\% User Time");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("cpu", "\\Processor\\Interrupts/sec");

        ASSERT_GROUP_AND_NON_RAW_COUNTER("cpu", "\\System\\Context Switches/sec");
        ASSERT_GROUP_AND_RAW_COUNTER("cpu", "\\System\\Processes");
        ASSERT_GROUP_AND_RAW_COUNTER("cpu", "\\System\\Processor Queue Length");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("cpu", "\\System\\System Up Time");
        ASSERT_GROUP_AND_RAW_COUNTER("cpu", "\\System\\Threads");

        ASSERT_GROUP_AND_RAW_COUNTER("memory", "\\Memory\\Available Bytes");
        ASSERT_GROUP_AND_RAW_COUNTER("memory", "\\Memory\\Cache Bytes");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("memory", "\\Memory\\Cache Faults/sec");
        ASSERT_GROUP_AND_RAW_COUNTER("memory", "\\Memory\\Commit Limit");
        ASSERT_GROUP_AND_RAW_COUNTER("memory", "\\Memory\\Committed Bytes");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("memory", "\\Memory\\Page Reads/sec");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("memory", "\\Memory\\Page Writes/sec");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("memory", "\\Memory\\Pages Input/sec");
        ASSERT_GROUP_AND_NON_RAW_COUNTER("memory", "\\Memory\\Pages Output/sec");
        ASSERT_GROUP_AND_RAW_COUNTER("memory", "\\Memory\\Pool Paged Resident Bytes");
        ASSERT_GROUP_AND_RAW_COUNTER("memory", "\\Memory\\System Cache Resident Bytes");
        ASSERT_GROUP_AND_RAW_COUNTER("memory", "\\Memory\\System Code Total Bytes");
    }
}

}  // namespace
}  // namespace mongo
