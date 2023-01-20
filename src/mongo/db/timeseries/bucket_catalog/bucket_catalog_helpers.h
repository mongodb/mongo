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
#include "mongo/base/string_data_comparator_interface.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/timeseries_options.h"

namespace mongo::timeseries::bucket_catalog {

/**
 * Generates and returns a MinMax object from an existing bucket document. Avoids unpacking the
 * bucket document and relies on the control.min and control.max summary fields.
 *
 * Returns a bad status if the bucket document is malformed.
 */
StatusWith<MinMax> generateMinMaxFromBucketDoc(const BSONObj& bucketDoc,
                                               const StringData::ComparatorInterface* comparator);

/**
 * Generates and returns a Schema object from an existing bucket document. Avoids unpacking the
 * bucket document and relies on the control.min and control.max summary fields.
 *
 * Returns a bad status if the bucket document is malformed or contains mixed schema measurements.
 */
StatusWith<Schema> generateSchemaFromBucketDoc(const BSONObj& bucketDoc,
                                               const StringData::ComparatorInterface* comparator);

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
 * Retrieves a document from the record store based off of the bucket ID.
 */
BSONObj findDocFromOID(OperationContext* opCtx, const Collection* coll, const OID& bucketId);

/**
 * Normalize metaField value (i.e. sort object keys) for a time-series measurement so that we can
 * more effectively match a measurement to an existing bucket. If 'as' is specified, the normalized
 * value will be added to 'builder' with the specified field name; otherwise it will be added with
 * its original field name.
 */
void normalizeMetadata(BSONObjBuilder* builder,
                       const BSONElement& elem,
                       boost::optional<StringData> as);

/**
 * Generates filters that can be used to run a 'find' query on the timeseries bucket collection to
 * find a bucket eligible to receive a new measurement specified by a document's metadata and
 * timestamp (measurementTs).
 *
 * A bucket is deemed suitable for the new measurement iff:
 * i.   the bucket is uncompressed and not closed
 * ii.  the meta fields match
 * iii. the measurementTs is within the allowed time span for the bucket
 *
 * {$and:
 *       [{"control.version":1},
 *       {$or: [{"control.closed":{$exists:false}},
 *              {"control.closed":false}]
 *       },
 *       {"meta":<metaValue>},
 *       {$and: [{"control.min.time":{$lte:<measurementTs>}},
 *               {"control.min.time":{$gt:<measurementTs - maxSpanSeconds>}}]
 *       }]
 * }
 */
BSONObj generateReopeningFilters(const Date_t& time,
                                 boost::optional<BSONElement> metadata,
                                 const std::string& controlMinTimePath,
                                 int64_t bucketMaxSpanSeconds);

/**
 * Notify the BucketCatalog of a direct write to a given bucket document.
 *
 * To be called from an OpObserver, e.g. in aboutToDelete and onUpdate.
 */
void handleDirectWrite(OperationContext* opCtx, const NamespaceString& ns, const OID& bucketId);

}  // namespace mongo::timeseries::bucket_catalog
