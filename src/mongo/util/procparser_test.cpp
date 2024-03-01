/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include <array>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <system_error>
#include <unistd.h>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/procparser.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

using namespace fmt::literals;

namespace {

class BaseProcTest : public unittest::Test {
public:
    StringMap<uint64_t> toStringMap(BSONObj obj) {
        StringMap<uint64_t> map;

        for (const auto& e : obj) {
            if (e.isABSONObj()) {
                std::string prefix = std::string(e.fieldName()) + ".";

                for (const auto& child : e.Obj()) {
                    map[prefix + child.fieldName()] = child.numberLong();
                }
            } else {
                map[e.fieldName()] = e.numberLong();
            }
        }

        return map;
    }

    template <typename T>
    bool contains(const StringMap<T>& container, StringData key) {
        return container.find(key) != container.end();
    }

    StringMap<uint64_t> uint64Map;
};

class FTDCProcStat : public BaseProcTest {
public:
    void parseStat(const std::vector<StringData>& keys, StringData input) {
        BSONObjBuilder builder;
        ASSERT_OK(procparser::parseProcStat(keys, input, 1000, &builder));
        uint64Map = toStringMap(builder.done());
    }

    std::vector<StringData> keys{"cpu", "ctxt", "processes"};
};

TEST_F(FTDCProcStat, TestStat) {

    parseStat(keys,
              "cpu  41801 9179 32206 831134223 34279 0 947 0 0 0\n"
              "cpu0 2977 450 2475 69253074 1959 0 116 0 0 0\n"
              "cpu1 6213 4261 9400 69177349 845 0 539 0 0 0\n"
              "cpu2 1949 831 3699 69261035 645 0 0 0 0 0\n"
              "cpu3 2222 644 3283 69264801 783 0 0 0 0 0\n"
              "cpu4 16576 607 4757 69232589 8195 0 291 0 0 0\n"
              "cpu5 3742 391 4571 69257332 2322 0 0 0 0 0\n"
              "cpu6 2173 376 743 69284308 400 0 0 0 0 0\n"
              "cpu7 1232 375 704 69285753 218 0 0 0 0 0\n"
              "cpu8 960 127 576 69262851 18107 0 0 0 0 0\n"
              "cpu9 1755 227 744 69283938 362 0 0 0 0 0\n"
              "cpu10 1380 641 678 69285193 219 0 0 0 0 0\n"
              "cpu11 618 244 572 69285995 218 0 0 0 0 0\n"
              "intr 54084718 135 2 ....\n"
              "ctxt 190305514\n"
              "btime 1463584038\n"
              "processes 47438\n"
              "procs_running 1\n"
              "procs_blocked 0\n"
              "softirq 102690251 8 26697410 115481 23345078 816026 0 2296 26068778 0 25645174\n");
    ASSERT_EQ(uint64Map.at("user_ms"), 41801UL);
    ASSERT_EQ(uint64Map.at("nice_ms"), 9179UL);
    ASSERT_EQ(uint64Map.at("system_ms"), 32206UL);
    ASSERT_EQ(uint64Map.at("idle_ms"), 831134223UL);
    ASSERT_EQ(uint64Map.at("iowait_ms"), 34279UL);
    ASSERT_EQ(uint64Map.at("irq_ms"), 0UL);
    ASSERT_EQ(uint64Map.at("softirq_ms"), 947UL);
    ASSERT_EQ(uint64Map.at("steal_ms"), 0UL);
    ASSERT_EQ(uint64Map.at("guest_ms"), 0UL);
    ASSERT_EQ(uint64Map.at("guest_nice_ms"), 0UL);
    ASSERT_EQ(uint64Map.at("ctxt"), 190305514UL);
    ASSERT_EQ(uint64Map.at("processes"), 47438UL);
}

TEST_F(FTDCProcStat, TestMissingField) {
    parseStat(keys,
              "cpu  41801 9179 32206\n"
              "ctxt 190305514\n");
    ASSERT_EQ(uint64Map.at("user_ms"), 41801UL);
    ASSERT_EQ(uint64Map.at("nice_ms"), 9179UL);
    ASSERT_EQ(uint64Map.at("system_ms"), 32206UL);
    ASSERT(!contains(uint64Map, "idle_ms"));
    ASSERT_EQ(uint64Map.at("ctxt"), 190305514UL);
    ASSERT(!contains(uint64Map, "processes"));
}

TEST_F(FTDCProcStat, TestMoreMissingFields) {
    parseStat(keys,
              "cpu  41801\n"
              "ctxt 190305514\n");
    ASSERT_EQ(uint64Map.at("user_ms"), 41801UL);
    ASSERT(!contains(uint64Map, "nice_ms"));
    ASSERT_EQ(uint64Map.at("ctxt"), 190305514UL);
    ASSERT(!contains(uint64Map, "processes"));
}

TEST_F(FTDCProcStat, TestMissingCpuField) {
    parseStat(keys,
              "cpu  \n"
              "ctxt 190305514\n");
    ASSERT_EQ(uint64Map.at("ctxt"), 190305514UL);
    ASSERT(!contains(uint64Map, "processes"));
}

TEST_F(FTDCProcStat, TestWithLessFields) {
    parseStat(keys, "cpu 41801 9179 32206");
    ASSERT_EQ(uint64Map.at("user_ms"), 41801UL);
    ASSERT_EQ(uint64Map.at("nice_ms"), 9179UL);
    ASSERT_EQ(uint64Map.at("system_ms"), 32206UL);
    ASSERT(!contains(uint64Map, "idle_ms"));
}

TEST_F(FTDCProcStat, TestWithOnlyCpu) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(procparser::parseProcStat(keys, "cpu", 1000, &builder));
}

TEST_F(FTDCProcStat, TestWithEvenLessFields) {
    parseStat(keys,
              "cpu  123\n"
              "ctxt");
    ASSERT_EQ(uint64Map.at("user_ms"), 123UL);
}

TEST_F(FTDCProcStat, TestEmpty) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(procparser::parseProcStat(keys, "", 1000, &builder));
}


// Test we can parse the /proc/stat on this machine. Also assert we have the expected fields
// This tests is designed to exercise our parsing code on various Linuxes and fail
// Normally when run in the FTDC loop we return a non-fatal error so we may not notice the failure
// otherwise.
TEST_F(FTDCProcStat, TestLocalStat) {
    std::vector<StringData> keys{
        "btime",
        "cpu",
        "ctxt",
        "processes",
        "procs_blocked",
        "procs_running",
    };

    BSONObjBuilder builder;

    ASSERT_OK(procparser::parseProcStatFile("/proc/stat", keys, &builder));

    BSONObj obj = builder.obj();
    auto uint64Map = toStringMap(obj);
    LOGV2(23364, "OBJ:{obj}", "obj"_attr = obj);
    ASSERT(contains(uint64Map, "user_ms"));
    ASSERT(contains(uint64Map, "nice_ms"));
    ASSERT(contains(uint64Map, "idle_ms"));
    ASSERT(contains(uint64Map, "system_ms"));
    ASSERT(contains(uint64Map, "iowait_ms"));
    ASSERT(contains(uint64Map, "irq_ms"));
    ASSERT(contains(uint64Map, "softirq_ms"));
    ASSERT(contains(uint64Map, "steal_ms"));
    // Needs 2.6.24 - ASSERT(contains(uint64Map, "guest_ms"));
    // Needs 2.6.33 - ASSERT(contains(uint64Map, "guest_nice_ms"));
    ASSERT(contains(uint64Map, "ctxt"));
    ASSERT(contains(uint64Map, "btime"));
    ASSERT(contains(uint64Map, "processes"));
    ASSERT(contains(uint64Map, "procs_running"));
    ASSERT(contains(uint64Map, "procs_blocked"));
}

