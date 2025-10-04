/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/util/tracking/context.h"

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
