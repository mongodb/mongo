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

#include <algorithm>
#include <array>
#include <boost/algorithm/string/finder.hpp>
#include <boost/algorithm/string/split.hpp>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>

#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/text.h"
namespace mongo {

namespace {

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
    int fd = open(filename.toString().c_str(), 0);
    if (fd == -1) {
        int err = errno;
        return Status(ErrorCodes::FileOpenFailed,
                      str::stream() << "Failed to open file " << filename << " with error: "
                                    << errnoWithDescription(err));
    }
    auto scopedGuard = MakeGuard([fd] { close(fd); });

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
                int err = errno;

                // Retry if we hit EGAIN or EINTR a few times before giving up
                if (retry < kFileReadRetryCount && (err == EAGAIN || err == EINTR)) {
                    ++retry;
                    continue;
                }

                return Status(ErrorCodes::FileStreamFailed,
                              str::stream() << "Failed to read file " << filename << " with error: "
                                            << errnoWithDescription(err));
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

}  // namespace

namespace procparser {
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
    bool foundKeys = false;

    using string_split_iterator = boost::split_iterator<StringData::const_iterator>;

    // Split the file by lines.
    // token_compress_on means the iterator skips over consecutive '\n'. This should not be a
    // problem in normal /proc/stat output.
    for (string_split_iterator lineIt = string_split_iterator(
             data.begin(),
             data.end(),
             boost::token_finder([](char c) { return c == '\n'; }, boost::token_compress_on));
         lineIt != string_split_iterator();
         ++lineIt) {

        StringData line((*lineIt).begin(), (*lineIt).end());

        // Split the line by spaces since that is the only delimiter for stat files.
        // token_compress_on means the iterator skips over consecutive ' '. This is needed for the
        // first line which is "cpu  <number>".
        string_split_iterator partIt = string_split_iterator(
            line.begin(),
            line.end(),
            boost::token_finder([](char c) { return c == ' '; }, boost::token_compress_on));

        // Skip processing this line if we do not have a key.
        if (partIt == string_split_iterator()) {
            continue;
        }

        StringData key((*partIt).begin(), (*partIt).end());

        ++partIt;

        // Skip processing this line if we only have a key, and no number.
        if (partIt == string_split_iterator()) {
            continue;
        }

        // Check if the key is in the list. /proc/stat will have extra keys, and
        // may not have the keys we want.
        if (keys.empty() || std::find(keys.begin(), keys.end(), key) != keys.end()) {

            foundKeys = true;

            if (key == "cpu") {
                // Cpu is 10 fields, we need to chew through all of them.
                // Some kernels we support lack the last field or two: guest and/or guest_nice.
                for (size_t index = 0;
                     partIt != string_split_iterator() && index < kAdditionCpuFieldCount;
                     ++partIt, ++index) {

                    StringData stringValue((*partIt).begin(), (*partIt).end() - (*partIt).begin());

                    uint64_t value;

                    if (!parseNumberFromString(stringValue, &value).isOK()) {
                        value = 0;
                    }

                    builder->appendNumber(kAdditionCpuFields[index],
                                          convertTicksToMilliSeconds(value, ticksPerSecond));
                }
            } else {
                StringData stringValue((*partIt).begin(), (*partIt).end() - (*partIt).begin());

                uint64_t value;

                if (!parseNumberFromString(stringValue, &value).isOK()) {
                    value = 0;
                }

                builder->appendNumber(key, value);
            }
        }
    }

    return foundKeys ? Status::OK()
                     : Status(ErrorCodes::NoSuchKey, "Failed to find any keys in stat string");
}

Status parseProcStatFile(StringData filename,
                         const std::vector<StringData>& keys,
                         BSONObjBuilder* builder) {
    auto swString = readFileAsString(filename);
    if (!swString.isOK()) {
        return swString.getStatus();
    }

    return parseProcStat(keys, swString.getValue(), getTicksPerSecond(), builder);
}

}  // namespace procparser
}  // namespace mongo