TEST_F(FTDCProcStat, TestLocalNonExistentStat) {
    std::vector<StringData> keys{
        "btime",
        "cpu",
        "ctxt",
        "processes",
        "procs_blocked",
        "procs_running",
    };
    BSONObjBuilder builder;

    ASSERT_NOT_OK(procparser::parseProcStatFile("/proc/does_not_exist", keys, &builder));
}

class FTDCProcMemInfo : public BaseProcTest {
public:
    void parseMeminfo(const std::vector<StringData>& keys, StringData input) {
        BSONObjBuilder builder;
        ASSERT_OK(procparser::parseProcMemInfo(keys, input, &builder));
        uint64Map = toStringMap(builder.done());
    }

    std::vector<StringData> keys{"Key1", "Key2", "Key3"};
};

TEST_F(FTDCProcMemInfo, TestMemInfo) {
    parseMeminfo(keys, "Key1: 123 kB\nKey2: 456 kB");
    ASSERT_EQ(uint64Map.at("Key1_kb"), 123UL);
    ASSERT_EQ(uint64Map.at("Key2_kb"), 456UL);
}

TEST_F(FTDCProcMemInfo, TestSpaceInKey) {
    parseMeminfo(keys, "Key1: 123 kB\nKey 2: 456 kB");
    ASSERT_EQ(uint64Map.at("Key1_kb"), 123UL);
    ASSERT(!contains(uint64Map, "Key2_kb"));
}

TEST_F(FTDCProcMemInfo, TestNoNewline) {
    parseMeminfo(keys, "Key1: 123 kB Key2: 456 kB");
    ASSERT_EQ(uint64Map.at("Key1_kb"), 123UL);
    ASSERT(!contains(uint64Map, "Key2_kb"));
}

TEST_F(FTDCProcMemInfo, TestMissingColon) {
    parseMeminfo(keys, "Key1 123 kB\nKey2: 456 kB");
    ASSERT_EQ(uint64Map.at("Key1_kb"), 123UL);
    ASSERT_EQ(uint64Map.at("Key2_kb"), 456UL);
}

TEST_F(FTDCProcMemInfo, TestMissingKb) {
    parseMeminfo(keys, "Key1: 123 kB\nKey2: 456\nKey3: 789 kB\nKey4: 789 kB");
    ASSERT_EQ(uint64Map.at("Key1_kb"), 123UL);
    ASSERT_EQ(uint64Map.at("Key2"), 456UL);
    ASSERT_EQ(uint64Map.at("Key3_kb"), 789UL);
    ASSERT(!contains(uint64Map, "Key4_kb"));
}

TEST_F(FTDCProcMemInfo, TestEmptyString) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(procparser::parseProcMemInfo(keys, "", &builder));
}


// Test we can parse the /proc/meminfo on this machine. Also assert we have the expected fields
// This tests is designed to exercise our parsing code on various Linuxes and fail
// Normally when run in the FTDC loop we return a non-fatal error so we may not notice the failure
// otherwise.
TEST_F(FTDCProcMemInfo, TestLocalMemInfo) {
    std::vector<StringData> keys{
        "Active",         "Active(anon)",
        "Active(file)",   "AnonHugePages",
        "AnonPages",      "Bounce",
        "Buffers",        "Cached",
        "CmaFree",        "CmaTotal",
        "CommitLimit",    "Committed_AS",
        "Dirty",          "HardwareCorrupted",
        "Inactive",       "Inactive(anon)",
        "Inactive(file)", "KernelStack",
        "Mapped",         "MemAvailable",
        "MemFree",        "MemTotal",
        "Mlocked",        "NFS_Unstable",
        "PageTables",     "SReclaimable",
        "SUnreclaim",     "Shmem",
        "Slab",           "SwapCached",
        "SwapFree",       "SwapTotal",
        "Unevictable",    "VmallocChunk",
        "VmallocTotal",   "VmallocUsed",
        "Writeback",      "WritebackTmp",
    };

    BSONObjBuilder builder;

    ASSERT_OK(procparser::parseProcMemInfoFile("/proc/meminfo", keys, &builder));

    BSONObj obj = builder.obj();
    auto uint64Map = toStringMap(obj);
    LOGV2(23365, "OBJ:{obj}", "obj"_attr = obj);
    ASSERT(contains(uint64Map, "MemTotal_kb"));
    ASSERT(contains(uint64Map, "MemFree_kb"));
    // Needs in 3.15+ - ASSERT(contains(uint64Map, "MemAvailable_kb");
    ASSERT(contains(uint64Map, "Buffers_kb"));
    ASSERT(contains(uint64Map, "Cached_kb"));
    ASSERT(contains(uint64Map, "SwapCached_kb"));
    ASSERT(contains(uint64Map, "Active_kb"));
    ASSERT(contains(uint64Map, "Inactive_kb"));
    // Needs 2.6.28+ - ASSERT(contains(uint64Map, "Active(anon)_kb"));
    // Needs 2.6.28+ - ASSERT(contains(uint64Map, "Inactive(anon)_kb"));
    // Needs 2.6.28+ - ASSERT(contains(uint64Map, "Active(file)_kb"));
    // Needs 2.6.28+ - ASSERT(contains(uint64Map, "Inactive(file)_kb"));
    // Needs 2.6.28+ - ASSERT(contains(uint64Map, "Unevictable_kb"));
    // Needs 2.6.28+ - ASSERT(contains(uint64Map, "Mlocked_kb"));
    ASSERT(contains(uint64Map, "SwapTotal_kb"));
    ASSERT(contains(uint64Map, "SwapFree_kb"));
    ASSERT(contains(uint64Map, "Dirty_kb"));
    ASSERT(contains(uint64Map, "Writeback_kb"));
    ASSERT(contains(uint64Map, "AnonPages_kb"));
    ASSERT(contains(uint64Map, "Mapped_kb"));
    // Needs 2.6.32+ - ASSERT(contains(uint64Map, "Shmem_kb"));
    ASSERT(contains(uint64Map, "Slab_kb"));
    // Needs 2.6.19+ - ASSERT(contains(uint64Map, "SReclaimable_kb"));
    // Needs 2.6.19+ - ASSERT(contains(uint64Map, "SUnreclaim_kb"));
    // Needs 2.6.32+ - ASSERT(contains(uint64Map, "KernelStack_kb"));
    ASSERT(contains(uint64Map, "PageTables_kb"));
    ASSERT(contains(uint64Map, "NFS_Unstable_kb"));
    ASSERT(contains(uint64Map, "Bounce_kb"));
    // Needs 2.6.19+ - ASSERT(contains(uint64Map, "WritebackTmp_kb"));
    ASSERT(contains(uint64Map, "CommitLimit_kb"));
    ASSERT(contains(uint64Map, "Committed_AS_kb"));
    ASSERT(contains(uint64Map, "VmallocTotal_kb"));
    ASSERT(contains(uint64Map, "VmallocUsed_kb"));
    ASSERT(contains(uint64Map, "VmallocChunk_kb"));
    // Needs CONFIG_MEMORY_FAILURE & 2.6.32+ ASSERT(contains(uint64Map,
    // "HardwareCorrupted_kb")); Needs CONFIG_TRANSPARENT_HUGEPAGE -
    // ASSERT(contains(uint64Map, "AnonHugePages_kb")); Needs CONFIG_CMA & 3.19+ -
    // ASSERT(contains(uint64Map, "CmaTotal_kb")); Needs CONFIG_CMA & 3.19+ -
    // ASSERT(contains(uint64Map, "CmaFree_kb"));
}

