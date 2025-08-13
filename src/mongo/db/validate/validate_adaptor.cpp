/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/db/validate/validate_adaptor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_impl.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/throttle_cursor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/validate/index_consistency.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

#include <climits>
#include <memory>
#include <stdexcept>
#include <string>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(failRecordStoreTraversal);

// Set limit for size of corrupted records that will be reported.
const long long kMaxErrorSizeBytes = 1 * 1024 * 1024;
const long long kInterruptIntervalNumBytes = 50 * 1024 * 1024;  // 50MB.

static constexpr const char* kSchemaValidationFailedReason =
    "Detected one or more documents not compliant with the collection's schema. Check logs for log "
    "id 5363500.";
static constexpr const char* kTimeseriesValidationInconsistencyReason =
    "Detected one or more documents in this collection incompatible with time-series "
    "specifications. For more info, see logs with log id 6698300.";
static constexpr const char* kBSONValidationNonConformantReason =
    "Detected one or more documents in this collection not conformant to BSON specifications. For "
    "more info, see logs with log id 6825900";
static constexpr const char* kTimeseriesBucketingParametersChangedInconsistencyReason =
    "A time series bucketing parameter was changed in this collection but "
    "timeseriesBucketingParametersChanged is not true. For more info, see logs with log id "
    "9175400.";
static constexpr char kMalformedMinMaxTimeseriesBucket[] =
    "Detected a time-series bucket with malformed min/max values";
static constexpr char kExpectedMixedSchemaTimeseriesWarning[] =
    "Detected a time-series bucket with mixed schema data";
static constexpr char kUnexpectedMixedSchemaTimeseriesError[] =
    "Detected a time-series bucket with mixed schema data when "
    "timeseriesBucketsMayHaveMixedSchemaData is false. You can run the collMod command to set this "
    "flag";
static constexpr char kOutOfOrderDocumentError[] = "Detected out-of-order documents. See logs.";
static constexpr char kInvalidDocumentError[] = "Detected one or more invalid documents. See logs.";
static constexpr char kNotEnoughSpaceToReportCorruptionWarning[] =
    "Not all corrupted records are listed due to size limitations.";

/**
 * Validate that for each record in a clustered RecordStore the record key (RecordId) matches the
 * document's cluster key in the record value.
 */
void _validateClusteredCollectionRecordId(OperationContext* opCtx,
                                          const RecordId& rid,
                                          const BSONObj& doc,
                                          const ClusteredIndexSpec& indexSpec,
                                          const CollatorInterface* collator,
                                          ValidateResults* results) {
    const auto ridFromDoc = record_id_helpers::keyForDoc(doc, indexSpec, collator);
    if (!ridFromDoc.isOK()) {
        results->addError(str::stream() << rid << " " << ridFromDoc.getStatus().reason());
        results->addCorruptRecord(rid);
        return;
    }

    const auto ksFromBSON =
        key_string::Builder(key_string::Version::kLatestVersion, ridFromDoc.getValue());
    const auto ksFromRid = key_string::Builder(key_string::Version::kLatestVersion, rid);

    const auto clusterKeyField = clustered_util::getClusterKeyFieldName(indexSpec);
    if (ksFromRid != ksFromBSON) {
        results->addError(str::stream()
                          << "Document with " << rid << " has mismatched " << doc[clusterKeyField]
                          << " (RecordId KeyString='" << ksFromRid.toString()
                          << "', cluster key KeyString='" << ksFromBSON.toString() << "')");
        results->addCorruptRecord(rid);
    }
}

// Checks that 'control.count' matches the actual number of measurements in a closed bucket.
Status _validateTimeseriesCount(const BSONObj& control,
                                int bucketCount,
                                int version,
                                bool shouldDecompressBSON) {
    // Skips the check if a bucket is compressed, but we are not in a validate mode that will
    // decompress the bucket to actually go through the measurements.
    if (version == timeseries::kTimeseriesControlUncompressedVersion || !shouldDecompressBSON) {
        return Status::OK();
    }
    long long controlCount;
    if (Status status = bsonExtractIntegerField(
            control, timeseries::kBucketControlCountFieldName, &controlCount);
        !status.isOK()) {
        return status;
    }
    if (controlCount != bucketCount) {
        return Status(ErrorCodes::BadValue,
                      fmt::format("The 'control.count' field ({}) does not match the actual number "
                                  "of measurements in the document ({}).",
                                  controlCount,
                                  bucketCount));
    }
    return Status::OK();
}

