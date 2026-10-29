// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/tracking/context.h"

[[MONGO_MOD_PARENT_PRIVATE]];
namespace mongo::timeseries::bucket_catalog {

/**
 * The specific data scope for the memory being allocated, i.e. what the memory will be used for.
 */
enum class TrackingScope {
    kArchivedBuckets,
    kBucketStateRegistry,
    kColumnBuilders,
    kIdleBuckets,
    kMiscellaneous,
    kOpenBucketsById,
    kOpenBucketsByKey,
    kReopeningRequests,
    kStats,
    kSummaries,
    kMeasurementBatching,
};

/**
 * A slight abstraction around the tracking::Context to allow us to extract the correct context for
 * the scope chosen when running with detailed diagnostics in debug mode. Still allows us to extract
 * the single context when running normally for release builds.
 */
struct TrackingContexts {
#ifndef MONGO_CONFIG_DEBUG_BUILD
    tracking::Context global;
#else
    tracking::Context archivedBuckets;
    tracking::Context bucketStateRegistry;
    tracking::Context columnBuilders;
    tracking::Context idleBuckets;
    tracking::Context miscellaneous;
    tracking::Context openBucketsById;
    tracking::Context openBucketsByKey;
    tracking::Context reopeningRequests;
    tracking::Context stats;
    tracking::Context summaries;
    tracking::Context measurementBatching;
#endif
};

/**
 * Selects the correct tracking context for the specified scope.
 */
constexpr tracking::Context& getTrackingContext(TrackingContexts& contexts, TrackingScope scope) {
#ifndef MONGO_CONFIG_DEBUG_BUILD
    return contexts.global;
#else
    switch (scope) {
        case TrackingScope::kArchivedBuckets:
            return contexts.archivedBuckets;
        case TrackingScope::kBucketStateRegistry:
            return contexts.bucketStateRegistry;
        case TrackingScope::kColumnBuilders:
            return contexts.columnBuilders;
        case TrackingScope::kIdleBuckets:
            return contexts.idleBuckets;
        case TrackingScope::kMiscellaneous:
            return contexts.miscellaneous;
        case TrackingScope::kOpenBucketsById:
            return contexts.openBucketsById;
        case TrackingScope::kOpenBucketsByKey:
            return contexts.openBucketsByKey;
        case TrackingScope::kReopeningRequests:
            return contexts.reopeningRequests;
        case TrackingScope::kStats:
            return contexts.stats;
        case TrackingScope::kSummaries:
            return contexts.summaries;
        case TrackingScope::kMeasurementBatching:
            return contexts.measurementBatching;
    }
    MONGO_UNREACHABLE;
#endif
}

}  // namespace mongo::timeseries::bucket_catalog