TEST_F(FTDCProcMemInfo, TestLocalNonExistentMemInfo) {
    std::vector<StringData> keys{};
    BSONObjBuilder builder;

    ASSERT_NOT_OK(procparser::parseProcMemInfoFile("/proc/does_not_exist", keys, &builder));
}

class FTDCProcNetstat : public BaseProcTest {
public:
    void parseNetstat(const std::vector<StringData>& keys, StringData input) {
        BSONObjBuilder builder;
        ASSERT_OK(procparser::parseProcNetstat(keys, input, &builder));
        uint64Map = toStringMap(builder.done());
    }

    std::vector<StringData> keys{"pfx1", "pfx2", "pfx3"};
};

TEST_F(FTDCProcNetstat, TestNetstat) {
    parseNetstat(keys,
                 "pfx1 key1 key2 key3\n"
                 "pfx1 1 2 3\n"
                 "pfxX key1 key2\n"
                 "pfxX key1 key2\n"
                 "pfx2 key4 key5\n"
                 "pfx2 4 5\n");
    ASSERT_EQ(uint64Map.at("pfx1key1"), 1UL);
    ASSERT_EQ(uint64Map.at("pfx1key2"), 2UL);
    ASSERT(!contains(uint64Map, "pfxXkey1"));
    ASSERT(!contains(uint64Map, "pfxXkey2"));
    ASSERT_EQ(uint64Map.at("pfx1key3"), 3UL);
    ASSERT_EQ(uint64Map.at("pfx2key4"), 4UL);
    ASSERT_EQ(uint64Map.at("pfx2key5"), 5UL);
}

TEST_F(FTDCProcNetstat, TestMismatchedKeysAndValues) {
    parseNetstat(keys,
                 "pfx1 key1 key2 key3\n"
                 "pfx1 1 2 3 4\n"
                 "pfx2 key4 key5\n"
                 "pfx2 4\n"
                 "pfx3 key6 key7\n");
    ASSERT_EQ(uint64Map.at("pfx1key1"), 1UL);
    ASSERT_EQ(uint64Map.at("pfx1key2"), 2UL);
    ASSERT_EQ(uint64Map.at("pfx1key3"), 3UL);
    ASSERT(!contains(uint64Map, "pfx1key4"));
    ASSERT_EQ(uint64Map.at("pfx2key4"), 4UL);
    ASSERT(!contains(uint64Map, "pfx2key5"));
    ASSERT(!contains(uint64Map, "pfx3key6"));
    ASSERT(!contains(uint64Map, "pfx3key7"));
}

TEST_F(FTDCProcNetstat, TestNonNumericValues) {
    parseNetstat(keys,
                 "pfx1 key1 key2 key3\n"
                 "pfx1 1 foo 3\n");
    ASSERT_EQ(uint64Map.at("pfx1key1"), 1UL);
    ASSERT(!contains(uint64Map, "pfx1key2"));
    ASSERT_EQ(uint64Map.at("pfx1key3"), 3UL);
}

TEST_F(FTDCProcNetstat, TestNoNewline) {
    parseNetstat(keys,
                 "pfx1 key1 key2 key3\n"
                 "pfx1 1 2 3\n"
                 "pfx2 key4 key5\n"
                 "pfx2 4 5");
    ASSERT_EQ(uint64Map.at("pfx1key1"), 1UL);
    ASSERT_EQ(uint64Map.at("pfx1key2"), 2UL);
    ASSERT_EQ(uint64Map.at("pfx1key3"), 3UL);
    ASSERT_EQ(uint64Map.at("pfx2key4"), 4UL);
    ASSERT_EQ(uint64Map.at("pfx2key5"), 5UL);
}

TEST_F(FTDCProcNetstat, TestSingleLine) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(procparser::parseProcNetstat(keys, "pfx1 key1 key2 key3\n", &builder));
}

TEST_F(FTDCProcNetstat, TestEmptyString) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(procparser::parseProcNetstat(keys, "", &builder));
}

// Test we can parse the /proc/net/netstat on this machine and assert we have some expected fields
// Some keys can vary between distros, so we test only for the existence of a few basic ones
TEST_F(FTDCProcNetstat, TestLocalNetstat) {

    BSONObjBuilder builder;

    std::vector<StringData> keys{"TcpExt:"_sd, "IpExt:"_sd};

    ASSERT_OK(procparser::parseProcNetstatFile(keys, "/proc/net/netstat", &builder));

    BSONObj obj = builder.obj();
    auto uint64Map = toStringMap(obj);
    LOGV2(23366, "OBJ:{obj}", "obj"_attr = obj);
    ASSERT(contains(uint64Map, "TcpExt:TCPTimeouts"));
    ASSERT(contains(uint64Map, "TcpExt:TCPPureAcks"));
    ASSERT(contains(uint64Map, "TcpExt:TCPAbortOnTimeout"));
    ASSERT(contains(uint64Map, "TcpExt:EmbryonicRsts"));
    ASSERT(contains(uint64Map, "TcpExt:ListenDrops"));
    ASSERT(contains(uint64Map, "TcpExt:ListenOverflows"));
    ASSERT(contains(uint64Map, "TcpExt:DelayedACKs"));
    ASSERT(contains(uint64Map, "IpExt:OutOctets"));
    ASSERT(contains(uint64Map, "IpExt:InOctets"));
}

// Test we can parse the /proc/net/snmp on this machine and assert we have some expected fields
// Some keys can vary between distros, so we test only for the existence of a few basic ones
TEST_F(FTDCProcNetstat, TestLocalNetSnmp) {

    BSONObjBuilder builder;

    std::vector<StringData> keys{"Tcp:"_sd, "Ip:"_sd};

    ASSERT_OK(procparser::parseProcNetstatFile(keys, "/proc/net/snmp", &builder));

    BSONObj obj = builder.obj();
    auto uint64Map = toStringMap(obj);
    LOGV2(23367, "OBJ:{obj}", "obj"_attr = obj);
    ASSERT(contains(uint64Map, "Ip:InReceives"));
    ASSERT(contains(uint64Map, "Ip:OutRequests"));
    ASSERT(contains(uint64Map, "Tcp:InSegs"));
    ASSERT(contains(uint64Map, "Tcp:OutSegs"));
}

TEST_F(FTDCProcNetstat, TestLocalNonExistentNetstat) {
    std::vector<StringData> keys{};
    BSONObjBuilder builder;

    ASSERT_NOT_OK(procparser::parseProcNetstatFile(keys, "/proc/does_not_exist", &builder));
}

class FTDCProcDiskStats : public BaseProcTest {
public:
    void parseDiskStat(const std::vector<StringData>& disks, StringData input) {
        BSONObjBuilder builder;
        ASSERT_OK(procparser::parseProcDiskStats(disks, input, &builder));
        uint64Map = toStringMap(builder.done());
    }

    std::vector<StringData> disks{"dm-1", "sda", "sdb"};
};

