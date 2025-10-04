/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

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
StatusWith<Date_t> extractTime(const BSONObj& doc, StringData timeFieldName);

/**
 * Extracts the time field of a measurement document and its meta field.
 *
 * Returns a bad status if the document is malformed.
 */
StatusWith<std::pair<Date_t, BSONElement>> extractTimeAndMeta(const BSONObj& doc,
                                                              StringData timeFieldName,
                                                              StringData metaFieldName);

/**
 * Constructs a singleton BSONObj with the minimum timestamp.
 */
BSONObj buildControlMinTimestampDoc(StringData timeField, Date_t roundedTime);

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