// Checks if the embedded timestamp in the bucket id field matches that in the 'control.min' field.
Status _validateTimeSeriesIdTimestamp(OperationContext* opCtx,
                                      const CollectionPtr& collection,
                                      const BSONObj& recordBson) {
    // Compares both timestamps as Dates.
    auto minTimestamp = recordBson.getField(timeseries::kBucketControlFieldName)
                            .Obj()
                            .getField(timeseries::kBucketControlMinFieldName)
                            .Obj()
                            .getField(collection->getTimeseriesOptions()->getTimeField())
                            .Date();

    auto oidEmbeddedTimestamp = recordBson.getField(timeseries::kBucketIdFieldName).OID().asDateT();

    // If this collection has extended-range measurements, we cannot assert that the
    // minTimestamp matches the embedded timestamp.
    if (minTimestamp != oidEmbeddedTimestamp &&
        !timeseries::dateOutsideStandardRange(minTimestamp)) {
        return Status(ErrorCodes::InvalidIdField,
                      fmt::format("Mismatch between the embedded timestamp {} in the time-series "
                                  "bucket '_id' field and the timestamp {} in 'control.min' field.",
                                  oidEmbeddedTimestamp.toString(),
                                  minTimestamp.toString()));
    }
    return Status::OK();
}

/**
 * Checks the bucket's 'control' field to make sure the version is valid and the min max timestamps
 * respect the bucket max span.
 */
Status _validateTimeseriesControlField(const CollectionPtr& collection,
                                       const BSONObj& controlField) {
    int bucketVersion = controlField.getIntField(timeseries::kBucketControlVersionFieldName);
    if (bucketVersion != timeseries::kTimeseriesControlUncompressedVersion &&
        bucketVersion != timeseries::kTimeseriesControlCompressedSortedVersion &&
        bucketVersion != timeseries::kTimeseriesControlCompressedUnsortedVersion) {
        return Status(
            ErrorCodes::BadValue,
            fmt::format("Invalid value for 'control.version'. Expected 1, 2, or 3, but got {}.",
                        bucketVersion));
    }

    const auto& timeseriesOptions = collection->getTimeseriesOptions();
    const auto& minTimestamp = controlField.getField(timeseries::kBucketControlMinFieldName)
                                   .Obj()
                                   .getField(timeseriesOptions->getTimeField())
                                   .Date();
    const auto& maxTimestamp = controlField.getField(timeseries::kBucketControlMaxFieldName)
                                   .Obj()
                                   .getField(timeseriesOptions->getTimeField())
                                   .Date();
    const auto& bucketMaxSpanSeconds = timeseriesOptions->getBucketMaxSpanSeconds();
    if (maxTimestamp - minTimestamp >= Seconds(*bucketMaxSpanSeconds)) {
        return Status(
            ErrorCodes::BadValue,
            fmt::format(
                "Bucket's timestamps in 'control.min' and 'control.max' fields do not respect the "
                "bucket max span. Min time: {}. Max time: {}. Bucket max span seconds: {}",
                minTimestamp.toString(),
                maxTimestamp.toString(),
                *bucketMaxSpanSeconds));
    }

    return Status::OK();
}

/**
 * Checks if the bucket's version matches the types of 'data' fields.
 */
Status _validateTimeseriesDataFieldTypes(const BSONElement& dataField, int bucketVersion) {
    auto dataType = (bucketVersion == timeseries::kTimeseriesControlUncompressedVersion)
        ? BSONType::object
        : BSONType::binData;
    // Checks that open buckets have 'Object' type and closed buckets have 'BinData Column' type.
    auto isCorrectType = [&](BSONElement el) {
        if (bucketVersion == timeseries::kTimeseriesControlUncompressedVersion) {
            return el.type() == BSONType::object;
        } else {
            return el.type() == BSONType::binData && el.binDataType() == BinDataType::Column;
        }
    };

    if (!isCorrectType(dataField)) {
        return Status(ErrorCodes::TypeMismatch,
                      fmt::format("Mismatch between time-series schema version and data field "
                                  "type. Expected type {}, but got {}.",
                                  mongo::typeName(dataType),
                                  mongo::typeName(dataField.type())));
    }
    return Status::OK();
}


/*
 * Checks that only buckets that have timeSeriesBucketingParameters flag set have changed
 * bucket parameters.
 */
