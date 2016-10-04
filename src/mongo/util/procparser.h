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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
 * Get a vector of disks to monitor by enumerating the specified directory.
 *
 * If the directory does not exist, or otherwise permission is denied, returns an empty vector.
 */
std::vector<std::string> findPhysicalDisks(StringData directory);

}  // namespace procparser
}  // namespace mongo