TEST_F(FTDCProcDiskStats, TestDiskStats) {
    parseDiskStat(
        disks,
        "   8       0 sda 120611 33630 6297628 96550 349797 167398 11311562 2453603 0 117514 "
        "2554160\n"
        "   8       1 sda1 138 37 8642 315 3 0 18 14 0 292 329\n"
        "   8       2 sda2 120409 33593 6285754 96158 329029 167398 11311544 2450573 0 115611 "
        "2550739\n"
        "   8      16 sdb 12707 3876 1525418 57507 997 3561 297576 97976 0 37870 155619\n"
        "   8      17 sdb1 12601 3876 1521090 57424 992 3561 297576 97912 0 37738 155468\n"
        "  11       0 sr0 0 0 0 0 0 0 0 0 0 0 0\n"
        "2253       0 dm-0 154910 0 6279522 177681 506513 0 11311544 5674418 0 117752 5852275\n"
        "2253       1 dm-1 109 0 4584 226 0 0 0 0 0 172 226");
    ASSERT_EQ(uint64Map.at("sda.reads"), 120611UL);
    ASSERT_EQ(uint64Map.at("sda.writes"), 349797UL);
    ASSERT_EQ(uint64Map.at("sda.io_queued_ms"), 2554160UL);
    ASSERT_EQ(uint64Map.at("sdb.reads"), 12707UL);
    ASSERT_EQ(uint64Map.at("sdb.writes"), 997UL);
    ASSERT_EQ(uint64Map.at("sdb.io_queued_ms"), 155619UL);
    ASSERT_EQ(uint64Map.at("dm-1.reads"), 109UL);
    ASSERT_EQ(uint64Map.at("dm-1.writes"), 0UL);
    ASSERT_EQ(uint64Map.at("dm-1.io_queued_ms"), 226UL);
}

TEST_F(FTDCProcDiskStats, TestDiskStatsNoActivity) {
    parseDiskStat(
        disks,
        "   8       0 sda 120611 33630 6297628 96550 349797 167398 11311562 2453603 0 117514 "
        "2554160\n"
        "   8       1 sda1 138 37 8642 315 3 0 18 14 0 292 329\n"
        "   8       2 sda2 120409 33593 6285754 96158 329029 167398 11311544 2450573 0 115611 "
        "2550739\n"
        "   8      16 sdb 0 0 0 0 0 0 0 0 0 0 0\n"
        "   8      17 sdb1 12601 3876 1521090 57424 992 3561 297576 97912 0 37738 155468\n"
        "  11       0 sr0 0 0 0 0 0 0 0 0 0 0 0\n"
        "2253       0 dm-0 154910 0 6279522 177681 506513 0 11311544 5674418 0 117752 5852275\n"
        "2253       1 dm-1 109 0 4584 226 0 0 0 0 0 172 226");
    ASSERT_EQ(uint64Map.at("sda.reads"), 120611UL);
    ASSERT_EQ(uint64Map.at("sda.writes"), 349797UL);
    ASSERT_EQ(uint64Map.at("sda.io_queued_ms"), 2554160UL);
    ASSERT(!contains(uint64Map, "sdb.reads"));
    ASSERT(!contains(uint64Map, "sdb.writes"));
    ASSERT(!contains(uint64Map, "sdb.io_queued_ms"));
    ASSERT_EQ(uint64Map.at("dm-1.reads"), 109UL);
    ASSERT_EQ(uint64Map.at("dm-1.writes"), 0UL);
    ASSERT_EQ(uint64Map.at("dm-1.io_queued_ms"), 226UL);
}

TEST_F(FTDCProcDiskStats, TestDiskStatsLessNumbers) {
    parseDiskStat(disks, "8       0 sda 120611 33630 6297628 96550 349797 ");
}

TEST_F(FTDCProcDiskStats, TestDiskStatsNoNumbers) {
    parseDiskStat(disks, "8       0 sda");
}

TEST_F(FTDCProcDiskStats, TestDiskStatsShortStrings) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(procparser::parseProcDiskStats(disks, "8       0", &builder));
    ASSERT_NOT_OK(procparser::parseProcDiskStats(disks, "8", &builder));
    ASSERT_NOT_OK(procparser::parseProcDiskStats(disks, "", &builder));
}

TEST_F(FTDCProcDiskStats, TestLocalNonExistentStat) {
    BSONObjBuilder builder;

    ASSERT_NOT_OK(procparser::parseProcDiskStatsFile("/proc/does_not_exist", disks, &builder));
}

TEST_F(FTDCProcDiskStats, TestFindNonExistentPath) {
    auto disks = procparser::findPhysicalDisks("/proc/does_not_exist");
    ASSERT_EQUALS(0UL, disks.size());
}

TEST_F(FTDCProcDiskStats, TestFindPathNoPermission) {
    auto disks = procparser::findPhysicalDisks("/sys/kernel/debug");
    ASSERT_EQUALS(0UL, disks.size());
}

// Test we can parse the /proc/diskstats on this machine. Also assert we have the expected fields
// This tests is designed to exercise our parsing code on various Linuxes and fail
// Normally when run in the FTDC loop we return a non-fatal error so we may not notice the failure
// otherwise.
TEST_F(FTDCProcDiskStats, TestLocalDiskStats) {
    auto disks = procparser::findPhysicalDisks("/sys/block");

    std::vector<StringData> disks2;
    for (const auto& disk : disks) {
        LOGV2(23368, "DISK:{disk}", "disk"_attr = disk);
        disks2.emplace_back(disk);
    }

    ASSERT_NOT_EQUALS(0UL, disks.size());

    BSONObjBuilder builder;

    ASSERT_OK(procparser::parseProcDiskStatsFile("/proc/diskstats", disks2, &builder));

    BSONObj obj = builder.obj();
    auto uint64Map = toStringMap(obj);
    LOGV2(23369, "OBJ:{obj}", "obj"_attr = obj);

    bool foundDisk = false;

    for (const auto& disk : disks) {
        std::string prefix(disk);
        prefix += ".";

        auto reads = prefix + "reads";
        auto io_queued_ms = prefix + "io_queued_ms";

        // Make sure that if have the first field, then we have the last field.
        if (uint64Map.find(reads) != uint64Map.end()) {
            foundDisk = true;
            if (uint64Map.find(io_queued_ms) == uint64Map.end()) {
                FAIL(std::string("Inconsistency for ") + disk);
            }
        }
    }

    if (!foundDisk) {
        FAIL("Did not find any interesting disks on this machine.");
    }
}

class FTDCProcMountStats : public BaseProcTest {
public:
    static boost::filesystem::space_info mockGetSpace(const boost::filesystem::path& p,
                                                      boost::system::error_code& ec) {
        ec = boost::system::error_code();
        auto result = boost::filesystem::space_info();
        if (p.string() == "/") {
            result.available = 11213234231;
            result.capacity = 23432543255;
            result.free = 12387912837;
        } else if (p.string() == "/boot") {
            result.available = result.free = 777;
            result.capacity = 888;
        } else if (p.string() == "/home/ubuntu") {
            result.available = result.free = 0;
            result.capacity = 999;
        } else if (p.string() == "/opt") {
            result.available = result.free = result.capacity = 0;
        } else if (p.string() == "/var") {
            ec.assign(1, ec.category());
        }
        return result;
    }

    void parseNetstat(StringData input) {
        BSONObjBuilder builder;
        ASSERT_OK(procparser::parseProcSelfMountStatsImpl(input, &builder, &mockGetSpace));
        obj = builder.obj();
    }

    BSONObj obj;
};