Status _validateTimeseriesBucketingParametersChanged(const CollectionPtr& coll,
                                                     timeseries::bucket_catalog::MinMax& minmax,
                                                     const BSONElement& controlMin,
                                                     const BSONElement& controlMax,
                                                     StringData fieldName,
                                                     ValidateResults* results,
                                                     int version,
                                                     bool shouldDecompressBSON) {
    // Skips the check if a bucket is compressed, but we are not in a validate mode that will
    // decompress the bucket to actually go through the measurements.
    if ((version == timeseries::kTimeseriesControlCompressedSortedVersion ||
         version == timeseries::kTimeseriesControlCompressedUnsortedVersion) &&
        !shouldDecompressBSON) {
        return Status::OK();
    }

    bool timeseriesBucketingParametersHaveChanged =
        coll->timeseriesBucketingParametersHaveChanged().value_or(true);

    auto min = minmax.min();
    auto max = minmax.max();

    auto checkTimeSeriesBucketingParametersChanged = [&]() {
        const auto options = coll->getTimeseriesOptions().value();
        auto roundedDownTimeStamp =
            timeseries::roundTimestampToGranularity(min.getField(fieldName).date(), options);
        return roundedDownTimeStamp < controlMin.Date();
    };

    if (checkTimeSeriesBucketingParametersChanged() && !timeseriesBucketingParametersHaveChanged &&
        results->addError(kTimeseriesBucketingParametersChangedInconsistencyReason)) {
        // Get the original timeseries bucketing parameters.
        auto originalTimeseriesOptions = coll->getTimeseriesOptions();
        invariant(originalTimeseriesOptions != boost::none);
        const auto& options = originalTimeseriesOptions.get();
        auto originalBucketMaxSpanSeconds = options.getBucketMaxSpanSeconds();
        auto originalBucketRoundingSeconds = options.getBucketRoundingSeconds();
        auto originalGranularity = options.getGranularity();

        LOGV2_ERROR_OPTIONS(9175400,
                            {logv2::LogTruncation::Disabled},
                            "A time series bucketing parameter was changed",
                            logAttrs(coll->ns()),
                            "currentBucketMaxSpanSeconds"_attr = originalBucketMaxSpanSeconds,
                            "currentBucketRoundingSeconds"_attr = originalBucketRoundingSeconds,
                            "currentGranularity"_attr = originalGranularity);
    }

    return Status::OK();
}

/**
 * Checks whether the min and max values between 'control' and 'data' match, taking timestamp
 * granularity into account.
 */
Status _validateTimeSeriesMinMax(const CollectionPtr& coll,
                                 timeseries::bucket_catalog::MinMax& minmax,
                                 const BSONElement& controlMin,
                                 const BSONElement& controlMax,
                                 StringData fieldName,
                                 int version,
                                 bool shouldDecompressBSON) {
    // Skips the check if a bucket is compressed, but we are not in a validate mode that will
    // decompress the bucket to actually go through the measurements.
    if ((version == timeseries::kTimeseriesControlCompressedSortedVersion ||
         version == timeseries::kTimeseriesControlCompressedUnsortedVersion) &&
        !shouldDecompressBSON) {
        return Status::OK();
    }
    auto min = minmax.min();
    auto max = minmax.max();
    auto checkMinAndMaxMatch = [&]() {
        const auto options = coll->getTimeseriesOptions().value();
        if (fieldName == options.getTimeField()) {
            // With measurement-level deletes (deletes with non-metafield filters) it is possible
            // that the earliest measurements got deleted. Since we keep the bucket's minTime
            // unchanged in that case, we cannot rely on the minTime always corresponding with what
            // the actual minimum measurement time is. We can, however, rely on the fact that the
            // rounded time of the earliest measurement is at greater than or equal to the
            // control.min time-field.
            // TODO (SERVER-94872): Reinstate the strict equality check.
            auto minTimestampsMatch =
                timeseries::roundTimestampToGranularity(min.getField(fieldName).Date(), options) >=
                timeseries::roundTimestampToGranularity(controlMin.Date(), options);
            // For the maximum check, if we had measurements that were pre-1970 (the lower end of
            // the extended range check), it is possible that the control.max value gets rounded up
            // to the epoch and is greater than the observed maximum timestamp. In the case where
            // the control.min is earlier than the epoch, we should relax the check.
            auto maxTimestampsMatch = (min.getField(fieldName).Date() < Date_t())
                ? controlMax.Date() >= max.getField(fieldName).Date()
                : controlMax.Date() == max.getField(fieldName).Date();

            return minTimestampsMatch && maxTimestampsMatch;
        } else {
            // We cannot guarantee that the field order of the BSON objects will be the same.
            return controlMin.wrap().woCompare(min,
                                               /*ordering=*/BSONObj(),
                                               BSONObj::ComparisonRules::kConsiderFieldName |
                                                   BSONObj::ComparisonRules::kIgnoreFieldOrder) ==
                0 &&
                controlMax.wrap().woCompare(max,
                                            /*ordering=*/BSONObj(),
                                            BSONObj::ComparisonRules::kConsiderFieldName |
                                                BSONObj::ComparisonRules::kIgnoreFieldOrder) == 0;
        }
    };

    if (!checkMinAndMaxMatch()) {
        return Status(
            ErrorCodes::BadValue,
            fmt::format(
                "Mismatch between time-series control and observed min or max for field {}. "
                "Control had min {} and max {}, but observed data had min {} and max {}.",
                fieldName,
                controlMin.toString(),
                controlMax.toString(),
                min.toString(),
                max.toString()));
    }

    return Status::OK();
}

/**
 * Attempts to parse the field name to integer.
 */
int _idxInt(StringData idx) {
    try {
        auto idxInt = std::stoi(std::string{idx});
        return idxInt;
    } catch (const std::invalid_argument&) {
        return INT_MIN;
    }
}

/**
 * Validates the indexes of the time field in the data field of a bucket. Checks the min and max
 * values match the ones in 'control' field. Counts the number of measurements.
 */
