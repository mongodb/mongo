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

#include "mongo/db/timeseries/bucket_catalog/measurement_map.h"
#include "mongo/db/timeseries/timeseries_tracked_types.h"

namespace mongo::timeseries::bucket_catalog_internal {

struct MovableState {
    MovableState(TrackingContext& trackingContext)
        : intermediateBuilders(trackingContext),
          uncompressedBucketDoc(makeTrackedBson(trackingContext, {})) {}

    // Whether the measurements in the bucket are sorted by timestamp or not. True by default,
    // if a v2 buckets gets promoted to v3 this is set to false. It should not be used for v1
    // buckets.
    bool bucketIsSortedByTime = true;

    // In-memory state of each committed data field. Enables fewer complete round-trips of
    // decompression + compression.
    bucket_catalog::MeasurementMap intermediateBuilders;

    // The uncompressed bucket. Only set when reopening uncompressed buckets and the always
    // compressed feature flag is enabled. Used to convert an uncompressed bucket to a
    // compressed bucket on the next insert, and will be cleared when finished.
    // TODO SERVER-86542: remove this member.
    TrackedBSONObj uncompressedBucketDoc;
};

}  // namespace mongo::timeseries::bucket_catalog_internal
