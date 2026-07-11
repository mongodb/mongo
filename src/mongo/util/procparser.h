// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class BSONObjBuilder;

namespace procparser {
using namespace std::literals::string_view_literals;

enum class FileNrKey {
    kFileHandlesInUse,
    kMaxFileHandles,
};

/**
 * Read from file, and write the specified list of keys into builder.
 *
 * See parseProcStat.
 *
 * Returns Status errors on file reading issues.
 */
Status parseProcStatFile(std::string_view filename,
                         const std::vector<std::string_view>& keys,
                         BSONObjBuilder* builder);

/**
 * Read from file, and write the specified list of keys in builder.
 */
Status parseProcMemInfoFile(std::string_view filename,
                            const std::vector<std::string_view>& keys,
                            BSONObjBuilder* builder);

/**
 * Read from file, and write the keys found in that file into builder.
 */
Status parseProcNetstatFile(const std::vector<std::string_view>& keys,
                            std::string_view filename,
                            BSONObjBuilder* builder);

/**
 * Read from file, and write the keys found in that file into builder.
 * See the above parseProcSockStats for details on the arguments and format
 * of the BSONObj written into builder.
 */
Status parseProcSockstatFile(const std::map<std::string_view, std::set<std::string_view>>& keys,
                             std::string_view filename,
                             BSONObjBuilder* builder);

/**
 * Read from file, and write the specified list of disks in builder.
 */
Status parseProcDiskStatsFile(std::string_view filename,
                              const std::vector<std::string_view>& disks,
                              BSONObjBuilder* builder);

/**
 * Read from file, and write the used/free space data for available mounts.
 */
Status parseProcSelfMountStatsFile(std::string_view filename, BSONObjBuilder* builder);

/**
 * Get a vector of disks to monitor by enumerating the specified directory.
 *
 * If the directory does not exist, or otherwise permission is denied, returns an empty vector.
 */
std::vector<std::string> findPhysicalDisks(std::string_view directory);

/**
 * Read from file, and write the specified list of keys in builder.
 */
Status parseProcVMStatFile(std::string_view filename,
                           const std::vector<std::string_view>& keys,
                           BSONObjBuilder* builder);

static const std::string_view kFileHandlesInUseKey = "sys_file_handles_in_use"sv;
static const std::string_view kMaxFileHandlesKey = "sys_max_file_handles"sv;

/**
 * Read from file, and write the specified keys in builder.
 */
Status parseProcSysFsFileNrFile(std::string_view filename, FileNrKey key, BSONObjBuilder* builder);

/**
 * Read from file, and write the specified keys in builder.
 */
Status parseProcPressureFile(std::string_view key,
                             std::string_view filename,
                             BSONObjBuilder* builder);

/**
 * Procfs files such as the files parsed by this component should be just a small to moderate amount
 * of text. To protect us from abnormally huge or even corrupted procfs files, we enforce a max size
 * on them. The ability to adjust this limit is allowed as a safety measure for the safety measure.
 */
void setProcFileSizeLimit(size_t limit);
size_t getProcFileSizeLimit();

//
// The rest of this file is implementation details that are only in the header to support testing.
//

namespace [[MONGO_MOD_FILE_PRIVATE]] detail {
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
Status parseProcStat(const std::vector<std::string_view>& keys,
                     std::string_view data,
                     int64_t ticksPerSecond,
                     BSONObjBuilder* builder);

/**
 * Read a string matching /proc/meminfo format, and write the specified list of keys in builder.
 *
 * keys - list of keys to output in BSON. If keys is empty, all keys are outputed.
 * data - string to parsee
 * builder - BSON output
 */
Status parseProcMemInfo(const std::vector<std::string_view>& keys,
                        std::string_view data,
                        BSONObjBuilder* builder);

/**
 * Read a string matching /proc/net/netstat format, and write the keys
 * found in that string into builder.
 *
 * data - string to parse
 * builder - BSON output
 */
Status parseProcNetstat(const std::vector<std::string_view>& keys,
                        std::string_view data,
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
Status parseProcSockstat(const std::map<std::string_view, std::set<std::string_view>>& keys,
                         std::string_view data,
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
Status parseProcDiskStats(const std::vector<std::string_view>& disks,
                          std::string_view data,
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
    std::string_view data,
    BSONObjBuilder* builder,
    std::function<boost::filesystem::space_info(const boost::filesystem::path&,
                                                boost::system::error_code&)> getSpace);

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
Status parseProcSysFsFileNr(FileNrKey key, std::string_view data, BSONObjBuilder* builder);

/**
 * Read a string matching /proc/vmstat format, and write the specified list of keys in builder.
 *
 * keys - list of keys to output in BSON. If keys is empty, all keys are outputed.
 * data - string to parsee
 * builder - BSON output
 */
Status parseProcVMStat(const std::vector<std::string_view>& keys,
                       std::string_view data,
                       BSONObjBuilder* builder);

static const std::string_view kPressureSomeTime = "some"sv;
static const std::string_view kPressureFullTime = "full"sv;

/**
 * Read a string matching /proc/pressure/<cpu|io|memory> format and write the specified keys in
 * builder.
 *
 * keys - list of keys to check for in the data and output its value.
 * data - string to parsee
 * builder - BSON output
 */
Status parseProcPressure(std::string_view data, BSONObjBuilder* builder);

}  // namespace detail

}  // namespace procparser
}  // namespace mongo