Status _validateTimeSeriesDataTimeField(const CollectionPtr& coll,
                                        const BSONElement& timeField,
                                        const BSONElement& controlMin,
                                        const BSONElement& controlMax,
                                        StringData fieldName,
                                        ValidateResults* results,
                                        int version,
                                        int* bucketCount,
                                        bool shouldDecompressBSON) {
    tracking::Context trackingContext;
    timeseries::bucket_catalog::MinMax minmax{trackingContext};
    if (version == timeseries::kTimeseriesControlUncompressedVersion) {
        for (const auto& metric : timeField.Obj()) {
            if (metric.type() != BSONType::date) {
                return Status(ErrorCodes::BadValue,
                              fmt::format("Time-series bucket {} field is not a Date", fieldName));
            }
            // Checks that indices are consecutively increasing numbers starting from 0.
            if (auto idx = _idxInt(metric.fieldNameStringData()); idx != *bucketCount) {
                return Status(
                    ErrorCodes::BadValue,
                    fmt::format("The indexes in time-series bucket data field '{}' is "
                                "not consecutively increasing from '0'. Expected: {}, but got: {}",
                                fieldName,
                                *bucketCount,
                                idx));
            }
            minmax.update(metric.wrap(fieldName), boost::none, coll->getDefaultCollator());
            ++(*bucketCount);
        }
    } else if (shouldDecompressBSON) {
        // Only decompress the bucket if we are in full validation mode, kBackgroundCheckBSON mode,
        // or kForegroundCheckBSON mode since this is a relatively expensive operation.
        try {
            BSONColumn col{timeField};
            Date_t prevTimestamp = Date_t::min();
            bool detectedOutOfOrder = false;
            for (const auto& metric : col) {
                if (!metric.eoo()) {
                    if (metric.type() != BSONType::date) {
                        return Status(
                            ErrorCodes::BadValue,
                            fmt::format("Time-series bucket '{}' field is not a Date", fieldName));
                    }
                    // Checks the time values are sorted in increasing order for v2 buckets
                    // (compressed, sorted). Skip the check if the bucket is v3 (compressed,
                    // unsorted).
                    Date_t curTimestamp = metric.Date();
                    if (curTimestamp < prevTimestamp) {
                        if (version == timeseries::kTimeseriesControlCompressedSortedVersion) {
                            return Status(
                                ErrorCodes::BadValue,
                                fmt::format(
                                    "Time-series bucket '{}' field is not in ascending order",
                                    fieldName));
                        } else if (version ==
                                   timeseries::kTimeseriesControlCompressedUnsortedVersion) {
                            detectedOutOfOrder = true;
                        }
                    }
                    prevTimestamp = curTimestamp;
                    minmax.update(metric.wrap(fieldName), boost::none, coll->getDefaultCollator());
                    ++(*bucketCount);
                } else {
                    return Status(ErrorCodes::BadValue,
                                  "Time-series bucket has missing time fields");
                }
            }
            if (version == timeseries::kTimeseriesControlCompressedUnsortedVersion &&
                !detectedOutOfOrder) {
                return Status(ErrorCodes::BadValue,
                              "Time-series bucket is v3 but has its measurements in-order on time");
            }
        } catch (DBException& e) {
            return Status(ErrorCodes::InvalidBSON,
                          str::stream() << "Exception occurred while decompressing a BSON column: "
                                        << e.toString());
        }
    }
    if (Status status = _validateTimeSeriesMinMax(
            coll, minmax, controlMin, controlMax, fieldName, version, shouldDecompressBSON);
        !status.isOK()) {
        return status;
    }

    if (Status status = _validateTimeseriesBucketingParametersChanged(coll,
                                                                      minmax,
                                                                      controlMin,
                                                                      controlMax,
                                                                      fieldName,
                                                                      results,
                                                                      version,
                                                                      shouldDecompressBSON);
        !status.isOK()) {
        return status;
    }

    return Status::OK();
}

/**
 * Validates the indexes of the data measurement fields of a bucket. Checks the min and max values
 * match the ones in 'control' field.
 */