TEST_F(FTDCProcMountStats, TestMountStatsHappyPath) {
    // clang-format off
    parseNetstat("25 30 0:23 / /sys rw,nosuid,nodev,noexec,relatime shared:7 - sysfs sysfs rw\n"
        "26 30 0:5 / /proc rw,nosuid,nodev,noexec,relatime shared:13 - proc proc rw\n"
        "27 30 0:6 / /dev rw,nosuid,relatime shared:2 - devtmpfs udev rw,size=8033308k,nr_inodes=2008327,mode=755\n"
        "28 27 0:24 / /dev/pts rw,nosuid,noexec,relatime shared:3 - devpts devpts rw,gid=5,mode=620,ptmxmode=000\n"
        "29 30 0:25 / /run rw,nosuid,noexec,relatime shared:5 - tmpfs tmpfs rw,size=1609528k,mode=755\n"
        "30 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p1 rw,discard\n"
        "31 25 0:7 / /sys/kernel/security rw,nosuid,nodev,noexec,relatime shared:8 - securityfs securityfs rw\n"
        "32 27 0:26 / /dev/shm rw,nosuid,nodev shared:4 - tmpfs tmpfs rw\n"
        "33 29 0:27 / /run/lock rw,nosuid,nodev,noexec,relatime shared:6 - tmpfs tmpfs rw,size=5120k\n"
        "34 25 0:28 / /sys/fs/cgroup ro,nosuid,nodev,noexec shared:9 - tmpfs tmpfs ro,mode=755\n"
        "35 34 0:29 / /sys/fs/cgroup/unified rw,nosuid,nodev,noexec,relatime shared:10 - cgroup2 cgroup rw\n"
        "36 34 0:30 / /sys/fs/cgroup/systemd rw,nosuid,nodev,noexec,relatime shared:11 - cgroup cgroup rw,xattr,name=systemd\n"
        "37 25 0:31 / /sys/fs/pstore rw,nosuid,nodev,noexec,relatime shared:12 - pstore pstore rw\n"
        "38 34 0:32 / /sys/fs/cgroup/rdma rw,nosuid,nodev,noexec,relatime shared:14 - cgroup cgroup rw,rdma\n"
        "39 34 0:33 / /sys/fs/cgroup/net_cls,net_prio rw,nosuid,nodev,noexec,relatime shared:15 - cgroup cgroup rw,net_cls,net_prio\n"
        "40 34 0:34 / /sys/fs/cgroup/perf_event rw,nosuid,nodev,noexec,relatime shared:16 - cgroup cgroup rw,perf_event\n"
        "41 34 0:35 / /sys/fs/cgroup/cpu,cpuacct rw,nosuid,nodev,noexec,relatime shared:17 - cgroup cgroup rw,cpu,cpuacct\n"
        "42 34 0:36 / /sys/fs/cgroup/freezer rw,nosuid,nodev,noexec,relatime shared:18 - cgroup cgroup rw,freezer\n"
        "43 34 0:37 / /sys/fs/cgroup/cpuset rw,nosuid,nodev,noexec,relatime shared:19 - cgroup cgroup rw,cpuset\n"
        "44 34 0:38 / /sys/fs/cgroup/devices rw,nosuid,nodev,noexec,relatime shared:20 - cgroup cgroup rw,devices\n"
        "45 34 0:39 / /sys/fs/cgroup/memory rw,nosuid,nodev,noexec,relatime shared:21 - cgroup cgroup rw,memory\n"
        "46 34 0:40 / /sys/fs/cgroup/pids rw,nosuid,nodev,noexec,relatime shared:22 - cgroup cgroup rw,pids\n"
        "47 34 0:41 / /sys/fs/cgroup/blkio rw,nosuid,nodev,noexec,relatime shared:23 - cgroup cgroup rw,blkio\n"
        "48 34 0:42 / /sys/fs/cgroup/hugetlb rw,nosuid,nodev,noexec,relatime shared:24 - cgroup cgroup rw,hugetlb\n"
        "49 27 0:21 / /dev/mqueue rw,relatime shared:25 - mqueue mqueue rw\n"
        "50 26 0:43 / /proc/sys/fs/binfmt_misc rw,relatime shared:26 - autofs systemd-1 rw,fd=32,pgrp=1,timeout=0,minproto=5,maxproto=5,direct,pipe_ino=3379\n"
        "51 27 0:44 / /dev/hugepages rw,relatime shared:27 - hugetlbfs hugetlbfs rw,pagesize=2M\n"
        "52 29 0:45 / /run/rpc_pipefs rw,relatime shared:28 - rpc_pipefs sunrpc rw\n"
        "53 25 0:8 / /sys/kernel/debug rw,relatime shared:29 - debugfs debugfs rw\n"
        "54 25 0:46 / /sys/fs/fuse/connections rw,relatime shared:30 - fusectl fusectl rw\n"
        "55 25 0:22 / /sys/kernel/config rw,relatime shared:31 - configfs configfs rw\n"
        "90 30 7:2 / /snap/core18/2128 ro,nodev,relatime shared:33 - squashfs /dev/loop2 ro\n"
        "94 30 259:0 / /home/ubuntu rw,noatime shared:35 - xfs /dev/nvme1n1 rw,attr2,inode64,logbufs=8,logbsize=32k,noquota\n"
        "96 50 0:47 / /proc/sys/fs/binfmt_misc rw,relatime shared:36 - binfmt_misc binfmt_misc rw\n"
        "192 30 7:3 / /snap/amazon-ssm-agent/3552 ro,nodev,relatime shared:38 - squashfs /dev/loop3 ro\n"
        "196 30 7:5 / /snap/amazon-ssm-agent/4046 ro,nodev,relatime shared:39 - squashfs /dev/loop5 ro\n"
        "435 30 0:53 / /var/lib/lxcfs rw,nosuid,nodev,relatime shared:248 - fuse.lxcfs lxcfs rw,user_id=0,group_id=0,allow_other\n"
        "379 30 7:6 / /snap/snapd/13270 ro,nodev,relatime shared:194 - squashfs /dev/loop6 ro\n"
        "387 30 7:4 / /snap/snapd/13640 ro,nodev,relatime shared:198 - squashfs /dev/loop4 ro\n"
        "92 30 7:1 / /snap/core18/2246 ro,nodev,relatime shared:34 - squashfs /dev/loop1 ro\n"
        "88 29 0:51 / /run/user/1000 rw,nosuid,nodev,relatime shared:32 - tmpfs tmpfs rw,size=1609524k,mode=700,uid=1000,gid=1000\n");
    // clang-format on
    ASSERT(obj["/"]["capacity"].isNumber());
    ASSERT(obj["/"]["capacity"].number() == 23432543255);
    ASSERT(obj["/"]["available"].isNumber());
    ASSERT(obj["/"]["available"].number() == 11213234231);
    ASSERT(obj["/"]["free"].isNumber());
    ASSERT(obj["/"]["free"].number() == 12387912837);
    ASSERT(obj["/home/ubuntu"]["capacity"].isNumber());
    ASSERT(obj["/home/ubuntu"]["capacity"].number() == 999);
    ASSERT(obj["/home/ubuntu"]["available"].isNumber());
    ASSERT(obj["/home/ubuntu"]["available"].number() == 0);
    ASSERT(obj["/home/ubuntu"]["free"].isNumber());
    ASSERT(obj["/home/ubuntu"]["free"].number() == 0);
}

TEST_F(FTDCProcMountStats, TestMountStatsZeroCapacity) {
    // clang-format off
    parseNetstat("25 30 0:23 / /sys rw,nosuid,nodev,noexec,relatime shared:7 - sysfs sysfs rw\n"
        "26 30 0:5 / /proc rw,nosuid,nodev,noexec,relatime shared:13 - proc proc rw\n"
        "27 30 0:6 / /dev rw,nosuid,relatime shared:2 - devtmpfs udev rw,size=8033308k,nr_inodes=2008327,mode=755\n"
        "28 27 0:24 / /dev/pts rw,nosuid,noexec,relatime shared:3 - devpts devpts rw,gid=5,mode=620,ptmxmode=000\n"
        "29 30 0:25 / /run rw,nosuid,noexec,relatime shared:5 - tmpfs tmpfs rw,size=1609528k,mode=755\n"
        "30 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p1 rw,discard\n"
        "31 25 0:7 / /opt rw,nosuid,nodev,noexec,relatime shared:8 - ext4 /dev/nvme0n1p2 rw,discard\n"
        "88 29 0:51 / /run/user/1000 rw,nosuid,nodev,relatime shared:32 - tmpfs tmpfs rw,size=1609524k,mode=700,uid=1000,gid=1000\n");
    // clang-format on
    ASSERT(!obj.hasElement("/opt"));
}

