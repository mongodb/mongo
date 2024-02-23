/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_catalog/write_batch.h"

#include <absl/container/node_hash_set.h>
#include <boost/container/vector.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/iterator/transform_iterator.hpp>
#include <set>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo::timeseries::bucket_catalog {

WriteBatch::WriteBatch(TrackingContext& trackingContext,
                       const BucketHandle& b,
                       BucketKey k,
                       OperationId o,
                       ExecutionStatsController& s,
                       StringData timeField)
    : bucketHandle(b),
      bucketKey(std::move(k)),
      opId(o),
      stats(s),
      timeField(timeField),
      intermediateBuilders(trackingContext) {}

BSONObj WriteBatch::toBSON() const {
    auto toFieldName = [](const auto& nameHashPair) {
        return nameHashPair.first;
    };
    return BSON("docs" << std::vector<BSONObj>(measurements.begin(), measurements.end())
                       << "bucketMin" << min << "bucketMax" << max << "numCommittedMeasurements"
                       << int(numPreviouslyCommittedMeasurements) << "newFieldNamesToBeInserted"
                       << std::set<std::string>(boost::make_transform_iterator(
                                                    newFieldNamesToBeInserted.begin(), toFieldName),
                                                boost::make_transform_iterator(
                                                    newFieldNamesToBeInserted.end(), toFieldName)));
}

bool claimWriteBatchCommitRights(WriteBatch& batch) {
    return !batch.commitRights.swap(true);
}

StatusWith<CommitInfo> getWriteBatchResult(WriteBatch& batch) {
    if (!batch.promise.getFuture().isReady()) {
        batch.stats.incNumWaits();
    }
    return batch.promise.getFuture().getNoThrow();
}

bool isWriteBatchFinished(const WriteBatch& batch) {
    return batch.promise.getFuture().isReady();
}

}  // namespace mongo::timeseries::bucket_catalog
