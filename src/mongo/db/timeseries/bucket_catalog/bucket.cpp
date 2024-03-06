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

#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/util/assert_util_core.h"

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

Bucket::Bucket(TrackingContext& trackingContext,
               const BucketId& bId,
               BucketKey k,
               StringData tf,
               Date_t mt,
               BucketStateRegistry& bsr)
    : bucketId(bId),
      key(std::move(k)),
      timeField(make_tracked_string(trackingContext, tf.data(), tf.size())),
      minTime(mt),
      bucketStateRegistry(bsr),
      lastChecked(getCurrentEraAndIncrementBucketCount(bucketStateRegistry)),
      fieldNames(makeTrackedStringSet(trackingContext)),
      uncommittedFieldNames(makeTrackedStringSet(trackingContext)),
      minmax(trackingContext),
      schema(trackingContext),
      batches(
          make_tracked_unordered_map<OperationId, std::shared_ptr<WriteBatch>>(trackingContext)),
      usingAlwaysCompressedBuckets(feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
          serverGlobalParams.featureCompatibility.acquireFCVSnapshot())),
      movableState(trackingContext) {}

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

void calculateBucketFieldsAndSizeChange(TrackingContext& trackingContext,
                                        const Bucket& bucket,
                                        const BSONObj& doc,
                                        boost::optional<StringData> metaField,
                                        Bucket::NewFieldNames& newFieldNamesToBeInserted,
                                        int32_t& sizeToBeAdded) {
    // BSON size for an object with an empty object field where field name is empty string.
    // We can use this as an offset to know the size when we have real field names.
    static constexpr int emptyObjSize = 12;
    // Validate in debug builds that this size is correct
    dassert(emptyObjSize == BSON("" << BSONObj()).objsize());

    newFieldNamesToBeInserted.clear();
    sizeToBeAdded = 0;
    auto numMeasurementsFieldLength = numDigits(bucket.numMeasurements);
    for (const auto& elem : doc) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == metaField) {
            // Ignore the metadata field since it will not be inserted.
            continue;
        }

        auto hashedKey = TrackedStringSet::hasher().hashed_key(trackingContext, fieldName);
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
                sizeToBeAdded += emptyObjSize + fieldName.size();

                // The control.min and control.max summaries don't have any information for
                // this new field name yet. Add two measurements worth of data to account
                // for this. As this is the first measurement for this field, min == max.
                sizeToBeAdded += elem.size() * 2;
            }
        }

        // Add the element size, taking into account that the name will be changed to its
        // positional number. Add 1 to the calculation since the element's field name size
        // accounts for a null terminator whereas the stringified position does not.
        sizeToBeAdded += elem.size() - elem.fieldNameSize() + numMeasurementsFieldLength + 1;
    }
}

std::shared_ptr<WriteBatch> activeBatch(TrackingContext& trackingContext,
                                        Bucket& bucket,
                                        OperationId opId,
                                        std::uint8_t stripe,
                                        ExecutionStatsController& stats) {
    auto it = bucket.batches.find(opId);
    if (it == bucket.batches.end()) {
        it = bucket.batches
                 .try_emplace(opId,
                              std::make_shared<WriteBatch>(
                                  trackingContext,
                                  BucketHandle{bucket.bucketId, stripe},
                                  bucket.key.cloneAsUntracked(),
                                  opId,
                                  stats,
                                  StringData{bucket.timeField.data(), bucket.timeField.size()}))
                 .first;
    }
    return it->second;
}

}  // namespace mongo::timeseries::bucket_catalog
