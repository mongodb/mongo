// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_catalog/write_batch.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/container/vector.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/move/utility_core.hpp>

namespace mongo::timeseries::bucket_catalog {

WriteBatch::WriteBatch(TrackingContexts& tcs,
                       const BucketId& b,
                       BucketKey k,
                       OperationId o,
                       ExecutionStatsController& s,
                       std::string_view timeField)
    : opId(o),
      trackingContexts(tcs),
      timeField(timeField),
      stats(s),
      bucketId(b),
      measurementMap(getTrackingContext(tcs, TrackingScope::kColumnBuilders)),
      bucketKey(std::move(k)) {}

BSONObj WriteBatch::toBSON() const {
    auto toFieldName = [](const auto& nameHashPair) {
        return nameHashPair.first;
    };

    return BSONObjBuilder{}
        .append("isReopened", isReopened)
        .append("bucketIsSortedByTime", bucketIsSortedByTime)
        .append("openedDueToMetadata", openedDueToMetadata)
        .append("opId", int(opId))
        .append("numCommittedMeasurements", int(numPreviouslyCommittedMeasurements))
        .append("timeField", timeField)
        .append("bucketMin", min)
        .append("bucketMax", max)
        .append("userBatchIndices",
                std::vector<int>(userBatchIndices.begin(), userBatchIndices.end()))
        .append("stmtIds", std::vector<int>(stmtIds.begin(), stmtIds.end()))
        .append("newFieldNamesToBeInserted",
                std::set<std::string>(
                    boost::make_transform_iterator(newFieldNamesToBeInserted.begin(), toFieldName),
                    boost::make_transform_iterator(newFieldNamesToBeInserted.end(), toFieldName)))
        .append("measurements", std::vector<BSONObj>(measurements.begin(), measurements.end()))
        .append("bucketOID", bucketId.oid)
        .append("bucketMetaData", bucketKey.metadata.toBSON())
        .obj();
}

Status getWriteBatchStatus(WriteBatch& batch) {
    if (!batch.promise.getFuture().isReady()) {
        batch.stats.incNumWaits();
    }
    return batch.promise.getFuture().getNoThrow();
}

bool isWriteBatchFinished(const WriteBatch& batch) {
    return batch.promise.getFuture().isReady();
}

}  // namespace mongo::timeseries::bucket_catalog
