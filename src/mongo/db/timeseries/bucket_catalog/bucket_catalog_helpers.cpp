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

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries::bucket_catalog {

namespace {

StatusWith<std::pair<const BSONObj, const BSONObj>> extractMinAndMax(const BSONObj& bucketDoc) {
    const BSONObj& controlObj = bucketDoc.getObjectField(kBucketControlFieldName);
    if (controlObj.isEmpty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "The control field is empty or not an object: "
                              << redact(bucketDoc)};
    }

    const BSONObj& minObj = controlObj.getObjectField(kBucketControlMinFieldName);
    const BSONObj& maxObj = controlObj.getObjectField(kBucketControlMaxFieldName);
    if (minObj.isEmpty() || maxObj.isEmpty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "The control min and/or max fields are empty or not objects: "
                              << redact(bucketDoc)};
    }

    return std::make_pair(minObj, maxObj);
}

/**
 * Generates a match filter used to identify suitable buckets for reopening, represented by:
 *
 * {$and:
 *       {$or: [{"control.closed":{$exists:false}},
 *              {"control.closed":false}]
 *       },
 *       {"meta":<metaValue>},
 *       {$and: [{"control.min.time":{$lte:<measurementTs>}},
 *               {"control.min.time":{$gt:<measurementTs - maxSpanSeconds>}}]
 *       },
         {"data.<timeField>.<timeseriesBucketMaxCount - 1>":{$exists:false}}]
 * }
 */
BSONObj generateReopeningMatchFilter(const Date_t& time,
                                     boost::optional<BSONElement> metadata,
                                     const std::string& controlMinTimePath,
                                     const std::string& maxDataTimeFieldPath,
                                     int64_t bucketMaxSpanSeconds) {
    // The bucket cannot be closed (aka open for new measurements).
    auto closedFlagFilter =
        BSON("$or" << BSON_ARRAY(BSON(kControlClosedPath << BSON("$exists" << false))
                                 << BSON(kControlClosedPath << false)));

    // The measurement meta field must match the bucket 'meta' field. If the field is not specified
    // we can only insert into buckets which also do not have a meta field.
    BSONObj metaFieldFilter;
    if (metadata && (*metadata).ok()) {
        BSONObjBuilder builder;
        builder.appendAs(*metadata, kBucketMetaFieldName);
        metaFieldFilter = builder.obj();
    } else {
        metaFieldFilter = BSON(kBucketMetaFieldName << BSON("$exists" << false));
    }

    // (minimumTs <= measurementTs) && (minimumTs + maxSpanSeconds > measurementTs)
    auto measurementMaxDifference = time - Seconds(bucketMaxSpanSeconds);
    auto lowerBound = BSON(controlMinTimePath << BSON("$lte" << time));
    auto upperBound = BSON(controlMinTimePath << BSON("$gt" << measurementMaxDifference));
    auto timeRangeFilter = BSON("$and" << BSON_ARRAY(lowerBound << upperBound));

    // If the "data.<timeField>.<timeseriesBucketMaxCount - 1>" field exists, it means the bucket is
    // full and we do not want to insert future measurements into it.
    auto measurementSizeFilter = BSON(maxDataTimeFieldPath << BSON("$exists" << false));

    return BSON("$and" << BSON_ARRAY(closedFlagFilter << timeRangeFilter << metaFieldFilter
                                                      << measurementSizeFilter));
}

}  // namespace

std::vector<BSONObj> generateReopeningPipeline(const Date_t& time,
                                               boost::optional<BSONElement> metadata,
                                               const std::string& controlMinTimePath,
                                               const std::string& maxDataTimeFieldPath,
                                               int64_t bucketMaxSpanSeconds,
                                               int32_t bucketMaxSize) {
    std::vector<BSONObj> pipeline;

    // Stage 1: Match stage with suitable bucket requirements.
    pipeline.push_back(
        BSON("$match" << generateReopeningMatchFilter(
                 time, metadata, controlMinTimePath, maxDataTimeFieldPath, bucketMaxSpanSeconds)));

    // Stage 2: Add an observable field for the bucket document size.
    pipeline.push_back(BSON("$set" << BSON("object_size" << BSON("$bsonSize" << "$$ROOT"))));

    // Stage 3: Restrict bucket documents exceeding the max bucket size.
    pipeline.push_back(BSON("$match" << BSON("object_size" << BSON("$lt" << bucketMaxSize))));

    // Stage 4: Unset the document size field.
    pipeline.push_back(BSON("$unset" << "object_size"));

    // Stage 5: Restrict the aggregation to one document.
    pipeline.push_back(BSON("$limit" << 1));

    return pipeline;
}

