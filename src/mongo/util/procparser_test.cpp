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


#include "mongo/platform/basic.h"

#include "mongo/util/procparser.h"

#include <boost/filesystem.hpp>
#include <map>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

namespace procparser {
Status parseProcSelfMountStatsImpl(StringData data,
                                   BSONObjBuilder* builder,
                                   boost::filesystem::space_info (*getSpace)(
                                       const boost::filesystem::path&, boost::system::error_code&));
}


namespace {
using StringMap = std::map<std::string, uint64_t>;

StringMap toStringMap(BSONObj& obj) {
    StringMap map;

    for (const auto& e : obj) {
        map[e.fieldName()] = e.numberLong();
    }

    return map;
}

StringMap toNestedStringMap(BSONObj& obj) {
    StringMap map;

    for (const auto& e : obj) {
        if (e.isABSONObj()) {
            std::string prefix = std::string(e.fieldName()) + ".";

            for (const auto& child : e.Obj()) {
                map[prefix + child.fieldName()] = child.numberLong();
            }
        }
    }

    return map;
}

#define ASSERT_KEY(_key) ASSERT_TRUE(stringMap.find(_key) != stringMap.end());
#define ASSERT_NO_KEY(_key) ASSERT_TRUE(stringMap.find(_key) == stringMap.end());
#define ASSERT_KEY_AND_VALUE(_key, _value) ASSERT_EQUALS(stringMap.at(_key), _value);

#define ASSERT_PARSE_STAT(_keys, _x)                                 \
    BSONObjBuilder builder;                                          \
    ASSERT_OK(procparser::parseProcStat(_keys, _x, 1000, &builder)); \
    auto obj = builder.obj();                                        \
    auto stringMap = toStringMap(obj);
#define ASSERT_PARSE_MEMINFO(_keys, _x)                           \
    BSONObjBuilder builder;                                       \
    ASSERT_OK(procparser::parseProcMemInfo(_keys, _x, &builder)); \
    auto obj = builder.obj();                                     \
    auto stringMap = toStringMap(obj);
#define ASSERT_PARSE_NETSTAT(_keys, _x)                           \
    BSONObjBuilder builder;                                       \
    ASSERT_OK(procparser::parseProcNetstat(_keys, _x, &builder)); \
    auto obj = builder.obj();                                     \
    auto stringMap = toStringMap(obj);
#define ASSERT_PARSE_DISKSTATS(_disks, _x)                           \
    BSONObjBuilder builder;                                          \
    ASSERT_OK(procparser::parseProcDiskStats(_disks, _x, &builder)); \
    auto obj = builder.obj();                                        \
    auto stringMap = toNestedStringMap(obj);
#define ASSERT_PARSE_MOUNTSTAT(x)                                                   \
    BSONObjBuilder builder;                                                         \
    ASSERT_OK(procparser::parseProcSelfMountStatsImpl(x, &builder, &mockGetSpace)); \
    auto obj = builder.obj();
#define ASSERT_PARSE_VMSTAT(_keys, _x)                           \
    BSONObjBuilder builder;                                      \
    ASSERT_OK(procparser::parseProcVMStat(_keys, _x, &builder)); \
    auto obj = builder.obj();                                    \
    auto stringMap = toStringMap(obj);
#define ASSERT_PARSE_SYS_FS_FILENR(_key, _x)                         \
    BSONObjBuilder builder;                                          \
    ASSERT_OK(procparser::parseProcSysFsFileNr(_key, _x, &builder)); \
    auto obj = builder.obj();                                        \
    auto stringMap = toStringMap(obj);

TEST(FTDCProcStat, TestStat) {

    std::vector<StringData> keys{"cpu", "ctxt", "processes"};

    // Normal case
    {
        ASSERT_PARSE_STAT(
            keys,
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
        ASSERT_KEY_AND_VALUE("user_ms", 41801UL);
        ASSERT_KEY_AND_VALUE("nice_ms", 9179UL);
        ASSERT_KEY_AND_VALUE("system_ms", 32206UL);
        ASSERT_KEY_AND_VALUE("idle_ms", 831134223UL);
        ASSERT_KEY_AND_VALUE("iowait_ms", 34279UL);
        ASSERT_KEY_AND_VALUE("irq_ms", 0UL);
        ASSERT_KEY_AND_VALUE("softirq_ms", 947UL);
        ASSERT_KEY_AND_VALUE("steal_ms", 0UL);
        ASSERT_KEY_AND_VALUE("guest_ms", 0UL);
        ASSERT_KEY_AND_VALUE("guest_nice_ms", 0UL);
        ASSERT_KEY_AND_VALUE("ctxt", 190305514UL);
        ASSERT_KEY_AND_VALUE("processes", 47438UL);
    }

    // Missing fields in cpu and others
    {
        ASSERT_PARSE_STAT(keys,
                          "cpu  41801 9179 32206\n"
                          "ctxt 190305514\n");
        ASSERT_KEY_AND_VALUE("user_ms", 41801UL);
        ASSERT_KEY_AND_VALUE("nice_ms", 9179UL);
        ASSERT_KEY_AND_VALUE("system_ms", 32206UL);
        ASSERT_NO_KEY("idle_ms");
        ASSERT_KEY_AND_VALUE("ctxt", 190305514UL);
        ASSERT_NO_KEY("processes");
    }

    // Missing fields in cpu and others
    {
        ASSERT_PARSE_STAT(keys,
                          "cpu  41801\n"
                          "ctxt 190305514\n");
        ASSERT_KEY_AND_VALUE("user_ms", 41801UL);
        ASSERT_NO_KEY("nice_ms");
        ASSERT_KEY_AND_VALUE("ctxt", 190305514UL);
        ASSERT_NO_KEY("processes");
    }

    // Missing fields in cpu
    {
        ASSERT_PARSE_STAT(keys,
                          "cpu  \n"
                          "ctxt 190305514\n");
        ASSERT_KEY_AND_VALUE("ctxt", 190305514UL);
        ASSERT_NO_KEY("processes");
    }

    // Single string with only cpu and numbers
    {
        ASSERT_PARSE_STAT(keys, "cpu 41801 9179 32206");
        ASSERT_KEY_AND_VALUE("user_ms", 41801UL);
        ASSERT_KEY_AND_VALUE("nice_ms", 9179UL);
        ASSERT_KEY_AND_VALUE("system_ms", 32206UL);
        ASSERT_NO_KEY("idle_ms");
    }

    // Single string with only cpu
    {
        BSONObjBuilder builder;
        ASSERT_NOT_OK(procparser::parseProcStat(keys, "cpu", 1000, &builder));
    }

    // Single string with only cpu and a number, and empty ctxt
    {
        ASSERT_PARSE_STAT(keys,
                          "cpu  123\n"
                          "ctxt");
        ASSERT_KEY_AND_VALUE("user_ms", 123UL);
    }

    // Empty String
    {
        BSONObjBuilder builder;
        ASSERT_NOT_OK(procparser::parseProcStat(keys, "", 1000, &builder));
    }
}

// Test we can parse the /proc/stat on this machine. Also assert we have the expected fields
// This tests is designed to exercise our parsing code on various Linuxes and fail
// Normally when run in the FTDC loop we return a non-fatal error so we may not notice the failure
// otherwise.
TEST(FTDCProcStat, TestLocalStat) {
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
    auto stringMap = toStringMap(obj);
    LOGV2(23364, "OBJ:{obj}", "obj"_attr = obj);
    ASSERT_KEY("user_ms");
    ASSERT_KEY("nice_ms");
    ASSERT_KEY("idle_ms");
    ASSERT_KEY("system_ms");
    ASSERT_KEY("iowait_ms");
    ASSERT_KEY("irq_ms");
    ASSERT_KEY("softirq_ms");
    ASSERT_KEY("steal_ms");
    // Needs 2.6.24 - ASSERT_KEY("guest_ms");
    // Needs 2.6.33 - ASSERT_KEY("guest_nice_ms");
    ASSERT_KEY("ctxt");
    ASSERT_KEY("btime");
    ASSERT_KEY("processes");
    ASSERT_KEY("procs_running");
    ASSERT_KEY("procs_blocked");
}

TEST(FTDCProcStat, TestLocalNonExistentStat) {
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

TEST(FTDCProcMemInfo, TestMemInfo) {

    std::vector<StringData> keys{"Key1", "Key2", "Key3"};

    // Normal case
    {
        ASSERT_PARSE_MEMINFO(keys, "Key1: 123 kB\nKey2: 456 kB");
        ASSERT_KEY_AND_VALUE("Key1_kb", 123UL);
        ASSERT_KEY_AND_VALUE("Key2_kb", 456UL);
    }

    // Space in key name
    {
        ASSERT_PARSE_MEMINFO(keys, "Key1: 123 kB\nKey 2: 456 kB");
        ASSERT_KEY_AND_VALUE("Key1_kb", 123UL);
        ASSERT_NO_KEY("Key2_kb");
    }

    // No newline
    {
        ASSERT_PARSE_MEMINFO(keys, "Key1: 123 kB Key2: 456 kB");
        ASSERT_KEY_AND_VALUE("Key1_kb", 123UL);
        ASSERT_NO_KEY("Key2_kb");
    }

    // Missing colon on first key
    {
        ASSERT_PARSE_MEMINFO(keys, "Key1 123 kB\nKey2: 456 kB");
        ASSERT_KEY_AND_VALUE("Key1_kb", 123UL);
        ASSERT_KEY_AND_VALUE("Key2_kb", 456UL);
    }

    // One token missing kB, HugePages is not size in kB
    {
        ASSERT_PARSE_MEMINFO(keys, "Key1: 123 kB\nKey2: 456\nKey3: 789 kB\nKey4: 789 kB");
        ASSERT_KEY_AND_VALUE("Key1_kb", 123UL);
        ASSERT_KEY_AND_VALUE("Key2", 456UL);
        ASSERT_KEY_AND_VALUE("Key3_kb", 789UL);
        ASSERT_NO_KEY("Key4_kb");
    }

    // Empty string
    {
        BSONObjBuilder builder;
        ASSERT_NOT_OK(procparser::parseProcMemInfo(keys, "", &builder));
    }
}

// Test we can parse the /proc/meminfo on this machine. Also assert we have the expected fields
// This tests is designed to exercise our parsing code on various Linuxes and fail
// Normally when run in the FTDC loop we return a non-fatal error so we may not notice the failure
// otherwise.
TEST(FTDCProcMemInfo, TestLocalMemInfo) {
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
    auto stringMap = toStringMap(obj);
    LOGV2(23365, "OBJ:{obj}", "obj"_attr = obj);
    ASSERT_KEY("MemTotal_kb");
    ASSERT_KEY("MemFree_kb");
    // Needs in 3.15+ - ASSERT_KEY("MemAvailable_kb");
    ASSERT_KEY("Buffers_kb");
    ASSERT_KEY("Cached_kb");
    ASSERT_KEY("SwapCached_kb");
    ASSERT_KEY("Active_kb");
    ASSERT_KEY("Inactive_kb");
    // Needs 2.6.28+ - ASSERT_KEY("Active(anon)_kb");
    // Needs 2.6.28+ - ASSERT_KEY("Inactive(anon)_kb");
    // Needs 2.6.28+ - ASSERT_KEY("Active(file)_kb");
    // Needs 2.6.28+ - ASSERT_KEY("Inactive(file)_kb");
    // Needs 2.6.28+ - ASSERT_KEY("Unevictable_kb");
    // Needs 2.6.28+ - ASSERT_KEY("Mlocked_kb");
    ASSERT_KEY("SwapTotal_kb");
    ASSERT_KEY("SwapFree_kb");
    ASSERT_KEY("Dirty_kb");
    ASSERT_KEY("Writeback_kb");
    ASSERT_KEY("AnonPages_kb");
    ASSERT_KEY("Mapped_kb");
    // Needs 2.6.32+ - ASSERT_KEY("Shmem_kb");
    ASSERT_KEY("Slab_kb");
    // Needs 2.6.19+ - ASSERT_KEY("SReclaimable_kb");
    // Needs 2.6.19+ - ASSERT_KEY("SUnreclaim_kb");
    // Needs 2.6.32+ - ASSERT_KEY("KernelStack_kb");
    ASSERT_KEY("PageTables_kb");
    ASSERT_KEY("NFS_Unstable_kb");
    ASSERT_KEY("Bounce_kb");
    // Needs 2.6.19+ - ASSERT_KEY("WritebackTmp_kb");
    ASSERT_KEY("CommitLimit_kb");
    ASSERT_KEY("Committed_AS_kb");
    ASSERT_KEY("VmallocTotal_kb");
    ASSERT_KEY("VmallocUsed_kb");
    ASSERT_KEY("VmallocChunk_kb");
    // Needs CONFIG_MEMORY_FAILURE & 2.6.32+ ASSERT_KEY("HardwareCorrupted_kb");
    // Needs CONFIG_TRANSPARENT_HUGEPAGE - ASSERT_KEY("AnonHugePages_kb");
    // Needs CONFIG_CMA & 3.19+ - ASSERT_KEY("CmaTotal_kb");
    // Needs CONFIG_CMA & 3.19+ - ASSERT_KEY("CmaFree_kb");
}


TEST(FTDCProcMemInfo, TestLocalNonExistentMemInfo) {
    std::vector<StringData> keys{};
    BSONObjBuilder builder;

    ASSERT_NOT_OK(procparser::parseProcMemInfoFile("/proc/does_not_exist", keys, &builder));
}

TEST(FTDCProcNetstat, TestNetstat) {

    // test keys
    std::vector<StringData> keys{"pfx1", "pfx2", "pfx3"};

    // Normal case
    {
        ASSERT_PARSE_NETSTAT(keys,
                             "pfx1 key1 key2 key3\n"
                             "pfx1 1 2 3\n"
                             "pfxX key1 key2\n"
                             "pfxX key1 key2\n"
                             "pfx2 key4 key5\n"
                             "pfx2 4 5\n");
        ASSERT_KEY_AND_VALUE("pfx1key1", 1UL);
        ASSERT_KEY_AND_VALUE("pfx1key2", 2UL);
        ASSERT_NO_KEY("pfxXkey1");
        ASSERT_NO_KEY("pfxXkey2");
        ASSERT_KEY_AND_VALUE("pfx1key3", 3UL)
        ASSERT_KEY_AND_VALUE("pfx2key4", 4UL);
        ASSERT_KEY_AND_VALUE("pfx2key5", 5UL);
    }

    // Mismatched keys and values
    {
        ASSERT_PARSE_NETSTAT(keys,
                             "pfx1 key1 key2 key3\n"
                             "pfx1 1 2 3 4\n"
                             "pfx2 key4 key5\n"
                             "pfx2 4\n"
                             "pfx3 key6 key7\n");
        ASSERT_KEY_AND_VALUE("pfx1key1", 1UL);
        ASSERT_KEY_AND_VALUE("pfx1key2", 2UL);
        ASSERT_KEY_AND_VALUE("pfx1key3", 3UL);
        ASSERT_NO_KEY("pfx1key4");
        ASSERT_KEY_AND_VALUE("pfx2key4", 4UL);
        ASSERT_NO_KEY("pfx2key5");
        ASSERT_NO_KEY("pfx3key6");
        ASSERT_NO_KEY("pfx3key7");
    }

    // Non-numeric value
    {
        ASSERT_PARSE_NETSTAT(keys,
                             "pfx1 key1 key2 key3\n"
                             "pfx1 1 foo 3\n");
        ASSERT_KEY_AND_VALUE("pfx1key1", 1UL);
        ASSERT_NO_KEY("pfx1key2");
        ASSERT_KEY_AND_VALUE("pfx1key3", 3UL)
    }

    // No newline
    {
        ASSERT_PARSE_NETSTAT(keys,
                             "pfx1 key1 key2 key3\n"
                             "pfx1 1 2 3\n"
                             "pfx2 key4 key5\n"
                             "pfx2 4 5");
        ASSERT_KEY_AND_VALUE("pfx1key1", 1UL);
        ASSERT_KEY_AND_VALUE("pfx1key2", 2UL);
        ASSERT_KEY_AND_VALUE("pfx1key3", 3UL)
        ASSERT_KEY_AND_VALUE("pfx2key4", 4UL);
        ASSERT_KEY_AND_VALUE("pfx2key5", 5UL);
    }

    // Single line only
    {
        BSONObjBuilder builder;
        ASSERT_NOT_OK(procparser::parseProcNetstat(keys, "pfx1 key1 key2 key3\n", &builder));
    }

    // Empty string
    {
        BSONObjBuilder builder;
        ASSERT_NOT_OK(procparser::parseProcNetstat(keys, "", &builder));
    }
}

// Test we can parse the /proc/net/netstat on this machine and assert we have some expected fields
// Some keys can vary between distros, so we test only for the existence of a few basic ones
TEST(FTDCProcNetstat, TestLocalNetstat) {

    BSONObjBuilder builder;

    std::vector<StringData> keys{"TcpExt:"_sd, "IpExt:"_sd};

    ASSERT_OK(procparser::parseProcNetstatFile(keys, "/proc/net/netstat", &builder));

    BSONObj obj = builder.obj();
    auto stringMap = toStringMap(obj);
    LOGV2(23366, "OBJ:{obj}", "obj"_attr = obj);
    ASSERT_KEY("TcpExt:TCPTimeouts");
    ASSERT_KEY("TcpExt:TCPPureAcks");
    ASSERT_KEY("TcpExt:TCPAbortOnTimeout");
    ASSERT_KEY("TcpExt:EmbryonicRsts");
    ASSERT_KEY("TcpExt:ListenDrops");
    ASSERT_KEY("TcpExt:ListenOverflows");
    ASSERT_KEY("TcpExt:DelayedACKs");
    ASSERT_KEY("IpExt:OutOctets");
    ASSERT_KEY("IpExt:InOctets");
}

// Test we can parse the /proc/net/snmp on this machine and assert we have some expected fields
// Some keys can vary between distros, so we test only for the existence of a few basic ones
TEST(FTDCProcNetstat, TestLocalNetSnmp) {

    BSONObjBuilder builder;

    std::vector<StringData> keys{"Tcp:"_sd, "Ip:"_sd};

    ASSERT_OK(procparser::parseProcNetstatFile(keys, "/proc/net/snmp", &builder));

    BSONObj obj = builder.obj();
    auto stringMap = toStringMap(obj);
    LOGV2(23367, "OBJ:{obj}", "obj"_attr = obj);
    ASSERT_KEY("Ip:InReceives");
    ASSERT_KEY("Ip:OutRequests");
    ASSERT_KEY("Tcp:InSegs");
    ASSERT_KEY("Tcp:OutSegs");
}

TEST(FTDCProcNetstat, TestLocalNonExistentNetstat) {
    std::vector<StringData> keys{};
    BSONObjBuilder builder;

    ASSERT_NOT_OK(procparser::parseProcNetstatFile(keys, "/proc/does_not_exist", &builder));
}

TEST(FTDCProcDiskStats, TestDiskStats) {

    std::vector<StringData> disks{"dm-1", "sda", "sdb"};

    // Normal case including high device major numbers.
    {
        ASSERT_PARSE_DISKSTATS(
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
        ASSERT_KEY_AND_VALUE("sda.reads", 120611UL);
        ASSERT_KEY_AND_VALUE("sda.writes", 349797UL);
        ASSERT_KEY_AND_VALUE("sda.io_queued_ms", 2554160UL);
        ASSERT_KEY_AND_VALUE("sdb.reads", 12707UL);
        ASSERT_KEY_AND_VALUE("sdb.writes", 997UL);
        ASSERT_KEY_AND_VALUE("sdb.io_queued_ms", 155619UL);
        ASSERT_KEY_AND_VALUE("dm-1.reads", 109UL);
        ASSERT_KEY_AND_VALUE("dm-1.writes", 0UL);
        ASSERT_KEY_AND_VALUE("dm-1.io_queued_ms", 226UL);
    }

    // Exclude a block device without any activity
    {
        ASSERT_PARSE_DISKSTATS(
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
        ASSERT_KEY_AND_VALUE("sda.reads", 120611UL);
        ASSERT_KEY_AND_VALUE("sda.writes", 349797UL);
        ASSERT_KEY_AND_VALUE("sda.io_queued_ms", 2554160UL);
        ASSERT_NO_KEY("sdb.reads");
        ASSERT_NO_KEY("sdb.writes");
        ASSERT_NO_KEY("sdb.io_queued_ms");
        ASSERT_KEY_AND_VALUE("dm-1.reads", 109UL);
        ASSERT_KEY_AND_VALUE("dm-1.writes", 0UL);
        ASSERT_KEY_AND_VALUE("dm-1.io_queued_ms", 226UL);
    }


    // Strings with less numbers
    { ASSERT_PARSE_DISKSTATS(disks, "8       0 sda 120611 33630 6297628 96550 349797 "); }

    // Strings with no numbers
    { ASSERT_PARSE_DISKSTATS(disks, "8       0 sda"); }

    // Strings that are too short
    {
        BSONObjBuilder builder;
        ASSERT_NOT_OK(procparser::parseProcDiskStats(disks, "8       0", &builder));
        ASSERT_NOT_OK(procparser::parseProcDiskStats(disks, "8", &builder));
        ASSERT_NOT_OK(procparser::parseProcDiskStats(disks, "", &builder));
    }
}

TEST(FTDCProcDiskStats, TestLocalNonExistentStat) {
    std::vector<StringData> disks{"dm-1", "sda", "sdb"};
    BSONObjBuilder builder;

    ASSERT_NOT_OK(procparser::parseProcDiskStatsFile("/proc/does_not_exist", disks, &builder));
}

TEST(FTDCProcDiskStats, TestFindBadPhysicalDiskPaths) {
    // Validate nothing goes wrong when we check a non-existent path.
    {
        auto disks = procparser::findPhysicalDisks("/proc/does_not_exist");
        ASSERT_EQUALS(0UL, disks.size());
    }

    // Validate nothing goes wrong when we check a path we do not have permission.
    {
        auto disks = procparser::findPhysicalDisks("/sys/kernel/debug");
        ASSERT_EQUALS(0UL, disks.size());
    }
}

// Test we can parse the /proc/diskstats on this machine. Also assert we have the expected fields
// This tests is designed to exercise our parsing code on various Linuxes and fail
// Normally when run in the FTDC loop we return a non-fatal error so we may not notice the failure
// otherwise.
TEST(FTDCProcDiskStats, TestLocalDiskStats) {
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
    auto stringMap = toNestedStringMap(obj);
    LOGV2(23369, "OBJ:{obj}", "obj"_attr = obj);

    bool foundDisk = false;

    for (const auto& disk : disks) {
        std::string prefix(disk);
        prefix += ".";

        auto reads = prefix + "reads";
        auto io_queued_ms = prefix + "io_queued_ms";

        // Make sure that if have the first field, then we have the last field.
        if (stringMap.find(reads) != stringMap.end()) {
            foundDisk = true;
            if (stringMap.find(io_queued_ms) == stringMap.end()) {
                FAIL(std::string("Inconsistency for ") + disk);
            }
        }
    }

    if (!foundDisk) {
        FAIL("Did not find any interesting disks on this machine.");
    }
}

boost::filesystem::space_info mockGetSpace(const boost::filesystem::path& p,
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

TEST(FTDCProcMountStats, TestMountStatsHappyPath) {
    // clang-format off
    ASSERT_PARSE_MOUNTSTAT("25 30 0:23 / /sys rw,nosuid,nodev,noexec,relatime shared:7 - sysfs sysfs rw\n"
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

TEST(FTDCProcMountStats, TestMountStatsZeroCapacity) {
    // clang-format off
    ASSERT_PARSE_MOUNTSTAT("25 30 0:23 / /sys rw,nosuid,nodev,noexec,relatime shared:7 - sysfs sysfs rw\n"
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

TEST(FTDCProcMountStats, TestMountStatsError) {
    // clang-format off
    ASSERT_PARSE_MOUNTSTAT("25 30 0:23 / /sys rw,nosuid,nodev,noexec,relatime shared:7 - sysfs sysfs rw\n"
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

TEST(FTDCProcMountStats, TestMountStatsGarbageInput) {
    ASSERT_PARSE_MOUNTSTAT(
        "sadjlkyfgs odyfg\x01$fgeairsufg oireasfgrysudvfbg \n\n\t\t\t34756gusf\r342");
}

TEST(FTDCProcMountStats, TestMountStatsSomewhatGarbageInput) {
    // clang-format off
    ASSERT_PARSE_MOUNTSTAT("11 11 11 11 11 11 11 11 11 11 11 11 11\n"
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
TEST(FTDCProcMountStats, TestLocalMountStats) {
    BSONObjBuilder bb;
    ASSERT_OK(procparser::parseProcSelfMountStatsFile("/proc/self/mountinfo", &bb));
    auto obj = bb.obj();
    ASSERT(obj.hasElement("/"));
}

TEST(FTDCProcVMStat, TestVMStat) {

    std::vector<StringData> keys{"Key1", "Key2", "Key3"};

    // Normal case
    {
        ASSERT_PARSE_VMSTAT(keys, "Key1 123\nKey2 456");
        ASSERT_KEY_AND_VALUE("Key1", 123UL);
        ASSERT_KEY_AND_VALUE("Key2", 456UL);
    }

    // No newline
    {
        ASSERT_PARSE_VMSTAT(keys, "Key1 123 Key2 456");
        ASSERT_KEY_AND_VALUE("Key1", 123UL);
        ASSERT_NO_KEY("Key2");
    }

    // Key without value
    {
        ASSERT_PARSE_VMSTAT(keys, "Key1 123\nKey2");
        ASSERT_KEY_AND_VALUE("Key1", 123UL);
        ASSERT_NO_KEY("Key2");
    }

    // Empty string
    {
        BSONObjBuilder builder;
        ASSERT_NOT_OK(procparser::parseProcVMStat(keys, "", &builder));
    }
}

// Test we can parse the /proc/vmstat on this machine. Also assert we have the expected fields
// This tests is designed to exercise our parsing code on various Linuxes and fail
// Normally when run in the FTDC loop we return a non-fatal error so we may not notice the failure
// otherwise.
TEST(FTDCProcVMStat, TestLocalVMStat) {
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
    auto stringMap = toStringMap(obj);
    ASSERT_KEY("nr_mlock");
    ASSERT_KEY("pgmajfault");
    ASSERT_KEY("pswpin");
    ASSERT_KEY("pswpout");
}


TEST(FTDCProcVMStat, TestLocalNonExistentVMStat) {
    std::vector<StringData> keys{};
    BSONObjBuilder builder;

    ASSERT_NOT_OK(procparser::parseProcVMStatFile("/proc/does_not_exist", keys, &builder));
}

TEST(FTDCProcSysFsFileNr, TestSuccess) {
    // Normal cases
    {
        ASSERT_PARSE_SYS_FS_FILENR(procparser::FileNrKey::kMaxFileHandles, "1 0 2\n");
        ASSERT_KEY_AND_VALUE(procparser::kMaxFileHandlesKey.toString(), 2);
        ASSERT_NO_KEY(procparser::kFileHandlesInUseKey.toString());
    }
    {
        ASSERT_PARSE_SYS_FS_FILENR(procparser::FileNrKey::kFileHandlesInUse, "1 0 2\n");
        ASSERT_KEY_AND_VALUE(procparser::kFileHandlesInUseKey.toString(), 1);
        ASSERT_NO_KEY(procparser::kMaxFileHandlesKey.toString());
    }
    // Test that we only parse up to where we need
    {
        ASSERT_PARSE_SYS_FS_FILENR(procparser::FileNrKey::kFileHandlesInUse, "1 0\n");
        ASSERT_KEY_AND_VALUE(procparser::kFileHandlesInUseKey.toString(), 1);
        ASSERT_NO_KEY(procparser::kMaxFileHandlesKey.toString());
    }
    {
        ASSERT_PARSE_SYS_FS_FILENR(procparser::FileNrKey::kFileHandlesInUse, "1\n");
        ASSERT_KEY_AND_VALUE(procparser::kFileHandlesInUseKey.toString(), 1);
        ASSERT_NO_KEY(procparser::kMaxFileHandlesKey.toString());
    }
}

TEST(FTDCProcSysFsFileNr, TestFailure) {
    // Failure cases
    BSONObjBuilder builder;
    ASSERT_NOT_OK(
        procparser::parseProcSysFsFileNr(procparser::FileNrKey::kFileHandlesInUse, "", &builder));
    ASSERT_NOT_OK(
        procparser::parseProcSysFsFileNr(procparser::FileNrKey::kMaxFileHandles, "", &builder));
    ASSERT_NOT_OK(procparser::parseProcSysFsFileNr(
        procparser::FileNrKey::kMaxFileHandles, "1 2\n", &builder));
}

TEST(FTDCProcSysFsFileNr, TestFile) {
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

}  // namespace
}  // namespace mongo