Status _validateTimeSeriesDataField(const CollectionPtr& coll,
                                    const BSONElement& dataField,
                                    const BSONElement& controlMin,
                                    const BSONElement& controlMax,
                                    StringData fieldName,
                                    ValidateResults* results,
                                    int version,
                                    int bucketCount,
                                    bool shouldDecompressBSON) {
    tracking::Context trackingContext;
    timeseries::bucket_catalog::MinMax minmax{trackingContext};
    if (version == timeseries::kTimeseriesControlUncompressedVersion) {
        // Checks that indices are in increasing order and within the correct range.
        int prevIdx = INT_MIN;
        for (const auto& metric : dataField.Obj()) {
            auto idx = _idxInt(metric.fieldNameStringData());
            if (idx <= prevIdx) {
                return Status(ErrorCodes::BadValue,
                              fmt::format("The index '{}' in time-series bucket data field '{}' is "
                                          "not in increasing order",
                                          metric.fieldNameStringData(),
                                          fieldName));
            }
            if (idx > bucketCount) {
                return Status(ErrorCodes::BadValue,
                              fmt::format("The index '{}' in time-series bucket data field '{}' is "
                                          "out of range",
                                          metric.fieldNameStringData(),
                                          fieldName));
            }
            if (idx < 0) {
                return Status(ErrorCodes::BadValue,
                              fmt::format("The index '{}' in time-series bucket data field '{}' is "
                                          "negative or non-numerical",
                                          metric.fieldNameStringData(),
                                          fieldName));
            }
            minmax.update(metric.wrap(fieldName), boost::none, coll->getDefaultCollator());
            prevIdx = idx;
        }
    } else if (shouldDecompressBSON) {
        // Only decompress the bucket if we are in full validation mode, kBackgroundCheckBSON mode,
        // or kForegroundCheckBSON mode since this is a relatively expensive operation.
        try {
            BSONColumn col{dataField};
            for (const auto& metric : col) {
                if (!metric.eoo()) {
                    minmax.update(metric.wrap(fieldName), boost::none, coll->getDefaultCollator());
                }
            }
        } catch (DBException& e) {
            return Status(ErrorCodes::InvalidBSON,
                          str::stream() << "Exception occurred while decompressing a BSON column: "
                                        << e.toString());
        }
    }

    if (Status status = _validateTimeSeriesMinMax(
            coll, minmax, controlMin, controlMax, fieldName, version, shouldDecompressBSON);
        !status.isOK()) {
        return status;
    }

    return Status::OK();
}

Status _validateTimeSeriesDataFields(const CollectionPtr& coll,
                                     const BSONObj& recordBson,
                                     ValidateResults* results,
                                     int bucketVersion,
                                     bool shouldDecompressBSON) {
    BSONObj data = recordBson.getField(timeseries::kBucketDataFieldName).Obj();
    BSONObj control = recordBson.getField(timeseries::kBucketControlFieldName).Obj();
    BSONObj controlMin = control.getField(timeseries::kBucketControlMinFieldName).Obj();
    BSONObj controlMax = control.getField(timeseries::kBucketControlMaxFieldName).Obj();

    // Builds a hash map for the fields to avoid repeated traversals.
    auto buildFieldTable = [&](StringMap<BSONElement>* table, const BSONObj& fields) {
        for (const auto& field : fields) {
            table->insert({std::string{field.fieldNameStringData()}, field});
        }
    };

    StringMap<BSONElement> dataFields;
    StringMap<BSONElement> controlMinFields;
    StringMap<BSONElement> controlMaxFields;
    buildFieldTable(&dataFields, data);
    buildFieldTable(&controlMinFields, controlMin);
    buildFieldTable(&controlMaxFields, controlMax);

    // Checks that the number of 'control.min' and 'control.max' fields agrees with number of 'data'
    // fields.
    if (dataFields.size() != controlMinFields.size() ||
        controlMinFields.size() != controlMaxFields.size()) {
        return Status(
            ErrorCodes::BadValue,
            fmt::format("Mismatch between the number of time-series control fields and the number "
                        "of data fields. Control had {} min fields and {} max fields, but observed "
                        "data had {} fields.",
                        controlMinFields.size(),
                        controlMaxFields.size(),
                        dataFields.size()));
    };

    // Validates the time field.
    int bucketCount = 0;
    auto timeFieldName = std::string{coll->getTimeseriesOptions().value().getTimeField()};
    if (Status status = _validateTimeseriesDataFieldTypes(dataFields[timeFieldName], bucketVersion);
        !status.isOK()) {
        return status;
    }

    if (Status status = _validateTimeSeriesDataTimeField(coll,
                                                         dataFields[timeFieldName],
                                                         controlMinFields[timeFieldName],
                                                         controlMaxFields[timeFieldName],
                                                         timeFieldName,
                                                         results,
                                                         bucketVersion,
                                                         &bucketCount,
                                                         shouldDecompressBSON);
        !status.isOK()) {
        return status;
    }

    if (Status status =
            _validateTimeseriesCount(control, bucketCount, bucketVersion, shouldDecompressBSON);
        !status.isOK()) {
        return status;
    }

    // Validates the other fields.
    for (const auto& [fieldName, dataField] : dataFields) {
        if (fieldName != timeFieldName) {
            if (Status status =
                    _validateTimeseriesDataFieldTypes(dataFields[fieldName], bucketVersion);
                !status.isOK()) {
                return status;
            }

            if (Status status = _validateTimeSeriesDataField(coll,
                                                             dataFields[fieldName],
                                                             controlMinFields[fieldName],
                                                             controlMaxFields[fieldName],
                                                             fieldName,
                                                             results,
                                                             bucketVersion,
                                                             bucketCount,
                                                             shouldDecompressBSON);
                !status.isOK()) {
                return status;
            }
        }
    }

    return Status::OK();
}