TEST_F(FTDCProcMountStats, TestMountStatsError) {
    // clang-format off
    parseNetstat("25 30 0:23 / /sys rw,nosuid,nodev,noexec,relatime shared:7 - sysfs sysfs rw\n"
        "26 30 0:5 / /proc rw,nosuid,nodev,noexec,relatime shared:13 - proc proc rw\n"
        "27 30 0:6 / /dev rw,nosuid,relatime shared:2 - devtmpfs udev rw,size=8033308k,nr_inodes=2008327,mode=755\n"
        "28 27 0:24 / /dev/pts rw,nosuid,noexec,relatime shared:3 - devpts devpts rw,gid=5,mode=620,ptmxmode=000\n"
        "29 30 0:25 / /run rw,nosuid,noexec,relatime shared:5 - tmpfs tmpfs rw,size=1609528k,mode=755\n"
        "30 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p1 rw,discard\n"
        "31 25 0:7 / /var rw,nosuid,nodev,noexec,relatime shared:8 - ext4 /dev/nvme0n1p2 rw,discard\n"
        "88 29 0:51 / /run/user/1000 rw,nosuid,nodev,relatime shared:32 - tmpfs tmpfs rw,size=1609524k,mode=700,uid=1000,gid=1000\n");
    // clang-format on
    ASSERT(!obj.hasElement("/var"));
}

TEST_F(FTDCProcMountStats, TestMountStatsGarbageInput) {
    parseNetstat("sadjlkyfgs odyfg\x01$fgeairsufg oireasfgrysudvfbg \n\n\t\t\t34756gusf\r342");
}

TEST_F(FTDCProcMountStats, TestMountStatsSomewhatGarbageInput) {
    // clang-format off
    parseNetstat("11 11 11 11 11 11 11 11 11 11 11 11 11\n"
        "\n"
        "            \n"
        "asidhsif gsys gfuwe esuf usfg 755\n"
        "28 27 754\t\x01\n"
        "29 30 0:25 / /run sdfget3 354t89 re89y3q9t q9ty fg\n"
        "30 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p1 rw,discard\n"
        "31 25 0:7 / /boot rw,nosuid,nodev,noexec,relatime shared:8 - ext4 /dev/nvme0n1p2 rw,discard\n"
        "88 29 0:51 / /run/user/1000 rw,nosuid,nodev,relatime shared:32 - tmpfs tmpfs rw,size=1609524k,mode=700,uid=1000,gid=1000");
    // clang-format on
    ASSERT(obj.hasElement("/boot"));
    ASSERT(obj["/boot"]["capacity"].isNumber());
    ASSERT(obj["/boot"]["capacity"].number() == 888);
    ASSERT(obj["/boot"]["available"].isNumber());
    ASSERT(obj["/boot"]["available"].number() == 777);
    ASSERT(obj["/boot"]["free"].isNumber());
    ASSERT(obj["/boot"]["free"].number() == 777);
}

// Test we can parse the /proc/self/mountinfo on this machine.
// This tests is designed to exercise our parsing code on various Linuxes and never fail
TEST_F(FTDCProcMountStats, TestLocalMountStats) {
    BSONObjBuilder bb;
    ASSERT_OK(procparser::parseProcSelfMountStatsFile("/proc/self/mountinfo", &bb));
    auto obj = bb.obj();
    ASSERT(obj.hasElement("/"));
}

class FTDCProcSelfStatus : public BaseProcTest {
public:
    void setMaps(StringMap<StringData>& strMap, BSONObj& obj) {
        for (const auto& e : obj) {
            if (e.isNumber()) {
                uint64Map[e.fieldName()] = e.numberLong();
            } else {
                strMap[e.fieldName()] = e.valueStringData();
            }
        }
    }

    void parseSelfStatus(const std::vector<StringData>& keys,
                         StringMap<StringData>& strMap,
                         StringData input) {
        BSONObjBuilder builder;
        ASSERT_OK(procparser::parseProcSelfStatus(keys, input, &builder));
        obj = builder.obj();
        setMaps(strMap, obj);
    }

    BSONObj obj;
    std::vector<StringData> statusKeys{"key1", "key2", "key3", "key4", "key5", "key6"};
};


TEST_F(FTDCProcSelfStatus, TestSelfStatus) {
    StringMap<StringData> strMap;
    parseSelfStatus(statusKeys,
                    strMap,
                    "key1: a\n"
                    "Key2: b\n"
                    "key3: 12\n"
                    "key4: ab cd 1\n"
                    "key5: 12 34\n"
                    " key6: 12345");
    ASSERT_EQUALS(strMap.at("key1"), "a");
    ASSERT_FALSE(contains(strMap, "key2") || contains(uint64Map, "key2"));
    ASSERT_EQ(uint64Map.at("key3"), 12);
    ASSERT_EQUALS(strMap.at("key4"), "ab cd 1");
    ASSERT_EQUALS(strMap.at("key5"), "12 34");
    ASSERT_FALSE(contains(strMap, "key6") || contains(uint64Map, "key6"));
}

TEST_F(FTDCProcSelfStatus, NoNewline) {
    StringMap<StringData> strMap;
    parseSelfStatus(
        statusKeys, strMap, "key1: a Key2: b key3: 12 key4: ab cd 1 key5: 12 34 key6: 12345");
    ASSERT_EQUALS(strMap.at("key1"), "a Key2");
}

TEST_F(FTDCProcSelfStatus, MissingColon) {
    StringMap<StringData> strMap;
    parseSelfStatus(
        statusKeys, strMap, "key1 a\nKey2: b\nkey3: 12\nkey4: ab cd 1\nkey5: 12 34\n key6: 12345");
    ASSERT_FALSE(contains(strMap, "key1") || contains(uint64Map, "key1"));
    ASSERT_FALSE(contains(strMap, "key2") || contains(uint64Map, "key2"));
    ASSERT_EQ(uint64Map.at("key3"), 12);
    ASSERT_EQUALS(strMap.at("key4"), "ab cd 1");
    ASSERT_EQUALS(strMap.at("key5"), "12 34");
    ASSERT_FALSE(contains(strMap, "key6") || contains(uint64Map, "key6"));
}

TEST_F(FTDCProcSelfStatus, EmptyData) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(procparser::parseProcSelfStatus(statusKeys, "", &builder));
}

TEST_F(FTDCProcSelfStatus, FileNotFound) {
    BSONObjBuilder builder;
    ASSERT_EQ(ErrorCodes::FileOpenFailed,
              procparser::parseProcSelfStatusFile(
                  "/proc/self/file_that_does_not_exist", statusKeys, &builder)
                  .code());
}

// Test we can parse the /proc/self/status on this machine. Also assert we have the expected fields
// This tests is designed to exercise our parsing code on various Linuxes and fail
// Normally when run in the FTDC loop we return a non-fatal error so we may not notice the
// failure otherwise.
TEST_F(FTDCProcSelfStatus, TestLocalSelfStatus) {
    std::vector<StringData> keys{};

    BSONObjBuilder builder;

    ASSERT_OK(procparser::parseProcMemInfoFile("/proc/self/status", keys, &builder));

    BSONObj obj = builder.obj();
    StringMap<StringData> strMap;
    setMaps(strMap, obj);

    ASSERT(contains(strMap, "HugetlbPages") || contains(uint64Map, "HugetlbPages"));
    ASSERT(contains(strMap, "Threads") || contains(uint64Map, "Threads"));
    ASSERT(contains(strMap, "Cpus_allowed") || contains(uint64Map, "Cpus_allowed"));
    ASSERT(contains(strMap, "Mems_allowed") || contains(uint64Map, "Mems_allowed"));
}

