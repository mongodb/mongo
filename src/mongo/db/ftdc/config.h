// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>

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
          metadataCaptureFrequency(kMetadataCaptureFrequencyDefault),
          sampleTimeout(kSampleTimeoutMillisDefault),
          minThreads(kMinThreadsDefault),
          maxThreads(kMaxThreadsDefault),
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
     * Period at which to collect configuration Metadata.
     *
     * Represents how often configuration metadata is collected relative to the overall period. For
     * instance, if metadataCaptureFrequency is 5, then the period of configuration metadata
     * collection is 5*period
     */
    std::uint64_t metadataCaptureFrequency;

    /**
     * Timeout on how long the controller should wait (in MS) for a collection to finish running.
     * This only applies to async FTDC collections.
     */
    Milliseconds sampleTimeout;

    /**
     * The minimum number of threads that the async collector thread pools should keep alive.
     */
    size_t minThreads;

    /**
     * The maximum number of threads that the async collector thread pools can scale to.
     */
    size_t maxThreads;

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
    static const std::uint64_t kMetadataCaptureFrequencyDefault;
    static const std::int64_t kSampleTimeoutMillisDefault;
    static const std::uint64_t kMinThreadsDefault;
    static const std::uint64_t kMaxThreadsDefault;
    static const std::uint64_t kMaxDirectorySizeBytesDefault = 500 * 1024 * 1024;
    static const std::uint64_t kMaxFileSizeBytesDefault = 10 * 1024 * 1024;

    static const std::uint64_t kMaxFileUniqifier = 65000;

    static const std::uint32_t kMaxSamplesPerArchiveMetricChunkDefault = 300;
    static const std::uint32_t kMaxSamplesPerInterimMetricChunkDefault = 10;
};

}  // namespace mongo