/**
 * Validates the consistency of a time-series bucket.
 */
Status _validateTimeSeriesBucketRecord(OperationContext* opCtx,
                                       const CollectionPtr& collection,
                                       const BSONObj& recordBson,
                                       ValidateResults* results,
                                       bool shouldDecompressBSON) {
    const auto& controlField = recordBson.getField(timeseries::kBucketControlFieldName).Obj();
    int bucketVersion = controlField.getIntField(timeseries::kBucketControlVersionFieldName);

    if (Status status = _validateTimeSeriesIdTimestamp(opCtx, collection, recordBson);
        !status.isOK()) {
        return status;
    }

    if (Status status = _validateTimeseriesControlField(collection, controlField); !status.isOK()) {
        return status;
    }

    if (Status status = _validateTimeSeriesDataFields(
            collection, recordBson, results, bucketVersion, shouldDecompressBSON);
        !status.isOK()) {
        return status;
    }

    return Status::OK();
}
}  // namespace

Status ValidateAdaptor::validateRecord(OperationContext* opCtx,
                                       const RecordId& recordId,
                                       const RecordData& record,
                                       long long* nNonCompliantDocuments,
                                       size_t* dataSize,
                                       ValidateResults* results,
                                       ValidationVersion validationVersion) {
    Status status = validateBSON(
        record.data(), record.size(), _validateState->getBSONValidateMode(), validationVersion);
    if (!status.isOK()) {
        if (status.code() != ErrorCodes::NonConformantBSON) {
            return status;
        }
        LOGV2_WARNING_OPTIONS(6825900,
                              {logv2::LogTruncation::Disabled},
                              "Document is not conformant to BSON specifications",
                              "recordId"_attr = recordId,
                              "reason"_attr = status);
        (*nNonCompliantDocuments)++;
        results->addWarning(kBSONValidationNonConformantReason);
    }

    BSONObj recordBson = record.toBson();
    *dataSize = recordBson.objsize();

    if (MONGO_unlikely(_validateState->logDiagnostics())) {
        LOGV2(4666601, "[validate]", "recordId"_attr = recordId, "recordData"_attr = recordBson);
    }

    const CollectionPtr& coll = _validateState->getCollection();
    if (coll->isClustered()) {
        _validateClusteredCollectionRecordId(opCtx,
                                             recordId,
                                             recordBson,
                                             coll->getClusteredInfo()->getIndexSpec(),
                                             coll->getDefaultCollator(),
                                             results);
    }

    SharedBufferFragmentBuilder pool(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);

    for (const auto& indexIdent : _validateState->getIndexIdents()) {
        const IndexDescriptor* descriptor =
            coll->getIndexCatalog()->findIndexByIdent(opCtx, indexIdent);
        if ((descriptor->isPartial() &&
             !exec::matcher::matchesBSON(descriptor->getEntry()->getFilterExpression(),
                                         recordBson)) ||
            !results->getIndexValidateResult(descriptor->indexName()).continueValidation()) {
            continue;
        }

        this->traverseRecord(opCtx, coll, descriptor->getEntry(), recordId, recordBson, results);
    }
    return Status::OK();
}

