/**
 * Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Configuration settings for full-time diagnostic data capture (FTDC).
 *
 * These settings are configurable by the user at startup & runtime via setParameter.
 */
struct FTDCConfig {
    FTDCConfig()
        : enabled(kEnabledDefault),
          maxDirectorySizeBytes(kMaxDirectorySizeBytesDefault),
          maxFileSizeBytes(kMaxFileSizeBytesDefault),
          period(kPeriodMillisDefault),
          maxSamplesPerArchiveMetricChunk(kMaxSamplesPerArchiveMetricChunkDefault),
          maxSamplesPerInterimMetricChunk(kMaxSamplesPerInterimMetricChunkDefault) {}

    /**
     * True if FTDC is collecting data. False otherwise
     */
    bool enabled;

    /**
     * Max Size of all FTDC files. If the total file size is > maxDirectorySizeBytes by summing up
     * all files in the FTDC directory, the extra files are removed.
     */
    std::uint64_t maxDirectorySizeBytes;

    /**
     * Max size of a file in bytes.
     */
    std::uint64_t maxFileSizeBytes;

    /**
     * Period at which to run FTDC.
     *
     * FTDC is always run at the beginning at the period, and skips periods if the collector runs
     * for more then a single period.
     */
    Milliseconds period;

    /**
     * Maximum number of samples to collect in an archive metric chunk for long term storage.
     */
    std::uint32_t maxSamplesPerArchiveMetricChunk;

    /**
     * Maximum number of samples to collect in an interim metric chunk in case the process
     * terminates.
     */
    std::uint32_t maxSamplesPerInterimMetricChunk;

    static const bool kEnabledDefault = true;

    static const std::int64_t kPeriodMillisDefault;
    static const std::uint64_t kMaxDirectorySizeBytesDefault = 100 * 1024 * 1024;
    static const std::uint64_t kMaxFileSizeBytesDefault = 10 * 1024 * 1024;

    static const std::uint64_t kMaxFileUniqifier = 65000;

    static const std::uint32_t kMaxSamplesPerArchiveMetricChunkDefault = 300;
    static const std::uint32_t kMaxSamplesPerInterimMetricChunkDefault = 10;
};

}  // namespace mongo