class FTDCProcVMStat : public BaseProcTest {
public:
    void parseVMStat(const std::vector<StringData>& keys, StringData input) {
        BSONObjBuilder builder;
        ASSERT_OK(procparser::parseProcVMStat(keys, input, &builder));
        uint64Map = toStringMap(builder.done());
    }

    std::vector<StringData> keys{"Key1", "Key2", "Key3"};
};

TEST_F(FTDCProcVMStat, TestVMStat) {
    parseVMStat(keys, "Key1 123\nKey2 456");
    ASSERT_EQ(uint64Map.at("Key1"), 123UL);
    ASSERT_EQ(uint64Map.at("Key2"), 456UL);
}

TEST_F(FTDCProcVMStat, TestNoNewline) {
    parseVMStat(keys, "Key1 123 Key2 456");
    ASSERT_EQ(uint64Map.at("Key1"), 123UL);
    ASSERT(!contains(uint64Map, "Key2"));
}

TEST_F(FTDCProcVMStat, TestNoValue) {
    parseVMStat(keys, "Key1 123\nKey2");
    ASSERT_EQ(uint64Map.at("Key1"), 123UL);
    ASSERT(!contains(uint64Map, "Key2"));
}

TEST_F(FTDCProcVMStat, TestEmptyString) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(procparser::parseProcVMStat(keys, "", &builder));
}

// Test we can parse the /proc/vmstat on this machine. Also assert we have the expected fields
// This tests is designed to exercise our parsing code on various Linuxes and fail
// Normally when run in the FTDC loop we return a non-fatal error so we may not notice the failure
// otherwise.
TEST_F(FTDCProcVMStat, TestLocalVMStat) {
    std::vector<StringData> keys{
        "balloon_deflate"_sd,
        "balloon_inflate"_sd,
        "nr_mlock"_sd,
        "numa_pages_migrated"_sd,  // Not on RHEL 6, added with
                                   // https://github.com/torvalds/linux/commit/03c5a6e16322c
        "pgfault"_sd,
        "pgmajfault"_sd,
        "pswpin"_sd,
        "pswpout"_sd,
    };

    BSONObjBuilder builder;

    ASSERT_OK(procparser::parseProcVMStatFile("/proc/vmstat", keys, &builder));

    BSONObj obj = builder.obj();
    auto uint64Map = toStringMap(obj);
    ASSERT(contains(uint64Map, "nr_mlock"));
    ASSERT(contains(uint64Map, "pgmajfault"));
    ASSERT(contains(uint64Map, "pswpin"));
    ASSERT(contains(uint64Map, "pswpout"));
}


TEST_F(FTDCProcVMStat, TestLocalNonExistentVMStat) {
    std::vector<StringData> keys{};
    BSONObjBuilder builder;

    ASSERT_NOT_OK(procparser::parseProcVMStatFile("/proc/does_not_exist", keys, &builder));
}

class FTDCProcSysFsFileNr : public BaseProcTest {
public:
    void parseSysFsFileNr(procparser::FileNrKey key, StringData input) {
        BSONObjBuilder builder;
        ASSERT_OK(procparser::parseProcSysFsFileNr(key, input, &builder));
        uint64Map = toStringMap(builder.done());
    }
};

TEST_F(FTDCProcSysFsFileNr, TestSuccess) {
    parseSysFsFileNr(procparser::FileNrKey::kMaxFileHandles, "1 0 2\n");
    ASSERT_EQ(uint64Map.at(procparser::kMaxFileHandlesKey.toString()), 2);
    ASSERT(!contains(uint64Map, procparser::kFileHandlesInUseKey.toString()));
}

TEST_F(FTDCProcSysFsFileNr, TestSuccess2) {
    parseSysFsFileNr(procparser::FileNrKey::kFileHandlesInUse, "1 0 2\n");
    ASSERT_EQ(uint64Map.at(procparser::kFileHandlesInUseKey.toString()), 1);
    ASSERT(!contains(uint64Map, procparser::kMaxFileHandlesKey.toString()));
}

TEST_F(FTDCProcSysFsFileNr, TestOnlyParseUpToWhatWeNeed) {
    parseSysFsFileNr(procparser::FileNrKey::kFileHandlesInUse, "1 0\n");
    ASSERT_EQ(uint64Map.at(procparser::kFileHandlesInUseKey.toString()), 1);
    ASSERT(!contains(uint64Map, procparser::kMaxFileHandlesKey.toString()));
}

TEST_F(FTDCProcSysFsFileNr, TestOnlyParseUpToWhatWeNeed2) {
    parseSysFsFileNr(procparser::FileNrKey::kFileHandlesInUse, "1\n");
    ASSERT_EQ(uint64Map.at(procparser::kFileHandlesInUseKey.toString()), 1);
    ASSERT(!contains(uint64Map, procparser::kMaxFileHandlesKey.toString()));
}

TEST_F(FTDCProcSysFsFileNr, TestFailure) {
    // Failure cases
    BSONObjBuilder builder;
    ASSERT_NOT_OK(
        procparser::parseProcSysFsFileNr(procparser::FileNrKey::kFileHandlesInUse, "", &builder));
    ASSERT_NOT_OK(
        procparser::parseProcSysFsFileNr(procparser::FileNrKey::kMaxFileHandles, "", &builder));
    ASSERT_NOT_OK(procparser::parseProcSysFsFileNr(
        procparser::FileNrKey::kMaxFileHandles, "1 2\n", &builder));
}

TEST_F(FTDCProcSysFsFileNr, TestFile) {
    BSONObjBuilder builder;
    // Normal cases
    ASSERT_OK(procparser::parseProcSysFsFileNrFile(
        "/proc/sys/fs/file-nr", procparser::FileNrKey::kFileHandlesInUse, &builder));
    ASSERT_OK(procparser::parseProcSysFsFileNrFile(
        "/proc/sys/fs/file-nr", procparser::FileNrKey::kMaxFileHandles, &builder));

    // Non-existent file case
    ASSERT_NOT_OK(procparser::parseProcSysFsFileNrFile(
        "/proc/non-existent-file", procparser::FileNrKey::kFileHandlesInUse, &builder));
}

class FTDCProcPressure : public BaseProcTest {
public:
    bool isPSISupported(StringData filename) {
        int fd = open(filename.toString().c_str(), 0);
        if (fd == -1) {
            return false;
        }
        ScopeGuard scopedGuard([fd] { close(fd); });

        std::array<char, 1> buf;

        while (read(fd, buf.data(), buf.size()) == -1) {
            auto ec = lastPosixError();
            if (ec == posixError(EOPNOTSUPP)) {
                return false;
            }
            ASSERT_EQ(ec, posixError(EINTR));
        }
        return true;
    }

    void parsePressure(StringData input) {
        BSONObjBuilder builder;
        ASSERT_OK(procparser::parseProcPressure(input, &builder));
        obj = builder.obj();
        uint64Map = toStringMap(obj);
    }

    BSONObj obj;
};