StatusWith<MinMax> generateMinMaxFromBucketDoc(tracking::Context& trackingContext,
                                               const BSONObj& bucketDoc,
                                               const StringDataComparator* comparator) {
    auto swDocs = extractMinAndMax(bucketDoc);
    if (!swDocs.isOK()) {
        return swDocs.getStatus();
    }

    const auto& [minObj, maxObj] = swDocs.getValue();

    try {
        return MinMax::parseFromBSON(trackingContext, minObj, maxObj, comparator);
    } catch (...) {
        return exceptionToStatus();
    }
}

StatusWith<Schema> generateSchemaFromBucketDoc(tracking::Context& trackingContext,
                                               const BSONObj& bucketDoc,
                                               const StringDataComparator* comparator) {
    auto swDocs = extractMinAndMax(bucketDoc);
    if (!swDocs.isOK()) {
        return swDocs.getStatus();
    }

    const auto& [minObj, maxObj] = swDocs.getValue();

    try {
        return Schema::parseFromBSON(trackingContext, minObj, maxObj, comparator);
    } catch (...) {
        return exceptionToStatus();
    }
}

StatusWith<Date_t> extractTime(const BSONObj& doc, StringData timeFieldName) {
    auto timeElem = doc[timeFieldName];
    if (!timeElem || BSONType::date != timeElem.type()) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << timeFieldName << "' must be present and contain a "
                              << "valid BSON UTC datetime value"};
    }
    return timeElem.Date();
}

BSONObj buildControlMinTimestampDoc(StringData timeField, Date_t roundedTime) {
    BSONObjBuilder builder;
    builder.append(timeField, roundedTime);
    return builder.obj();
}

StatusWith<std::pair<Date_t, BSONElement>> extractTimeAndMeta(const BSONObj& doc,
                                                              StringData timeFieldName,
                                                              StringData metaFieldName) {
    // Iterate the document once, checking for both fields.
    BSONElement timeElem;
    BSONElement metaElem;
    for (auto&& el : doc) {
        if (!timeElem && el.fieldNameStringData() == timeFieldName) {
            timeElem = el;
            if (metaElem) {
                break;
            }
        } else if (!metaElem && el.fieldNameStringData() == metaFieldName) {
            metaElem = el;
            if (timeElem) {
                break;
            }
        }
    }

    if (!timeElem || BSONType::date != timeElem.type()) {
        return {ErrorCodes::BadValue,
                str::stream() << "'" << timeFieldName << "' must be present and contain a "
                              << "valid BSON UTC datetime value"};
    }
    auto time = timeElem.Date();

    return std::make_pair(time, metaElem);
}

void handleDirectWrite(RecoveryUnit& ru,
                       BucketCatalog& bucketCatalog,
                       const TimeseriesOptions& options,
                       const UUID& collectionUUID,
                       const BSONObj& bucket) {
    const BucketId bucketId = extractBucketId(bucketCatalog, options, collectionUUID, bucket);

    // First notify the BucketCatalog that we intend to start a direct write, so we can conflict
    // with any already-prepared operation, and also block bucket reopening if it's enabled.
    directWriteStart(bucketCatalog.bucketStateRegistry, bucketId);

    // Then register callbacks so we can let the BucketCatalog know that we are done with our direct
    // write after the actual write takes place (or is abandoned), and allow reopening.
    ru.onCommit([&bucketCatalog, bucketId](OperationContext*, boost::optional<Timestamp>) {
        directWriteFinish(bucketCatalog.bucketStateRegistry, bucketId);
    });
    ru.onRollback([&bucketCatalog, bucketId](OperationContext*) {
        directWriteFinish(bucketCatalog.bucketStateRegistry, bucketId);
    });
}

}  // namespace mongo::timeseries::bucket_catalog
