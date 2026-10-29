// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::timeseries::bucket_catalog {

/**
 * Generates and returns a MinMax object from an existing bucket document. Avoids unpacking the
 * bucket document and relies on the control.min and control.max summary fields.
 *
 * Returns a bad status if the bucket document is malformed.
 */
StatusWith<MinMax> generateMinMaxFromBucketDoc(tracking::Context&,
                                               const BSONObj& bucketDoc,
                                               const StringDataComparator* comparator);

/**
 * Generates and returns a Schema object from an existing bucket document. Avoids unpacking the
 * bucket document and relies on the control.min and control.max summary fields.
 *
 * Returns a bad status if the bucket document is malformed or contains mixed schema measurements.
 */
StatusWith<Schema> generateSchemaFromBucketDoc(tracking::Context&,
                                               const BSONObj& bucketDoc,
                                               const StringDataComparator* comparator);

/**
 * Extracts the time field of a measurement document.
 *
 * Returns a bad status if the document is malformed.
 */
StatusWith<Date_t> extractTime(const BSONObj& doc, std::string_view timeFieldName);

/**
 * Extracts the time field of a measurement document and its meta field.
 *
 * Returns a bad status if the document is malformed.
 */
StatusWith<std::pair<Date_t, BSONElement>> extractTimeAndMeta(const BSONObj& doc,
                                                              std::string_view timeFieldName,
                                                              std::string_view metaFieldName);

/**
 * Constructs a singleton BSONObj with the minimum timestamp.
 */
BSONObj buildControlMinTimestampDoc(std::string_view timeField, Date_t roundedTime);

/**
 * Generates an aggregation pipeline to identify a bucket eligible to receive a new measurement
 * specified by a document's metadata and timestamp (measurementTs).
 *
 * A bucket is deemed suitable for the new measurement iff:
 * i.   the bucket is uncompressed and not closed
 * ii.  the meta fields match
 * iii. the measurementTs is within the allowed time span for the bucket
 * iv.  the bucket has less than the max number of measurements and is below the max bucket size
 */
std::vector<BSONObj> generateReopeningPipeline(const Date_t& time,
                                               boost::optional<BSONElement> metadata,
                                               const std::string& controlMinTimePath,
                                               const std::string& maxDataTimeFieldPath,
                                               int64_t bucketMaxSpanSeconds,
                                               int32_t bucketMaxSize);

/**
 * Notify the BucketCatalog of a direct write to a given bucket document.
 *
 * To be called from an OpObserver, e.g. in onDelete and onUpdate.
 */
void handleDirectWrite(RecoveryUnit&,
                       BucketCatalog&,
                       const TimeseriesOptions& options,
                       const UUID& collectionUUID,
                       const BSONObj& bucket);
}  // namespace mongo::timeseries::bucket_catalog
