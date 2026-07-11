// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

[[MONGO_MOD_PARENT_PRIVATE]];
namespace mongo::timeseries::bucket_catalog {

/**
 * Mode enum to determine the rollover type decision for a given bucket.
 */
enum class RolloverAction {
    kNone,       // Keep bucket open
    kArchive,    // Archive bucket
    kSoftClose,  // Close bucket so it remains eligible for reopening
    kHardClose,  // Permanently close bucket
};

/**
 * Reasons why a bucket was rolled over.
 */
enum class RolloverReason {
    kNone,           // Not actually rolled over
    kTimeForward,    // Measurement time would violate max span for this bucket
    kTimeBackward,   // Measurement time was before bucket min time
    kCount,          // Adding this measurement would violate max count
    kSchemaChange,   // This measurement has a schema incompatible with existing measurements
    kCachePressure,  // System is under cache pressure, and adding this measurement would make
                     // the bucket larger than the dynamic size limit
    kSize,  // Adding this measurement would make the bucket larger than the normal size limit
};

/**
 * Returns the RolloverAction based on the RolloverReason.
 */
RolloverAction getRolloverAction(RolloverReason reason);

}  // namespace mongo::timeseries::bucket_catalog
