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

#include "mongo/util/procparser.h"

#include <boost/filesystem.hpp>
#include <map>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {

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
#define ASSERT_PARSE_DISKSTATS(_disks, _x)                           \
    BSONObjBuilder builder;                                          \
    ASSERT_OK(procparser::parseProcDiskStats(_disks, _x, &builder)); \
    auto obj = builder.obj();                                        \
    auto stringMap = toNestedStringMap(obj);

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
        "btime", "cpu", "ctxt", "processes", "procs_blocked", "procs_running",
    };

    BSONObjBuilder builder;

    ASSERT_OK(procparser::parseProcStatFile("/proc/stat", keys, &builder));

    BSONObj obj = builder.obj();
    auto stringMap = toStringMap(obj);
    log() << "OBJ:" << obj;
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
        "btime", "cpu", "ctxt", "processes", "procs_blocked", "procs_running",
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
    log() << "OBJ:" << obj;
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
        log() << "DISK:" << disk;
        disks2.emplace_back(disk);
    }

    ASSERT_NOT_EQUALS(0UL, disks.size());

    BSONObjBuilder builder;

    ASSERT_OK(procparser::parseProcDiskStatsFile("/proc/diskstats", disks2, &builder));

    BSONObj obj = builder.obj();
    auto stringMap = toNestedStringMap(obj);
    log() << "OBJ:" << obj;

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

}  // namespace
}  // namespace mongo
