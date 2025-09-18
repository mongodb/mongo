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

#include "mongo/db/timeseries/bucket_catalog/bucket.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::timeseries::bucket_catalog {

namespace {
uint8_t numDigits(uint32_t num) {
    uint8_t numDigits = 0;
    while (num) {
        num /= 10;
        ++numDigits;
    }
    return numDigits;
}
}  // namespace

Bucket::Bucket(TrackingContexts& trackingContexts,
               const BucketId& bId,
               BucketKey k,
               StringData tf,
               Date_t mt,
               BucketStateRegistry& bsr)
    : minTime(mt),
      lastChecked(getCurrentEraAndIncrementBucketCount(bsr)),
      bucketStateRegistry(bsr),
      bucketId(bId),
      timeField(tracking::make_string(
          getTrackingContext(trackingContexts, TrackingScope::kOpenBucketsById),
          tf.data(),
          tf.size())),
      key(std::move(k)),
      fieldNames(tracking::makeStringSet(
          getTrackingContext(trackingContexts, TrackingScope::kOpenBucketsById))),
      uncommittedFieldNames(tracking::makeStringSet(
          getTrackingContext(trackingContexts, TrackingScope::kOpenBucketsById))),
      batches(tracking::make_unordered_map<OperationId, std::shared_ptr<WriteBatch>>(
          getTrackingContext(trackingContexts, TrackingScope::kOpenBucketsById))),
      minmax(getTrackingContext(trackingContexts, TrackingScope::kSummaries)),
      schema(getTrackingContext(trackingContexts, TrackingScope::kSummaries)),
      measurementMap(getTrackingContext(trackingContexts, TrackingScope::kColumnBuilders)) {}

Bucket::~Bucket() {
    decrementBucketCountForEra(bucketStateRegistry, lastChecked);
}

bool allCommitted(const Bucket& bucket) {
    return bucket.batches.empty() && !bucket.preparedBatch;
}

bool schemaIncompatible(Bucket& bucket,
                        const BSONObj& input,
                        boost::optional<StringData> metaField,
                        const StringDataComparator* comparator) {
    auto result = bucket.schema.update(input, metaField, comparator);
    return (result == Schema::UpdateStatus::Failed);
}

void calculateBucketFieldsAndSizeChange(TrackingContexts& trackingContexts,
                                        const Bucket& bucket,
                                        const BSONObj& doc,
                                        boost::optional<StringData> metaField,
                                        Bucket::NewFieldNames& newFieldNamesToBeInserted,
                                        Sizes& sizesToBeAdded) {
    // BSON size for an object with an empty object field where field name is empty string.
    // We can use this as an offset to know the size when we have real field names.
    static constexpr int emptyObjSize = 12;
    // Validate in debug builds that this size is correct
    dassert(emptyObjSize == BSON("" << BSONObj()).objsize());

    newFieldNamesToBeInserted.clear();
    sizesToBeAdded.uncommittedVerifiedSize = 0;
    sizesToBeAdded.uncommittedMeasurementEstimate = 0;

    auto numMeasurementsFieldLength = numDigits(bucket.numMeasurements);

    // When a measurement larger than the threshold (in bytes) is being inserted into a bucket, we
    // use the measurements uncompressed size towards the bucket size limit.
    const int32_t largeMeasurementThreshold = gTimeseriesLargeMeasurementThreshold.load();

    for (const auto& elem : doc) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == metaField) {
            // Only account for the meta field size once, on bucket insert, since it is stored
            // uncompressed at the top-level of the bucket.
            if (bucket.size == 0) {
                sizesToBeAdded.uncommittedVerifiedSize +=
                    kBucketMetaFieldName.size() + elem.size() - elem.fieldNameSize();
            }
            continue;
        }

        auto hashedKey = tracking::StringSet::hasher().hashed_key(
            getTrackingContext(trackingContexts, TrackingScope::kOpenBucketsById), fieldName);
        if (!bucket.fieldNames.contains(hashedKey)) {
            // Record the new field name only if it hasn't been committed yet. There could
            // be concurrent batches writing to this bucket with the same new field name,
            // but they're not guaranteed to commit successfully.
            newFieldNamesToBeInserted.push_back(
                StringMapHashedKey{hashedKey.key(), hashedKey.hash()});

            // Only update the bucket size once to account for the new field name if it
            // isn't already pending a commit from another batch.
            if (!bucket.uncommittedFieldNames.contains(hashedKey)) {
                // Add the size of an empty object with that field name.
                sizesToBeAdded.uncommittedVerifiedSize += emptyObjSize + fieldName.size();

                // The control.min and control.max summaries don't have any information for
                // this new field name yet. Add two measurements worth of data to account
                // for this. As this is the first measurement for this field, min == max.
                sizesToBeAdded.uncommittedVerifiedSize += elem.size() * 2;
            }
        }

        // The element size, taking into account that the name will be changed to its positional
        // number. Add 1 to the calculation since the element's field name size accounts for a null
        // terminator whereas the stringified position does not.
        const int32_t elementSize =
            elem.size() - elem.fieldNameSize() + numMeasurementsFieldLength + 1;

        if (elementSize > largeMeasurementThreshold) {
            sizesToBeAdded.uncommittedMeasurementEstimate += elementSize;
        }
    }
}

std::shared_ptr<WriteBatch> activeBatch(TrackingContexts& trackingContexts,
                                        Bucket& bucket,
                                        OperationId opId,
                                        std::uint8_t stripe,
                                        ExecutionStatsController& stats) {
    auto it = bucket.batches.find(opId);
    if (it == bucket.batches.end()) {
        it = bucket.batches
                 .try_emplace(opId,
                              std::make_shared<WriteBatch>(
                                  trackingContexts,
                                  bucket.bucketId,
                                  bucket.key,
                                  opId,
                                  stats,
                                  StringData{bucket.timeField.data(), bucket.timeField.size()}))
                 .first;
    }
    return it->second;
}

}  // namespace mongo::timeseries::bucket_catalog