void ValidateAdaptor::traverseRecordStore(OperationContext* opCtx,
                                          ValidateResults* results,
                                          ValidationVersion validationVersion) {
    _numRecords = 0;  // need to reset it because this function can be called more than once.
    long long dataSizeTotal = 0;
    long long interruptIntervalNumBytes = 0;
    long long nInvalid = 0;
    long long nNonCompliantDocuments = 0;
    long long numCorruptRecordsSizeBytes = 0;

    ON_BLOCK_EXIT([&]() {
        results->setNumInvalidDocuments(nInvalid);
        results->setNumNonCompliantDocuments(nNonCompliantDocuments);
        results->setNumRecords(_numRecords);
        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            _progress.get(lk)->finished();
        }
    });

    RecordId prevRecordId;

    // In case validation occurs twice and the progress meter persists after index traversal
    if (_progress.get(WithLock::withoutLock()) &&
        _progress.get(WithLock::withoutLock())->isActive()) {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        _progress.get(lk)->finished();
    }

    // Because the progress meter is intended as an approximation, it's sufficient to get the number
    // of records when we begin traversing, even if this number may deviate from the final number.
    const auto& coll = _validateState->getCollection();
    const char* curopMessage = "Validate: scanning documents";
    const auto totalRecords = coll->getRecordStore()->numRecords();
    const auto rs = coll->getRecordStore();
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        _progress.set(lk, CurOp::get(opCtx)->setProgress(lk, curopMessage, totalRecords), opCtx);
    }

    if (_validateState->getFirstRecordId().isNull()) {
        // The record store is empty if the first RecordId isn't initialized.
        return;
    }

    const std::unique_ptr<SeekableRecordThrottleCursor>& traverseRecordStoreCursor =
        _validateState->getTraverseRecordStoreCursor();
    for (auto record =
             traverseRecordStoreCursor->seekExact(opCtx, _validateState->getFirstRecordId());
         record;
         record = traverseRecordStoreCursor->next(opCtx)) {
        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            _progress.get(lk)->hit();
        }
        ++_numRecords;
        auto dataSize = record->data.size();
        interruptIntervalNumBytes += dataSize;
        dataSizeTotal += dataSize;
        size_t validatedSize = 0;
        Status status = validateRecord(opCtx,
                                       record->id,
                                       record->data,
                                       &nNonCompliantDocuments,
                                       &validatedSize,
                                       results,
                                       validationVersion);

        // Log the out-of-order entries as errors.
        //
        // Validate uses a DataCorruptionDetectionMode::kLogAndContinue mode such that data
        // corruption errors are logged without throwing, so certain checks must be duplicated here
        // as well.
        if ((prevRecordId.isValid() && prevRecordId > record->id) ||
            MONGO_unlikely(failRecordStoreTraversal.shouldFail())) {
            results->addError(kOutOfOrderDocumentError);
        }

        // validatedSize = dataSize is not a general requirement as some storage engines may use
        // padding, but we still require that they return the unpadded record data.
        if (!status.isOK() || validatedSize != static_cast<size_t>(dataSize)) {
            // If status is not okay, dataSize is not reliable.
            if (!status.isOK()) {
                LOGV2_OPTIONS(4835001,
                              {logv2::LogTruncation::Disabled},
                              "Document corruption details - Document validation failed with error",
                              "recordId"_attr = record->id,
                              "error"_attr = status);
            } else {
                LOGV2_OPTIONS(
                    4835002,
                    {logv2::LogTruncation::Disabled},
                    "Document corruption details - Document validation failure; size mismatch",
                    "recordId"_attr = record->id,
                    "validatedBytes"_attr = validatedSize,
                    "recordBytes"_attr = dataSize);
            }

            if (_validateState->fixErrors()) {
                writeConflictRetry(opCtx, "corrupt record removal", _validateState->nss(), [&] {
                    WriteUnitOfWork wunit(opCtx);
                    rs->deleteRecord(
                        opCtx, *shard_role_details::getRecoveryUnit(opCtx), record->id);
                    wunit.commit();
                });
                results->setRepaired(true);
                results->addNumRemovedCorruptRecords(1);
                _numRecords--;
            } else {
                results->addError(kInvalidDocumentError);
                numCorruptRecordsSizeBytes += record->id.memUsage();
                if (numCorruptRecordsSizeBytes <= kMaxErrorSizeBytes) {
                    results->addCorruptRecord(record->id);
                } else {
                    results->addWarning(kNotEnoughSpaceToReportCorruptionWarning);
                }

                nInvalid++;
            }
        } else {
            // If the document is not corrupted, validate the document against this collection's
            // schema validator. Don't treat invalid documents as errors since documents can bypass
            // document validation when being inserted or updated.
            auto result = coll->checkValidation(opCtx, record->data.toBson());

            if (result.first != Collection::SchemaValidationResult::kPass) {
                LOGV2_WARNING_OPTIONS(5363500,
                                      {logv2::LogTruncation::Disabled},
                                      "Document is not compliant with the collection's schema",
                                      logAttrs(coll->ns()),
                                      "recordId"_attr = record->id,
                                      "reason"_attr = result.second);

                nNonCompliantDocuments++;
                results->addWarning(kSchemaValidationFailedReason);
            } else if (coll->getTimeseriesOptions()) {
                BSONObj recordBson = record->data.toBson();

                // Checks for time-series collection consistency.
                Status bucketStatus =
                    _validateTimeSeriesBucketRecord(opCtx,
                                                    coll,
                                                    recordBson,
                                                    results,
                                                    _validateState->isBSONConformanceValidation());
                // This log id should be kept in sync with the associated warning messages that are
                // returned to the client.
                if (!bucketStatus.isOK()) {
                    LOGV2_WARNING_OPTIONS(
                        6698300,
                        {logv2::LogTruncation::Disabled},
                        "Document is not compliant with time-series specifications",
                        logAttrs(coll->ns()),
                        "recordId"_attr = record->id,
                        "reason"_attr = bucketStatus);
                    nNonCompliantDocuments++;
                    results->addError(kTimeseriesValidationInconsistencyReason);
                }
                auto containsMixedSchemaDataResponse =
                    coll->doesTimeseriesBucketsDocContainMixedSchemaData(recordBson);
                if (!containsMixedSchemaDataResponse.isOK() &&
                    results->addError(kMalformedMinMaxTimeseriesBucket)) {
                    LOGV2_WARNING_OPTIONS(8469900,
                                          {logv2::LogTruncation::Disabled},
                                          kMalformedMinMaxTimeseriesBucket,
                                          logAttrs(coll->ns()),
                                          "bucketId"_attr = record->id,
                                          "error"_attr =
                                              containsMixedSchemaDataResponse.getStatus());
                } else if (containsMixedSchemaDataResponse.isOK() &&
                           containsMixedSchemaDataResponse.getValue()) {
                    bool mixedSchemaAllowed = coll->getTimeseriesMixedSchemaBucketsState()
                                                  .canStoreMixedSchemaBucketsSafely();
                    if (mixedSchemaAllowed &&
                        results->addWarning(kExpectedMixedSchemaTimeseriesWarning)) {
                        LOGV2_WARNING_OPTIONS(8469901,
                                              {logv2::LogTruncation::Disabled},
                                              kExpectedMixedSchemaTimeseriesWarning,
                                              logAttrs(coll->ns()),
                                              "bucketId"_attr = record->id);
                    } else if (!mixedSchemaAllowed &&
                               results->addError(kUnexpectedMixedSchemaTimeseriesError)) {
                        LOGV2_WARNING_OPTIONS(8469902,
                                              {logv2::LogTruncation::Disabled},
                                              kUnexpectedMixedSchemaTimeseriesError,
                                              logAttrs(coll->ns()),
                                              "bucketId"_attr = record->id);
                    }
                }
            }
        }

        prevRecordId = record->id;

        if (_numRecords % IndexConsistency::kInterruptIntervalNumRecords == 0 ||
            interruptIntervalNumBytes >= kInterruptIntervalNumBytes) {
            // Periodically checks for interrupts and yields.
            opCtx->checkForInterrupt();
            _validateState->yieldCursors(opCtx);

            if (interruptIntervalNumBytes >= kInterruptIntervalNumBytes) {
                interruptIntervalNumBytes = 0;
            }
        }
    }

    if (results->getNumRemovedCorruptRecords() > 0) {
        results->addWarning(str::stream() << "Removed " << results->getNumRemovedCorruptRecords()
                                          << " invalid documents.");
    }

    const auto fastCount = coll->numRecords(opCtx);
    if (_validateState->shouldEnforceFastCount() && fastCount != _numRecords) {
        results->addError(str::stream()
                          << "fast count (" << fastCount << ") does not match number of records ("
                          << _numRecords << ") for collection '" << coll->ns().toStringForErrorMsg()
                          << "'");
    }

    // Do not update the record store stats if we're in the background as we've validated a
    // checkpoint and it may not have the most up-to-date changes.
    if (results->isValid() && !_validateState->isBackground()) {
        coll->getRecordStore()->updateStatsAfterRepair(_numRecords, dataSizeTotal);
    }
}

