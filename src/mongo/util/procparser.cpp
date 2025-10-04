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


#include "mongo/util/procparser.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <istream>
#include <map>
#include <set>
#include <string>
#include <system_error>

#include <fcntl.h>

#include <boost/algorithm/string/constants.hpp>
#include <boost/algorithm/string/finder.hpp>
// IWYU pragma: no_include "boost/algorithm/string/detail/finder.hpp"
#include <boost/algorithm/string/find_iterator.hpp>
#include <boost/core/addressof.hpp>
#include <boost/function/function_base.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/range/iterator_range_core.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"
#include <boost/type_index/type_index_facade.hpp>

#ifndef _WIN32
#include <type_traits>
#endif

#include "mongo/base/error_codes.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/pcre.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

namespace mongo {

namespace {

template <typename It>
StringData stringDataFromRange(It first, It last) {
    if (auto d = std::distance(first, last))
        return StringData(&*first, static_cast<size_t>(d));
    return {};
}

template <typename Range>
StringData stringDataFromRange(const Range& r) {
    return stringDataFromRange(r.begin(), r.end());
}

/**
 * Get USER_HZ for the machine. See time(7) for an explanation.
 */
int64_t getTicksPerSecond() {
    int64_t ret = sysconf(_SC_CLK_TCK);
    return ret;
}

/**
 * Convert USER_HZ to milliseconds.
 */
double convertTicksToMilliSeconds(const int64_t ticks, const int64_t ticksPerSecond) {
    return static_cast<double>(ticks) / (static_cast<double>(ticksPerSecond) / 1000.0);
}

const size_t kFileBufferSize = 16384;
const size_t kFileReadRetryCount = 5;

constexpr auto kSysBlockDeviceDirectoryName = "device";

/**
 * Read a file from disk as a string with a null-terminating byte using the POSIX file api.
 *
 * This function is designed to get all the data it needs from small /proc files in a single read.
 * The /proc/stat and /proc/diskstats files can vary in size, but 16kb will cover most cases.
 *
 * Finally, we return errors instead of throwing to ensure that FTDC can return partial information
 * on failure instead of no information. Some container filesystems may overlay /proc so we may not
 * be reading directly from the kernel.
 */
StatusWith<std::string> readFileAsString(StringData filename) {
    int fd = open(std::string{filename}.c_str(), 0);
    if (fd == -1) {
        auto ec = lastSystemError();
        return Status(ErrorCodes::FileOpenFailed,
                      str::stream() << "Failed to open file " << filename
                                    << " with error: " << errorMessage(ec));
    }
    ScopeGuard scopedGuard([fd] { close(fd); });

    BufBuilder builder(kFileBufferSize);
    std::array<char, kFileBufferSize> buf;

    ssize_t size_read = 0;

    // Read until the end as needed
    do {

        // Retry if interrupted
        size_t retry = 0;

        do {
            size_read = read(fd, buf.data(), kFileBufferSize);

            if (size_read == -1) {
                auto ec = lastPosixError();

                // Retry if we hit EAGAIN or EINTR a few times before giving up
                if (retry < kFileReadRetryCount &&
                    (ec == posixError(EAGAIN) || ec == posixError(EINTR))) {
                    ++retry;
                    continue;
                }

                return Status(ErrorCodes::FileStreamFailed,
                              str::stream() << "Failed to read file " << filename
                                            << " with error: " << errorMessage(ec));
            }

            break;
        } while (true);

        if (size_read != 0) {
            builder.appendBuf(buf.data(), size_read);
        }
    } while (size_read != 0);

    // Null terminate the buffer since we are about to convert it to a string
    builder.appendChar(0);

    return std::string(builder.buf(), builder.len());
}


const char* const kAdditionCpuFields[] = {"user_ms",
                                          "nice_ms",
                                          "system_ms",
                                          "idle_ms",
                                          "iowait_ms",
                                          "irq_ms",
                                          "softirq_ms",
                                          "steal_ms",
                                          "guest_ms",
                                          "guest_nice_ms"};
const size_t kAdditionCpuFieldCount = std::extent<decltype(kAdditionCpuFields)>::value;

const char* const kDiskFields[] = {
    "reads",
    "reads_merged",
    "read_sectors",
    "read_time_ms",
    "writes",
    "writes_merged",
    "write_sectors",
    "write_time_ms",
    "io_in_progress",
    "io_time_ms",
    "io_queued_ms",
};

const size_t kDiskFieldCount = std::extent<decltype(kDiskFields)>::value;

}  // namespace

namespace procparser {

using StringSplitIterator = boost::split_iterator<StringData::const_iterator>;

/**
 * This function is a generic function that passes in logic to parse a proc file. It looks for
 * keys within the data param and captures the corresponding values. If no keys were found
 * within the data this function returns a NoSuchKey error.
 *
 * @param keys: The elements we are looking for in the data.
 * @param input: The StringData we are parsing.
 * @param getKey: Uses a StringSplitIterator to split the line by a user specified
 * delimiter and returns the first value as the key.
 * @param getValueAndProcess: Specifies how to get the value after the key and
 * decides what to do with the key-value pair.
 */
Status parseGenericStats(const std::vector<StringData>& keys,
                         StringData input,
                         std::function<StringData(StringData, StringSplitIterator&)> getKey,
                         std::function<void(StringData, StringSplitIterator&)> getValueAndProcess) {
    bool foundKeys = false;

    for (auto lineIt = StringSplitIterator(
             input.begin(),
             input.end(),
             boost::token_finder([](char c) { return c == '\n'; }, boost::token_compress_on));
         lineIt != StringSplitIterator();
         ++lineIt) {
        StringData line = stringDataFromRange(*lineIt);
        StringSplitIterator splitLineIterator;
        auto key = getKey(line, splitLineIterator);

        if (splitLineIterator == StringSplitIterator() ||
            ++splitLineIterator == StringSplitIterator()) {
            continue;
        }

        if (keys.empty() || std::find(keys.begin(), keys.end(), key) != keys.end()) {
            foundKeys = true;
            getValueAndProcess(key, splitLineIterator);
        }
    }
    return foundKeys ? Status::OK()
                     : Status(ErrorCodes::NoSuchKey, "Failed to find any keys in string");
}


// Here is an example of the type of string it supports.
// Note: intr output has been trimmed
//
// The cpu field maps up to 10 individual fields depending on the kernel version. For other views,
// this code assumes there is only a single value.
//
// For more information, see:
// Documentation/filesystems/proc.txt in the Linux kernel
// proc(5) man page
//
// > cat /proc/stat
// cpu  41801 9179 32206 831134223 34279 0 947 0 0 0
// cpu0 2977 450 2475 69253074 1959 0 116 0 0 0
// cpu1 6213 4261 9400 69177349 845 0 539 0 0 0
// cpu2 1949 831 3699 69261035 645 0 0 0 0 0
// cpu3 2222 644 3283 69264801 783 0 0 0 0 0
// cpu4 16576 607 4757 69232589 8195 0 291 0 0 0
// cpu5 3742 391 4571 69257332 2322 0 0 0 0 0
// cpu6 2173 376 743 69284308 400 0 0 0 0 0
// cpu7 1232 375 704 69285753 218 0 0 0 0 0
// cpu8 960 127 576 69262851 18107 0 0 0 0 0
// cpu9 1755 227 744 69283938 362 0 0 0 0 0
// cpu10 1380 641 678 69285193 219 0 0 0 0 0
// cpu11 618 244 572 69285995 218 0 0 0 0 0
// intr 54084718 135 2 ....
// ctxt 190305514
// btime 1463584038
// processes 47438
// procs_running 1
// procs_blocked 0
// softirq 102690251 8 26697410 115481 23345078 816026 0 2296 26068778 0 25645174
//
Status parseProcStat(const std::vector<StringData>& keys,
                     StringData data,
                     int64_t ticksPerSecond,
                     BSONObjBuilder* builder) {
    return parseGenericStats(
        keys,
        data,
        [](StringData line, StringSplitIterator& lineIt) {
            lineIt = StringSplitIterator(
                line.begin(),
                line.end(),
                boost::token_finder([](char c) { return c == ' '; }, boost::token_compress_on));
            return stringDataFromRange(*lineIt);
        },
        [&](StringData key, StringSplitIterator& valueIt) {
            StringData value = stringDataFromRange(valueIt->begin(), valueIt->end());

            // Cpu is 10 fields, we need to chew through all of them.
            // Some kernels we support lack the last field or two: guest and/or guest_nice.
            if (key == "cpu") {
                for (size_t index = 0;
                     valueIt != StringSplitIterator() && index < kAdditionCpuFieldCount;
                     ++valueIt, ++index) {
                    uint64_t numVal;
                    if (!NumberParser{}(stringDataFromRange(*valueIt), &numVal).isOK()) {
                        numVal = 0;
                    }
                    builder->appendNumber(kAdditionCpuFields[index],
                                          convertTicksToMilliSeconds(numVal, ticksPerSecond));
                }
            } else {
                uint64_t numVal;
                if (!NumberParser{}(value, &numVal).isOK()) {
                    numVal = 0;
                }

                builder->appendNumber(key, static_cast<long long>(numVal));
            }
        });
}

Status parseProcStatFile(StringData filename,
                         const std::vector<StringData>& keys,
                         BSONObjBuilder* builder) {
    auto swString = readFileAsString(filename);
    if (!swString.isOK()) {
        return swString.getStatus();
    }

    auto status = parseProcStat(keys, swString.getValue(), getTicksPerSecond(), builder);
    if (!status.isOK()) {
        status.addContext(fmt::format("Parsing {}", filename));
    }
    return status;
}

// Here is an example of the type of string it supports:
// Note: output has been trimmed
//
// For more information, see:
// Documentation/filesystems/proc.txt in the Linux kernel
// proc(5) man page
//
// > cat /proc/meminfo
// MemTotal:       12294392 kB
// MemFree:         3652612 kB
// MemAvailable:   11831704 kB
// Buffers:          568536 kB
// Cached:          6421520 kB
// SwapCached:            0 kB
// HugePages_Total:       0
//
// Note: HugePages_* do not end in kB, it is not a typo
//
Status parseProcMemInfo(const std::vector<StringData>& keys,
                        StringData data,
                        BSONObjBuilder* builder) {
    return parseGenericStats(
        keys,
        data,
        [](StringData line, StringSplitIterator& lineIt) {
            lineIt =
                StringSplitIterator(line.begin(),
                                    line.end(),
                                    boost::token_finder([](char c) { return c == ' ' || c == ':'; },
                                                        boost::token_compress_on));
            return stringDataFromRange(*lineIt);
        },
        [&](StringData key, StringSplitIterator& valueIt) {
            StringData value = stringDataFromRange(*valueIt);

            uint64_t numVal;
            if (!NumberParser{}(value, &numVal).isOK()) {
                numVal = 0;
            }

            // Check if the line ends in "kB"
            ++valueIt;

            // If there is one last token, check if it is actually "kB"
            if (valueIt != StringSplitIterator()) {
                StringData kbToken = stringDataFromRange(*valueIt);
                auto keyWithSuffix = std::string{key};

                if (kbToken == "kB") {
                    keyWithSuffix.append("_kb");
                }

                builder->appendNumber(keyWithSuffix, static_cast<long long>(numVal));
            } else {
                builder->appendNumber(key, static_cast<long long>(numVal));
            }
        });
}

Status parseProcMemInfoFile(StringData filename,
                            const std::vector<StringData>& keys,
                            BSONObjBuilder* builder) {
    auto swString = readFileAsString(filename);
    if (!swString.isOK()) {
        return swString.getStatus();
    }

    auto status = parseProcMemInfo(keys, swString.getValue(), builder);
    if (!status.isOK()) {
        status.addContext(fmt::format("Parsing {}", filename));
    }
    return status;
}

//
// Here is an example of the type of string it supports (long lines elided for clarity).
// > cat /proc/net/netstat
// TcpExt: SyncookiesSent SyncookiesRecv SyncookiesFailed ...
// TcpExt: 3437 5938 13368 ...
// IpExt: InNoRoutes InTruncatedPkts InMcastPkts ...
// IpExt: 999 1 4819969 ...
//
// Parser assumes file consists of alternating lines of keys and values
// key and value lines consist of space-separated tokens
// first token is a key prefix that is prepended in the output to each key
// all prefixed keys and corresponding values are copied to output as-is
//

Status parseProcNetstat(const std::vector<StringData>& keys,
                        StringData data,
                        BSONObjBuilder* builder) {
    StringSplitIterator keysIt;
    bool foundKeys = false;

    // Split the file by lines.
    uint32_t lineNum = 0;
    for (StringSplitIterator lineIt = StringSplitIterator(
             data.begin(),
             data.end(),
             boost::token_finder([](char c) { return c == '\n'; }, boost::token_compress_on));
         lineIt != StringSplitIterator();
         ++lineIt, ++lineNum) {

        if (lineNum % 2 == 0) {

            // even numbered lines are keys
            keysIt = StringSplitIterator(
                (*lineIt).begin(),
                (*lineIt).end(),
                boost::token_finder([](char c) { return c == ' '; }, boost::token_compress_on));

        } else {

            // odd numbered lines are values
            StringSplitIterator valuesIt = StringSplitIterator(
                (*lineIt).begin(),
                (*lineIt).end(),
                boost::token_finder([](char c) { return c == ' '; }, boost::token_compress_on));

            StringData prefix;

            // iterate over the keys and values in parallel
            for (uint32_t keyNum = 0;
                 keysIt != StringSplitIterator() && valuesIt != StringSplitIterator();
                 ++keysIt, ++valuesIt, ++keyNum) {

                if (keyNum == 0) {

                    // first token is a prefix to be applied to remaining keys
                    prefix = stringDataFromRange(*keysIt);

                    // ignore line if prefix isn't in requested list
                    if (!keys.empty() && std::find(keys.begin(), keys.end(), prefix) == keys.end())
                        break;

                } else {

                    // remaining tokens are key/value pairs
                    StringData key = stringDataFromRange(*keysIt);
                    StringData stringValue = stringDataFromRange(*valuesIt);
                    uint64_t value;
                    if (NumberParser{}(stringValue, &value).isOK()) {
                        builder->appendNumber(std::string{prefix} + std::string{key},
                                              static_cast<long long>(value));
                        foundKeys = true;
                    }
                }
            }
        }
    }

    return foundKeys ? Status::OK()
                     : Status(ErrorCodes::NoSuchKey, "Failed to find any keys in netstats string");
}

Status parseProcNetstatFile(const std::vector<StringData>& keys,
                            StringData filename,
                            BSONObjBuilder* builder) {
    auto swString = readFileAsString(filename);
    if (!swString.isOK()) {
        return swString.getStatus();
    }
    return parseProcNetstat(keys, swString.getValue(), builder);
}

Status parseProcSockstat(const std::map<StringData, std::set<StringData>>& linesAndKeys,
                         StringData data,
                         BSONObjBuilder* builder) {
    auto newlineFinder =
        boost::token_finder([](char c) { return c == '\n'; }, boost::token_compress_on);
    auto spaceAndColonFinder =
        boost::token_finder([](char c) { return c == ' ' || c == ':'; }, boost::token_compress_on);

    bool foundKeys = false;

    // Split the file by lines.
    for (StringSplitIterator lineIt(data.begin(), data.end(), newlineFinder);
         lineIt != StringSplitIterator();
         ++lineIt) {
        StringData line = stringDataFromRange(*lineIt);
        // Split the line by spaces and colons since these are the delimeters for sockstat files.
        StringSplitIterator partIt(line.begin(), line.end(), spaceAndColonFinder);
        // Check the line-key, which is the first part of the line, to see if we care about it.
        StringData lineKey = stringDataFromRange(*partIt);
        auto bucketIt = linesAndKeys.find(lineKey);
        if (bucketIt == linesAndKeys.end()) {
            // We don't care about this line.
            continue;
        }

        // We do care about this line; extract the values we care about.
        // Start a new document with the line key as the name.
        BSONObjBuilder sub(builder->subobjStart(lineKey));
        ++partIt;
        auto lineKeySet = bucketIt->second;
        while (partIt != StringSplitIterator()) {
            StringData key = stringDataFromRange(*partIt);
            if (!lineKeySet.count(key)) {
                // Don't care about this key/value. Skip past it.
                ++partIt;
                ++partIt;
                continue;
            }
            // We do care about this key. Get the value.
            ++partIt;
            StringData stringValue = stringDataFromRange(*partIt);
            long long value;
            if (!NumberParser{}(stringValue, &value).isOK()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "Couldn't parse '" << stringValue << "' to number");
            }
            sub.appendNumber(std::string{key}, value);
            foundKeys = true;
            ++partIt;
        }
    }
    return foundKeys ? Status::OK()
                     : Status(ErrorCodes::NoSuchKey, "Failed to find any keys in sockStats string");
}

Status parseProcSockstatFile(const std::map<StringData, std::set<StringData>>& keys,
                             StringData filename,
                             BSONObjBuilder* builder) {
    auto swString = readFileAsString(filename);
    if (!swString.isOK()) {
        return swString.getStatus();
    }
    return parseProcSockstat(keys, swString.getValue(), builder);
}


// Here is an example of the type of string it supports:
//
// For more information, see:
// Documentation/iostats.txt in the Linux kernel
// proc(5) man page
//
// > cat /proc/diskstats
//   8       0 sda 120611 33630 6297628 96550 349797 167398 11311562 2453603 0 117514 2554160
//   8       1 sda1 138 37 8642 315 3 0 18 14 0 292 329
//   8       2 sda2 120409 33593 6285754 96158 329029 167398 11311544 2450573 0 115611 2550739
//   8      16 sdb 12707 3876 1525418 57507 997 3561 297576 97976 0 37870 155619
//   8      17 sdb1 12601 3876 1521090 57424 992 3561 297576 97912 0 37738 155468
//  11       0 sr0 0 0 0 0 0 0 0 0 0 0 0
// 253       0 dm-0 154910 0 6279522 177681 506513 0 11311544 5674418 0 117752 5852275
// 253       1 dm-1 109 0 4584 226 0 0 0 0 0 172 226
//
Status parseProcDiskStats(const std::vector<StringData>& disks,
                          StringData data,
                          BSONObjBuilder* builder) {
    bool foundKeys = false;
    std::vector<uint64_t> stats;
    stats.reserve(kDiskFieldCount);

    // Split the file by lines.
    // token_compress_on means the iterator skips over consecutive '\n'. This should not be a
    // problem in normal /proc/diskstats output.
    for (StringSplitIterator lineIt = StringSplitIterator(
             data.begin(),
             data.end(),
             boost::token_finder([](char c) { return c == '\n'; }, boost::token_compress_on));
         lineIt != StringSplitIterator();
         ++lineIt) {

        StringData line = stringDataFromRange(*lineIt);

        // Skip leading whitespace so that the split_iterator starts on non-whitespace otherwise we
        // get an empty first token. Device major numbers (the first number on each line) are right
        // aligned to 4 spaces and start from
        // single digits.
        auto beginNonWhitespace =
            std::find_if_not(line.begin(), line.end(), [](char c) { return c == ' '; });

        // Split the line by spaces since that is the only delimiter for diskstats files.
        // token_compress_on means the iterator skips over consecutive ' '.
        StringSplitIterator partIt = StringSplitIterator(
            beginNonWhitespace,
            line.end(),
            boost::token_finder([](char c) { return c == ' '; }, boost::token_compress_on));

        // Skip processing this line if the line is blank
        if (partIt == StringSplitIterator()) {
            continue;
        }

        ++partIt;

        // Skip processing this line if we only have a device major number.
        if (partIt == StringSplitIterator()) {
            continue;
        }

        ++partIt;

        // Skip processing this line if we only have a device major minor.
        if (partIt == StringSplitIterator()) {
            continue;
        }

        StringData disk = stringDataFromRange(*partIt);

        // Skip processing this line if we only have a block device name.
        if (partIt == StringSplitIterator()) {
            continue;
        }

        ++partIt;

        // Check if the disk is in the list. /proc/diskstats will have extra disks, and may not have
        // the disk we want.
        if (disks.empty() || std::find(disks.begin(), disks.end(), disk) != disks.end()) {
            foundKeys = true;

            stats.clear();

            // Only generate a disk document if the disk has some activity. For instance, there
            // could be a CD-ROM drive that is not used.
            bool hasSomeNonZeroStats = false;

            for (size_t index = 0; partIt != StringSplitIterator() && index < kDiskFieldCount;
                 ++partIt, ++index) {

                StringData stringValue = stringDataFromRange(*partIt);

                uint64_t value;

                if (!NumberParser{}(stringValue, &value).isOK()) {
                    value = 0;
                }

                if (value != 0) {
                    hasSomeNonZeroStats = true;
                }

                stats.push_back(value);
            }

            if (hasSomeNonZeroStats) {
                // Start a new document with disk as the name.
                BSONObjBuilder sub(builder->subobjStart(disk));

                for (size_t index = 0; index < stats.size() && index < kDiskFieldCount; ++index) {
                    sub.appendNumber(kDiskFields[index], static_cast<long long>(stats[index]));
                }

                sub.doneFast();
            }
        }
    }

    return foundKeys ? Status::OK()
                     : Status(ErrorCodes::NoSuchKey, "Failed to find any keys in diskstats string");
}

Status parseProcDiskStatsFile(StringData filename,
                              const std::vector<StringData>& disks,
                              BSONObjBuilder* builder) {
    auto swString = readFileAsString(filename);
    if (!swString.isOK()) {
        return swString.getStatus();
    }

    return parseProcDiskStats(disks, swString.getValue(), builder);
}

Status parseProcSelfMountStatsImpl(
    StringData data,
    BSONObjBuilder* builder,
    std::function<boost::filesystem::space_info(const boost::filesystem::path&,
                                                boost::system::error_code&)> getSpace) {
    invariant(getSpace);
    std::istringstream iss(std::string{data});
    for (std::string line; std::getline(iss, line);) {
        // As described in the /proc/[pid]/mountinfo section of `man 5 proc`:
        //
        // 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
        // |  |  |    |     |     |          |          |      |     |
        // (1)(2)(3:4)(5)   (6)   (7)        (8)        (9)   (10)   (11)
        static const pcre::Regex kRe(R"re(\d+ \d+ \d+:\d+ \S+ (\S+))re");
        if (auto m = kRe.matchView(line)) {
            std::string mountPoint{m[1]};
            boost::filesystem::path p(mountPoint);
            boost::system::error_code ec;
            boost::filesystem::space_info spaceInfo = getSpace(p, ec);
            if (!ec.failed() && spaceInfo.capacity) {
                BSONObjBuilder bob(builder->subobjStart(mountPoint));
                bob.appendNumber("capacity", static_cast<long long>(spaceInfo.capacity));
                bob.appendNumber("available", static_cast<long long>(spaceInfo.available));
                bob.appendNumber("free", static_cast<long long>(spaceInfo.free));
                bob.doneFast();
            }
        }
    }

    return Status::OK();
}

Status parseProcSelfMountStats(StringData data, BSONObjBuilder* builder) {
    auto bfsSpace = [](auto&&... args) {
        return boost::filesystem::space(args...);
    };
    return parseProcSelfMountStatsImpl(data, builder, bfsSpace);
}

Status parseProcSelfMountStatsFile(StringData filename, BSONObjBuilder* builder) {
    auto swString = readFileAsString(filename);
    if (!swString.isOK()) {
        return swString.getStatus();
    }

    return parseProcSelfMountStats(swString.getValue(), builder);
}

namespace {

/**
 * Is this a disk that is interesting to us? We only want physical disks, not multiple disk devices,
 * LVM2 devices, partitions, or RAM disks.
 *
 * A physical disk has a symlink to a directory at /sys/block/<device name>/device.
 *
 * Note: returns false upon any errors such as access denied.
 */
bool isInterestingDisk(const boost::filesystem::path& path) {
    boost::filesystem::path blockDevicePath(path);
    blockDevicePath /= kSysBlockDeviceDirectoryName;

    boost::system::error_code ec;
    auto statusSysBlock = boost::filesystem::status(blockDevicePath, ec);
    if (!boost::filesystem::exists(statusSysBlock)) {
        return false;
    }

    if (ec) {
        LOGV2_WARNING(23912,
                      "Error checking directory '{blockDevicePath_generic_string}': {ec_message}",
                      "blockDevicePath_generic_string"_attr = blockDevicePath.generic_string(),
                      "ec_message"_attr = ec.message());
        return false;
    }

    if (!boost::filesystem::is_directory(statusSysBlock)) {
        return false;
    }

    return true;
}

}  // namespace

std::vector<std::string> findPhysicalDisks(StringData sysBlockPath) {
    boost::system::error_code ec;
    auto sysBlockPathStr = std::string{sysBlockPath};

    auto statusSysBlock = boost::filesystem::status(sysBlockPathStr, ec);
    if (ec) {
        LOGV2_WARNING(23913,
                      "Error checking directory '{sysBlockPathStr}': {ec_message}",
                      "sysBlockPathStr"_attr = sysBlockPathStr,
                      "ec_message"_attr = ec.message());
        return {};
    }

    if (!(boost::filesystem::exists(statusSysBlock) &&
          boost::filesystem::is_directory(statusSysBlock))) {
        LOGV2_WARNING(23914,
                      "Could not find directory '{sysBlockPathStr}': {ec_message}",
                      "sysBlockPathStr"_attr = sysBlockPathStr,
                      "ec_message"_attr = ec.message());
        return {};
    }

    std::vector<std::string> files;

    // Iterate through directories in /sys/block. The directories in this directory can be physical
    // block devices (like SSD or HDD) or virtual devices like the LVM2 device mapper or a multiple
    // disk device. It does not contain disk partitions.
    boost::filesystem::directory_iterator di(sysBlockPathStr, ec);
    if (ec) {
        LOGV2_WARNING(23915,
                      "Error getting directory iterator '{sysBlockPathStr}': {ec_message}",
                      "sysBlockPathStr"_attr = sysBlockPathStr,
                      "ec_message"_attr = ec.message());
        return {};
    }

    for (; di != boost::filesystem::directory_iterator(); di++) {
        auto path = (*di).path();

        if (isInterestingDisk(path)) {
            files.push_back(path.filename().generic_string());
        }
    }

    return files;
}

// Here is an example of the type of string it supports:
// Note: output has been trimmed
//
// For more information, see:
// proc(5) man page
//
// > cat /proc/vmstat
// nr_free_pages 2732282
// nr_zone_inactive_anon 686253
// nr_zone_active_anon 4975441
// nr_zone_inactive_file 2332485
// nr_zone_active_file 4791149
// nr_zone_unevictable 0
// nr_zone_write_pending 0
// nr_mlock 0
//
Status parseProcVMStat(const std::vector<StringData>& keys,
                       StringData data,
                       BSONObjBuilder* builder) {
    return parseGenericStats(
        keys,
        data,
        [](StringData line, StringSplitIterator& lineIt) {
            lineIt = StringSplitIterator(
                line.begin(),
                line.end(),
                boost::token_finder([](char c) { return c == ' '; }, boost::token_compress_on));
            return stringDataFromRange(*lineIt);
        },
        [&](StringData key, StringSplitIterator& valueIt) {
            StringData value = stringDataFromRange(*valueIt);

            uint64_t numVal;
            if (!NumberParser{}(value, &numVal).isOK()) {
                numVal = 0;
            }

            builder->appendNumber(key, static_cast<long long>(numVal));
        });
}

Status parseProcVMStatFile(StringData filename,
                           const std::vector<StringData>& keys,
                           BSONObjBuilder* builder) {
    auto swString = readFileAsString(filename);
    if (!swString.isOK()) {
        return swString.getStatus();
    }

    auto status = parseProcVMStat(keys, swString.getValue(), builder);
    if (!status.isOK()) {
        status.addContext(fmt::format("Parsing {}", filename));
    }
    return status;
}

Status parseProcSysFsFileNr(FileNrKey key, StringData data, BSONObjBuilder* builder) {
    // Format: HANDLES_IN_USE<whitespace>UNUSED_HANDLES<whitespace>MAX_HANDLES<return>
    StringSplitIterator partIt(
        data.begin(),
        data.end(),
        boost::token_finder([](char c) { return c == ' ' || c == '\t' || c == '\n'; },
                            boost::token_compress_on));

    if (partIt == StringSplitIterator()) {
        return Status(ErrorCodes::FailedToParse, "Couldn't find first token");
    }

    if (key == FileNrKey::kFileHandlesInUse) {
        StringData stringValue = stringDataFromRange(*partIt);
        uint64_t value;
        if (!NumberParser{}(stringValue, &value).isOK()) {
            return Status(ErrorCodes::FailedToParse, "Couldn't parse first token to number");
        }

        builder->appendNumber(kFileHandlesInUseKey, static_cast<long long>(value));
        return Status::OK();
    }
    ++partIt;

    if (partIt == StringSplitIterator()) {
        return Status(ErrorCodes::FailedToParse, "Couldn't find second token");
    }
    // The second value is the number of allocated but unused file handles, which should always be
    // 0; we ignore this.
    ++partIt;

    if (partIt == StringSplitIterator()) {
        return Status(ErrorCodes::FailedToParse, "Couldn't find third token");
    }

    invariant(key == FileNrKey::kMaxFileHandles);
    StringData stringValue = stringDataFromRange(*partIt);
    uint64_t value;
    if (!NumberParser{}(stringValue, &value).isOK()) {
        return Status(ErrorCodes::FailedToParse, "Couldn't parse third token to number");
    }

    builder->appendNumber(kMaxFileHandlesKey, static_cast<long long>(value));

    return Status::OK();
}

Status parseProcSysFsFileNrFile(StringData filename, FileNrKey key, BSONObjBuilder* builder) {
    auto swString = readFileAsString(filename);
    if (!swString.isOK()) {
        return swString.getStatus();
    }

    return parseProcSysFsFileNr(key, swString.getValue(), builder);
}

// Here is an example of the type of string it supports:
//
// > cat /proc/pressure/<cpu|io|memory>
// some avg10=0.00 avg60=0.00 avg300=0.14 total=1434509127
// full avg10=0.00 avg60=0.00 avg300=0.14 total=1035574668
//
// Note: /proc/pressure/cpu only has 'some' entry
//
Status parseProcPressure(StringData data, BSONObjBuilder* builder) {
    // Split the file by lines.
    // token_compress_on means the iterator skips over consecutive '\n'.
    for (StringSplitIterator lineIt(
             data.begin(),
             data.end(),
             boost::token_finder([](char c) { return c == '\n'; }, boost::token_compress_on));
         lineIt != StringSplitIterator();
         ++lineIt) {

        StringData line = stringDataFromRange(*lineIt);

        // Split the line by spaces and equal signs since these are the delimiters for pressure
        // files. token_compress_on means the iterator skips over consecutive ' '. This is needed
        // for every line.
        StringSplitIterator partIt(line.begin(),
                                   line.end(),
                                   boost::token_finder([](char c) { return c == ' ' || c == '='; },
                                                       boost::token_compress_on));

        // Skip processing this line if we do not have a key.
        if (partIt == StringSplitIterator()) {
            continue;
        }

        StringData time = stringDataFromRange(*partIt);

        ++partIt;

        // Skip processing this line if we only have a key, and no arguments.
        if (partIt == StringSplitIterator()) {
            continue;
        }

        // Share of time is either 'some' or 'full'.
        if (time != kPressureSomeTime && time != kPressureFullTime) {
            return Status(ErrorCodes::FailedToParse, "Couldn't find the share of time");
        }

        // Lookup for 'total' token in the parts.
        auto totalIt = std::find_if(partIt, StringSplitIterator(), [](const auto& vec) {
            return stringDataFromRange(vec) == "total"_sd;
        });

        // If 'total' token is not found on the row return an error.
        if (totalIt == StringSplitIterator()) {
            return Status(ErrorCodes::NoSuchKey, "Failed to find 'total' token");
        }

        [[maybe_unused]] StringData totalToken = stringDataFromRange(*totalIt);

        ++totalIt;

        if (totalIt == StringSplitIterator()) {
            return Status(ErrorCodes::FailedToParse, "No value found for 'total' token");
        }

        StringData stringValue = stringDataFromRange(*totalIt);

        double value;

        if (!NumberParser{}(stringValue, &value).isOK()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Couldn't parse '" << stringValue << "' to number");
        }

        *builder << time << BSON("totalMicros" << value);
    }

    return Status::OK();
}

// Example of BSONObjBuilder created:
//  cpu : {
//    some : {
//       totalMicros : ...
//    },
//    fulll: {
//       totalMicros : ...
//     }
//  }
Status parseProcPressureFile(StringData key, StringData filename, BSONObjBuilder* builder) {
    auto swString = readFileAsString(filename);
    if (!swString.isOK()) {
        return swString.getStatus();
    }

    BSONObjBuilder sub(builder->subobjStart(key));
    Status status = parseProcPressure(swString.getValue(), &sub);
    sub.doneFast();

    return status;
}

}  // namespace procparser
}  // namespace mongo