TEST_F(FTDCProcPressure, TestSuccess) {
    parsePressure(
        "some avg10=0.10 avg60=6.50 avg300=1.00 total=14\nfull avg10=2.30 "
        "avg60=0.00 avg300=0.14 total=10");
    ASSERT(obj["some"]["totalMicros"].Double() == 14);

    ASSERT(obj["full"]["totalMicros"].Double() == 10);
}
TEST_F(FTDCProcPressure, TestSuccess2) {
    parsePressure("some avg10=0.10 avg60=6.50 avg300=1.00 total=14");
    ASSERT(obj["some"]["totalMicros"].Double() == 14);

    ASSERT(!obj["full"]);
}
TEST_F(FTDCProcPressure, TestSuccess3) {
    parsePressure(
        "some avg10=0.10    avg60=6.50      avg300=1.00 total=14\nfull avg10=2.30 "
        "avg60=0.00 avg300=0.14        total=10");
    ASSERT(obj["some"]["totalMicros"].Double() == 14);

    ASSERT(obj["full"]["totalMicros"].Double() == 10);
}

TEST_F(FTDCProcPressure, TestEmptyFile) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(procparser::parseProcPressureFile("", "", &builder));
}

TEST_F(FTDCProcPressure, TestNonExistentFile) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(procparser::parseProcPressureFile("cpu", "/proc/non-existent-file", &builder));
}

TEST_F(FTDCProcPressure, TestTotalNotFound) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(
        procparser::parseProcPressure("some avg10=0.10 avg60=6.50 avg300=1.00", &builder));
}

TEST_F(FTDCProcPressure, TestTotalNotInARow) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(
        procparser::parseProcPressure("some avg10=0.10 avg60=6.50 avg300=1.00\nfull avg10=2.30 "
                                      "avg60=0.00 avg300=0.14 total=10",
                                      &builder));
}

TEST_F(FTDCProcPressure, TestTotalNotValid) {
    BSONObjBuilder builder;
    ASSERT_NOT_OK(procparser::parseProcPressure(
        "some avg10=0.10 avg60=6.50 avg300=1.00 total=invalid", &builder));
}

TEST_F(FTDCProcPressure, TestLocalPressureInfo) {
    if (isPSISupported("/proc/pressure/cpu")) {
        BSONObjBuilder builder;

        ASSERT_OK(procparser::parseProcPressureFile("cpu", "/proc/pressure/cpu", &builder));

        BSONObj obj = builder.obj();
        ASSERT(obj.hasField("cpu"));
        ASSERT(obj["cpu"]["some"]);
        ASSERT(obj["cpu"]["some"]["totalMicros"]);

        // After linux kernel 5.13, /proc/pressure/cpu includes 'full' filled with 0.
        ASSERT(!obj["cpu"]["full"] || obj["cpu"]["full"]["totalMicros"].Double() == 0);
    }

    if (isPSISupported("/proc/pressure/memory")) {
        BSONObjBuilder builder;

        ASSERT_OK(procparser::parseProcPressureFile("memory", "/proc/pressure/memory", &builder));

        BSONObj obj = builder.obj();
        ASSERT(obj.hasField("memory"));
        ASSERT(obj["memory"]["some"]);
        ASSERT(obj["memory"]["some"]["totalMicros"]);

        ASSERT(obj["memory"]["full"]);
        ASSERT(obj["memory"]["full"]["totalMicros"]);
    }

    if (isPSISupported("/proc/pressure/io")) {
        BSONObjBuilder builder;

        ASSERT_OK(procparser::parseProcPressureFile("io", "/proc/pressure/io", &builder));

        BSONObj obj = builder.obj();
        ASSERT(obj.hasField("io"));
        ASSERT(obj["io"]["some"]);
        ASSERT(obj["io"]["some"]["totalMicros"]);

        ASSERT(obj["io"]["full"]);
        ASSERT(obj["io"]["full"]["totalMicros"]);
    }
}

class FTDCProcSockstat : public unittest::Test {
public:
    // Parse "keys" out of "input", which should be a string matching the format of a
    // /proc/net/sockstat file. Asserts that we can parse the keys successfully and returns a
    // BSONObj of the parsed data.
    BSONObj assertParseSockstat(std::map<StringData, std::set<StringData>> keys, StringData input) {
        BSONObjBuilder builder;
        ASSERT_OK(procparser::parseProcSockstat(keys, input, &builder));
        return builder.obj();
    }

    std::map<StringData, std::set<StringData>> testKeys{
        {"sockets", {"used"}},
        {"TCP", {"inuse", "tw", "mem"}},
    };
};

TEST_F(FTDCProcSockstat, TestSockstatSuccess) {
    auto obj = assertParseSockstat(testKeys,
                                   "sockets: used 290\n"
                                   "TCP: inuse 8 orphan 0 tw 0 alloc 12 mem 1\n"
                                   "UDP: inuse 6 mem 4\n");

    // Requested keys present with correct values.
    ASSERT_EQ(obj["sockets"]["used"].Int(), 290);
    ASSERT_EQ(obj["TCP"]["inuse"].Int(), 8);
    ASSERT_EQ(obj["TCP"]["tw"].Int(), 0);
    ASSERT_EQ(obj["TCP"]["mem"].Int(), 1);

    // Keys aren't present if they weren't requested.
    ASSERT(obj["TCP"]["alloc"].eoo());
    // Sections aren't present if they weren't requested.
    ASSERT(obj["UDP"].eoo());
}

TEST_F(FTDCProcSockstat, TestBadSocketString) {
    StringData badString = "I'm not in the right format";
    BSONObjBuilder bob;
    auto s = procparser::parseProcSockstat(testKeys, badString, &bob);
    // No desired keys found so error.
    ASSERT_EQ(s.code(), ErrorCodes::NoSuchKey);
}

TEST_F(FTDCProcSockstat, TestEmptyString) {
    StringData badString = "";
    BSONObjBuilder bob;
    auto s = procparser::parseProcSockstat(testKeys, badString, &bob);
    // No desired keys found so error.
    ASSERT_EQ(s.code(), ErrorCodes::NoSuchKey);
}

TEST_F(FTDCProcSockstat, TestStringWithNoNumber) {
    StringData badString = "sockets: used alot";
    BSONObjBuilder bob;
    auto s = procparser::parseProcSockstat(testKeys, badString, &bob);
    ASSERT_EQ(s.code(), ErrorCodes::FailedToParse);
}
TEST_F(FTDCProcSockstat, TestGoodSocketString) {
    // OK if string doesn't terminate with newline.
    auto obj = assertParseSockstat(testKeys,
                                   "sockets: used 290\n"
                                   "TCP: inuse 8 orphan 0 tw 0 alloc 12 mem 1\n"
                                   "UDP: inuse 6 mem 4");

    // Requested keys present with correct values.
    ASSERT_EQ(obj["sockets"]["used"].Int(), 290);
    ASSERT_EQ(obj["TCP"]["inuse"].Int(), 8);
    ASSERT_EQ(obj["TCP"]["tw"].Int(), 0);
    ASSERT_EQ(obj["TCP"]["mem"].Int(), 1);
}

// Test we can parse the /proc/net/sockset on this machine and assert we have some expected fields.
// Can't test values because they vary at runtime.
TEST_F(FTDCProcNetstat, TestLocalSockstat) {

    BSONObjBuilder builder;

    std::map<StringData, std::set<StringData>> testKeys{
        {"sockets", {"used"}},
        {"TCP", {"inuse", "orphan", "tw", "alloc", "mem"}},
    };

    ASSERT_OK(procparser::parseProcSockstatFile(testKeys, "/proc/net/sockstat", &builder));

    BSONObj obj = builder.obj();
    LOGV2(4840200, "Parsed local /net/proc/sockstat file", "obj"_attr = obj);

    for (auto&& [category, nodes] : testKeys) {
        ASSERT(obj[category].isABSONObj()) << ", category={}"_format(category);
        for (auto&& node : nodes) {
            ASSERT(obj[category][node].isNumber())
                << ", category={}, node={}"_format(category, node);
        }
    }
}

}  // namespace
}  // namespace mongo
