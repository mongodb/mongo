/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <algorithm>
#include <set>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"

namespace mongo {
/**
 * Carries parameters for unpacking a bucket.
 */
struct BucketSpec {
    // The user-supplied timestamp field name specified during time-series collection creation.
    std::string timeField;

    // An optional user-supplied metadata field name specified during time-series collection
    // creation. This field name is used during materialization of metadata fields of a measurement
    // after unpacking.
    boost::optional<std::string> metaField;

    // The set of field names in the data region that should be included or excluded.
    std::set<std::string> fieldSet;

    // Vector of computed meta field projection names. Added at the end of materialized
    // measurements.
    std::vector<std::string> computedMetaProjFields;
};

/**
 * BucketUnpacker will unpack bucket fields for metadata and the provided fields.
 */
class BucketUnpacker {
public:
    // A table that is useful for interpolations between the number of measurements in a bucket and
    // the byte size of a bucket's data section timestamp column. Each table entry is a pair (b_i,
    // S_i), where b_i is the number of measurements in the bucket and S_i is the byte size of the
    // timestamp BSONObj. The table is bounded by 16 MB (2 << 23 bytes) where the table entries are
    // pairs of b_i and S_i for the lower bounds of the row key digit intervals [0, 9], [10, 99],
    // [100, 999], [1000, 9999] and so on. The last entry in the table, S7, is the first entry to
    // exceed the server BSON object limit of 16 MB.
    static constexpr std::array<std::pair<int32_t, int32_t>, 8> kTimestampObjSizeTable{
        {{0, BSONObj::kMinBSONLength},
         {10, 115},
         {100, 1195},
         {1000, 12895},
         {10000, 138895},
         {100000, 1488895},
         {1000000, 15888895},
         {10000000, 168888895}}};

    /**
     * Given the size of a BSONObj timestamp column, formatted as it would be in a time-series
     * system.buckets.X collection, returns the number of measurements in the bucket in O(1) time.
     */
    static int computeMeasurementCount(int targetTimestampObjSize);

    // Set of field names reserved for time-series buckets.
    static const std::set<StringData> reservedBucketFieldNames;

    // When BucketUnpacker is created with kInclude it must produce measurements that contain the
    // set of fields. Otherwise, if the kExclude option is used, the measurements will include the
    // set difference between all fields in the bucket and the provided fields.
    enum class Behavior { kInclude, kExclude };

    BucketUnpacker(BucketSpec spec, Behavior unpackerBehavior);

    /**
     * This method will continue to materialize Documents until the bucket is exhausted. A
     * precondition of this method is that 'hasNext()' must be true.
     */
    Document getNext();

    /**
     * This method will extract the j-th measurement from the bucket. A precondition of this method
     * is that j >= 0 && j <= the number of measurements within the underlying bucket.
     */
    Document extractSingleMeasurement(int j);

    bool hasNext() const {
        return _timeFieldIter && _timeFieldIter->more();
    }

    /**
     * This resets the unpacker to prepare to unpack a new bucket described by the given document.
     */
    void reset(BSONObj&& bucket);

    Behavior behavior() const {
        return _unpackerBehavior;
    }

    const BucketSpec& bucketSpec() const {
        return _spec;
    }

    const BSONObj& bucket() const {
        return _bucket;
    }

    bool includeMetaField() const {
        return _includeMetaField;
    }

    bool includeTimeField() const {
        return _includeTimeField;
    }

    int32_t numberOfMeasurements() const {
        return _numberOfMeasurements;
    }

    void setBucketSpecAndBehavior(BucketSpec&& bucketSpec, Behavior behavior);

    // Add computed meta projection names to the bucket specification.
    void addComputedMetaProjFields(const std::vector<StringData>& computedFieldNames);

private:
    BucketSpec _spec;
    Behavior _unpackerBehavior;

    // Iterates the timestamp section of the bucket to drive the unpacking iteration.
    boost::optional<BSONObjIterator> _timeFieldIter;

    // A flag used to mark that the timestamp value should be materialized in measurements.
    bool _includeTimeField;

    // A flag used to mark that a bucket's metadata value should be materialized in measurements.
    bool _includeMetaField;

    // The bucket being unpacked.
    BSONObj _bucket;

    // Since the metadata value is the same across all materialized measurements we can cache the
    // metadata BSONElement in the reset phase and use it to materialize the metadata in each
    // measurement.
    BSONElement _metaValue;

    // Iterators used to unpack the columns of the above bucket that are populated during the reset
    // phase according to the provided 'Behavior' and 'BucketSpec'.
    std::vector<std::pair<std::string, BSONObjIterator>> _fieldIters;

    // Map <name, BSONElement> for the computed meta field projections. Updated for
    // every bucket upon reset().
    stdx::unordered_map<std::string, BSONElement> _computedMetaProjections;

    // The number of measurements in the bucket.
    int32_t _numberOfMeasurements = 0;
};

/**
 * Removes metaField from the field set and returns a boolean indicating whether metaField should be
 * included in the materialized measurements. Always returns false if metaField does not exist.
 */
inline bool eraseMetaFromFieldSetAndDetermineIncludeMeta(BucketUnpacker::Behavior unpackerBehavior,
                                                         BucketSpec* bucketSpec) {
    if (!bucketSpec->metaField ||
        std::find(bucketSpec->computedMetaProjFields.cbegin(),
                  bucketSpec->computedMetaProjFields.cend(),
                  *bucketSpec->metaField) != bucketSpec->computedMetaProjFields.cend()) {
        return false;
    } else if (auto itr = bucketSpec->fieldSet.find(*bucketSpec->metaField);
               itr != bucketSpec->fieldSet.end()) {
        bucketSpec->fieldSet.erase(itr);
        return unpackerBehavior == BucketUnpacker::Behavior::kInclude;
    } else {
        return unpackerBehavior == BucketUnpacker::Behavior::kExclude;
    }
}

/**
 * Determines if timestamp values should be included in the materialized measurements.
 */
inline bool determineIncludeTimeField(BucketUnpacker::Behavior unpackerBehavior,
                                      BucketSpec* bucketSpec) {
    return (unpackerBehavior == BucketUnpacker::Behavior::kInclude) ==
        (bucketSpec->fieldSet.find(bucketSpec->timeField) != bucketSpec->fieldSet.end());
}

/**
 * Determines if an arbitrary field should be included in the materialized measurements.
 */
inline bool determineIncludeField(StringData fieldName,
                                  BucketUnpacker::Behavior unpackerBehavior,
                                  const BucketSpec& bucketSpec) {
    return (unpackerBehavior == BucketUnpacker::Behavior::kInclude) ==
        (bucketSpec.fieldSet.find(fieldName.toString()) != bucketSpec.fieldSet.end());
}
}  // namespace mongo
