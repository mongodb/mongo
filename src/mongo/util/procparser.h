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

#pragma once

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"

namespace mongo {

class BSONObjBuilder;

namespace procparser {

/**
 * Reads a string matching /proc/stat format, and writes the values of the specified keys into
 * builder. If fields are not in the "data" parameter, they are omitted. Converts fields
 * from USER_HZ to milliseconds, and names fields with a "_ms" suffix. If the string is empty,
 * corrupt, or missing fields, the builder will simply be missing fields.
 *
 * keys - sorted vector of field names to include in the output, "cpu" will include the 11 fields
 *        that make up cpu. If keys is empty, all keys are outputed.
 * data - string to parsee
 * ticksPerSecond - USER_HZ value
 * builder - BSON output
 */
Status parseProcStat(const std::vector<StringData>& keys,
                     StringData data,
                     int64_t ticksPerSecond,
                     BSONObjBuilder* builder);

/**
 * Read from file, and write the specified list of keys into builder.
 *
 * See parseProcStat.
 *
 * Returns Status errors on file reading issues.
 */
Status parseProcStatFile(StringData filename,
                         const std::vector<StringData>& keys,
                         BSONObjBuilder* builder);

/**
 * Read a string matching /proc/meminfo format, and write the specified list of keys in builder.
 *
 * keys - list of keys to output in BSON. If keys is empty, all keys are outputed.
 * data - string to parsee
 * builder - BSON output
 */
Status parseProcMemInfo(const std::vector<StringData>& keys,
                        StringData data,
                        BSONObjBuilder* builder);

/**
 * Read from file, and write the specified list of keys in builder.
 */
Status parseProcMemInfoFile(StringData filename,
                            const std::vector<StringData>& keys,
                            BSONObjBuilder* builder);

/**
 * Read a string matching /proc/net/netstat format, and write the keys
 * found in that string into builder.
 *
 * data - string to parse
 * builder - BSON output
 */
Status parseProcNetstat(const std::vector<StringData>& keys,
                        StringData data,
                        BSONObjBuilder* builder);
/**
 * Read from file, and write the keys found in that file into builder.
 */
Status parseProcNetstatFile(const std::vector<StringData>& keys,
                            StringData filename,
                            BSONObjBuilder* builder);


/**
 * Read a string matching /proc/net/sockstat format, and write the relevant keys found into builder.
 * /proc/net/sockstat lines begin with a section key, like "sockets" or "TCP". The line continues
 * with space-separated pairs of keys and values until a newline begins a new section.
 *
 * keys - Map of section-keys to the relevant keys in that section.
 * data - string to parse
 * builder - BSON output. A sub-document for each request section is written with the requested
 * key-value pairs.
 *
 * For example, if the /proc/net/sockstat contained:
 *
 * sockets: used 299
 * TCP: inuse 8 orphan 0 tw 0 alloc 12 mem 1
 * ...
 *
 * And `keys` contained:
 *    keys["sockets"] = {"used"}
 *    keys["TCP" = {"alloc", "inuse"}
 *
 * The resulting BSONObj would look like:
 *    {
 *        "sockets": {
 *           "used": 299,
 *        },
 *        "TCP": {
 *           "inuse": 8,
 *           "alloc": 12,
 *        }
 *    }
 */
Status parseProcSockstat(const std::map<StringData, std::set<StringData>>& keys,
                         StringData data,
                         BSONObjBuilder* builder);
/**
 * Read from file, and write the keys found in that file into builder.
 * See the above parseProcSockStats for details on the arguments and format
 * of the BSONObj written into builder.
 */
Status parseProcSockstatFile(const std::map<StringData, std::set<StringData>>& keys,
                             StringData filename,
                             BSONObjBuilder* builder);

/**
 * Read a string matching /proc/diskstats format, and write the specified list of disks in builder.
 *
 * disks - vector of block devices to include in output. For each disk selected, 11 fields are
 *         output in a nested document. There is no error if the disk is not found in the data. Also
 *         a disk is excluded if it has no activity since startup (i.e. an idle CD-ROM drive). If
 *         disks is empty, all non-zero block devices are outputed (this will include partitions,
 *         etc).
 * data - string to parsee
 * builder - BSON output
 */
Status parseProcDiskStats(const std::vector<StringData>& disks,
                          StringData data,
                          BSONObjBuilder* builder);

/**
 * Read from file, and write the specified list of disks in builder.
 */
Status parseProcDiskStatsFile(StringData filename,
                              const std::vector<StringData>& disks,
                              BSONObjBuilder* builder);

/**
 * Read a string matching /proc/self/mountinfo format and parse for any existing mounts.
 *
 * data - The string to parse.
 * builder - BSON output that the disk information is written to.
 * getSpace - A function that takes in a filesystem path to search for mounts on and an error_code
 * argument to return any errors. Disk space information is returned
 */
Status parseProcSelfMountStatsImpl(
    StringData data,
    BSONObjBuilder* builder,
    std::function<boost::filesystem::space_info(const boost::filesystem::path&,
                                                boost::system::error_code&)> getSpace);

/**
 * Read from file, and write the used/free space data for available mounts.
 */
Status parseProcSelfMountStatsFile(StringData filename, BSONObjBuilder* builder);

/**
 * Read a string matching /proc/self/status format and parse for any matching keys.
 *
 * keys - list of keys to output in BSON. If keys is empty, all keys are outputed.
 * data - string to parse
 * builder - BSON output
 */
Status parseProcSelfStatus(const std::vector<StringData>& keys,
                           StringData data,
                           BSONObjBuilder* builder);

/**
 * Read from file, and write the specified list of keys in builder.
 */
Status parseProcSelfStatusFile(StringData filename,
                               const std::vector<StringData>& keys,
                               BSONObjBuilder* builder);

/**
 * Get a vector of disks to monitor by enumerating the specified directory.
 *
 * If the directory does not exist, or otherwise permission is denied, returns an empty vector.
 */
std::vector<std::string> findPhysicalDisks(StringData directory);

/**
 * Read a string matching /proc/vmstat format, and write the specified list of keys in builder.
 *
 * keys - list of keys to output in BSON. If keys is empty, all keys are outputed.
 * data - string to parsee
 * builder - BSON output
 */
Status parseProcVMStat(const std::vector<StringData>& keys,
                       StringData data,
                       BSONObjBuilder* builder);

/**
 * Read from file, and write the specified list of keys in builder.
 */
Status parseProcVMStatFile(StringData filename,
                           const std::vector<StringData>& keys,
                           BSONObjBuilder* builder);

static const StringData kFileHandlesInUseKey = "sys_file_handles_in_use"_sd;
static const StringData kMaxFileHandlesKey = "sys_max_file_handles"_sd;

enum class FileNrKey {
    kFileHandlesInUse,
    kMaxFileHandles,
};

/**
 * Read a string matching /proc/fs/sys/file-nr format, and write the specified keys in builder.
 *
 * appendFileHandlesInUse - if true, append the number of currently used file handles with the key
 * kFileHandlesInUseKey.
 * appendMaxFileHandles - if true, append the maximum number of file handles with the key
 * kMaxFileHandlesKey.
 * data - string to parse
 * builder - BSON output
 */
Status parseProcSysFsFileNr(FileNrKey key, StringData data, BSONObjBuilder* builder);

/**
 * Read from file, and write the specified keys in builder.
 */
Status parseProcSysFsFileNrFile(StringData filename, FileNrKey key, BSONObjBuilder* builder);

static const StringData kPressureSomeTime = "some"_sd;
static const StringData kPressureFullTime = "full"_sd;

/**
 * Read a string matching /proc/pressure/<cpu|io|memory> format and write the specified keys in
 * builder.
 *
 * keys - list of keys to check for in the data and output its value.
 * data - string to parsee
 * builder - BSON output
 */
Status parseProcPressure(StringData data, BSONObjBuilder* builder);

/**
 * Read from file, and write the specified keys in builder.
 */
Status parseProcPressureFile(StringData key, StringData filename, BSONObjBuilder* builder);

}  // namespace procparser
}  // namespace mongo