void ValidateAdaptor::validateIndexKeyCount(OperationContext* opCtx,
                                            const IndexCatalogEntry* index,
                                            IndexValidateResults& results) {
    _keyBasedIndexConsistency.validateIndexKeyCount(opCtx, index, &_numRecords, results);
}

void ValidateAdaptor::traverseIndex(OperationContext* opCtx,
                                    const IndexCatalogEntry* index,
                                    int64_t* numTraversedKeys,
                                    ValidateResults* results) {
    // The progress meter will be inactive after traversing the record store to allow the message
    // and the total to be set to different values.
    if (!_progress.get(WithLock::withoutLock())->isActive()) {
        const char* curopMessage = "Validate: scanning index entries";
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        _progress.set(lk,
                      CurOp::get(opCtx)->setProgress(
                          lk, curopMessage, _keyBasedIndexConsistency.getTotalIndexKeys()),
                      opCtx);
    }

    int64_t numKeys = _keyBasedIndexConsistency.traverseIndex(opCtx, index, _progress, results);

    if (numTraversedKeys) {
        *numTraversedKeys = numKeys;
    }
}

void ValidateAdaptor::traverseRecord(OperationContext* opCtx,
                                     const CollectionPtr& coll,
                                     const IndexCatalogEntry* index,
                                     const RecordId& recordId,
                                     const BSONObj& record,
                                     ValidateResults* results) {
    _keyBasedIndexConsistency.traverseRecord(opCtx, coll, index, recordId, record, results);
}

void ValidateAdaptor::setSecondPhase() {
    _keyBasedIndexConsistency.setSecondPhase();
}

bool ValidateAdaptor::limitMemoryUsageForSecondPhase(ValidateResults* result) {
    return _keyBasedIndexConsistency.limitMemoryUsageForSecondPhase(result);
}

bool ValidateAdaptor::haveEntryMismatch() const {
    return _keyBasedIndexConsistency.haveEntryMismatch();
}

void ValidateAdaptor::repairIndexEntries(OperationContext* opCtx, ValidateResults* results) {
    _keyBasedIndexConsistency.repairIndexEntries(opCtx, results);
}

void ValidateAdaptor::addIndexEntryErrors(OperationContext* opCtx, ValidateResults* results) {
    _keyBasedIndexConsistency.addIndexEntryErrors(opCtx, results);
}

}  // namespace mongo
