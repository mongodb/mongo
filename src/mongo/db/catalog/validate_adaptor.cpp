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


#include "mongo/platform/basic.h"

#include "mongo/db/catalog/validate_adaptor.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/column_index_consistency.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/catalog/throttle_cursor.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/columns_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

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
        results->valid = false;
        results->errors.push_back(str::stream() << rid << " " << ridFromDoc.getStatus().reason());
        results->corruptRecords.push_back(rid);
        return;
    }

    const auto ksFromBSON =
        KeyString::Builder(KeyString::Version::kLatestVersion, ridFromDoc.getValue());
    const auto ksFromRid = KeyString::Builder(KeyString::Version::kLatestVersion, rid);

    const auto clusterKeyField = clustered_util::getClusterKeyFieldName(indexSpec);
    if (ksFromRid != ksFromBSON) {
        results->valid = false;
        results->errors.push_back(
            str::stream() << "Document with " << rid << " has mismatched " << doc[clusterKeyField]
                          << " (RecordId KeyString='" << ksFromRid.toString()
                          << "', cluster key KeyString='" << ksFromBSON.toString() << "')");
        results->corruptRecords.push_back(rid);
    }
}

void schemaValidationFailed(CollectionValidation::ValidateState* state,
                            Collection::SchemaValidationResult result,
                            ValidateResults* results) {
    invariant(Collection::SchemaValidationResult::kPass != result);

    if (state->isCollectionSchemaViolated()) {
        // Only report the message once.
        return;
    }

    state->setCollectionSchemaViolated();

    // When testing is enabled, only warn about non-compliant documents to prevent test failures.
    if (TestingProctor::instance().isEnabled() ||
        Collection::SchemaValidationResult::kWarn == result) {
        results->warnings.push_back(kSchemaValidationFailedReason);
    } else if (Collection::SchemaValidationResult::kError == result) {
        results->errors.push_back(kSchemaValidationFailedReason);
        results->valid = false;
    }
}

// Checks that 'control.count' matches the actual number of measurements in a closed bucket.
Status _validateTimeseriesCount(const BSONObj& control, int bucketCount, int version) {
    if (version == 1) {
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
Status _validateTimeSeriesIdTimestamp(const CollectionPtr& collection, const BSONObj& recordBson) {
    // Compares both timestamps measured in seconds.
    int64_t minTimestamp = recordBson.getField(timeseries::kBucketControlFieldName)
                               .Obj()
                               .getField(timeseries::kBucketControlMinFieldName)
                               .Obj()
                               .getField(collection->getTimeseriesOptions()->getTimeField())
                               .timestamp()
                               .asInt64() /
        1000;
    int64_t oidEmbeddedTimestamp =
        recordBson.getField(timeseries::kBucketIdFieldName).OID().getTimestamp();
    if (minTimestamp != oidEmbeddedTimestamp) {
        return Status(
            ErrorCodes::InvalidIdField,
            fmt::format("Mismatch between the embedded timestamp {} in the time-series "
                        "bucket '_id' field and the timestamp {} in 'control.min' field.",
                        Date_t::fromMillisSinceEpoch(oidEmbeddedTimestamp * 1000).toString(),
                        Date_t::fromMillisSinceEpoch(minTimestamp * 1000).toString()));
    }
    return Status::OK();
}

/**
 * Checks the value of the bucket's version.
 */
Status _validateTimeseriesControlVersion(const BSONObj& recordBson, int bucketVersion) {
    if (bucketVersion != 1 && bucketVersion != 2) {
        return Status(
            ErrorCodes::BadValue,
            fmt::format("Invalid value for 'control.version'. Expected 1 or 2, but got {}.",
                        bucketVersion));
    }
    return Status::OK();
}

/**
 * Checks if the bucket's version matches the types of 'data' fields.
 */
Status _validateTimeseriesDataFieldTypes(const BSONElement& dataField, int bucketVersion) {
    auto dataType = bucketVersion == 1 ? BSONType::Object : BSONType::BinData;
    // Checks that open buckets have 'Object' type and closed buckets have 'BinData Column' type.
    auto isCorrectType = [&](BSONElement el) {
        if (bucketVersion == 1) {
            return el.type() == BSONType::Object;
        } else {
            return el.type() == BSONType::BinData && el.binDataType() == BinDataType::Column;
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

/**
 * Checks whether the min and max values between 'control' and 'data' match, taking timestamp
 * granularity into account.
 */
Status _validateTimeSeriesMinMax(const CollectionPtr& coll,
                                 timeseries::bucket_catalog::MinMax& minmax,
                                 const BSONElement& controlMin,
                                 const BSONElement& controlMax,
                                 StringData fieldName) {
    auto min = minmax.min();
    auto max = minmax.max();
    auto checkMinAndMaxMatch = [&]() {
        const auto options = coll->getTimeseriesOptions().value();
        if (fieldName == options.getTimeField()) {
            return controlMin.Date() ==
                timeseries::roundTimestampToGranularity(min.getField(fieldName).Date(), options) &&
                controlMax.Date() == max.getField(fieldName).Date();
        } else {
            return controlMin.wrap().woCompare(min) == 0 && controlMax.wrap().woCompare(max) == 0;
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
        auto idxInt = std::stoi(idx.toString());
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
                                        int version,
                                        int* bucketCount) {
    timeseries::bucket_catalog::MinMax minmax;
    if (version == 1) {
        for (const auto& metric : timeField.Obj()) {
            if (metric.type() != BSONType::Date) {
                return Status(ErrorCodes::BadValue,
                              fmt::format("Time-series bucket {} field is not a Date", fieldName));
            }
            // Checks that indices are consecutively increasing numbers starting from 0.
            if (auto idx = _idxInt(metric.fieldNameStringData()); idx != *bucketCount) {
                return Status(ErrorCodes::BadValue,
                              fmt::format("The index '{}' in time-series bucket data field '{}' is "
                                          "not consecutively increasing from '0'",
                                          metric.fieldNameStringData(),
                                          fieldName));
            }
            minmax.update(metric.wrap(fieldName), boost::none, coll->getDefaultCollator());
            ++(*bucketCount);
        }
    } else {
        BSONColumn col{timeField};
        Date_t prevTimestamp = Date_t::min();
        for (const auto& metric : col) {
            if (!metric.eoo()) {
                if (metric.type() != BSONType::Date) {
                    return Status(
                        ErrorCodes::BadValue,
                        fmt::format("Time-series bucket '{}' field is not a Date", fieldName));
                }
                // Checks the time values are sorted in increasing order for compressed buckets.
                Date_t curTimestamp = metric.Date();
                if (curTimestamp >= prevTimestamp) {
                    prevTimestamp = curTimestamp;
                } else {
                    return Status(
                        ErrorCodes::BadValue,
                        fmt::format("Time-series bucket '{}' field is not in ascending order",
                                    fieldName));
                }
                minmax.update(metric.wrap(fieldName), boost::none, coll->getDefaultCollator());
                ++(*bucketCount);
            } else {
                return Status(ErrorCodes::BadValue, "Time-series bucket has missing time fields");
            }
        }
    }

    if (Status status = _validateTimeSeriesMinMax(coll, minmax, controlMin, controlMax, fieldName);
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
                                    int version,
                                    int bucketCount) {
    timeseries::bucket_catalog::MinMax minmax;
    if (version == 1) {
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
    } else {
        BSONColumn col{dataField};
        for (const auto& metric : col) {
            if (!metric.eoo()) {
                minmax.update(metric.wrap(fieldName), boost::none, coll->getDefaultCollator());
            }
        }
    }

    if (Status status = _validateTimeSeriesMinMax(coll, minmax, controlMin, controlMax, fieldName);
        !status.isOK()) {
        return status;
    }

    return Status::OK();
}

Status _validateTimeSeriesDataFields(const CollectionPtr& coll,
                                     const BSONObj& recordBson,
                                     int bucketVersion) {
    BSONObj data = recordBson.getField(timeseries::kBucketDataFieldName).Obj();
    BSONObj control = recordBson.getField(timeseries::kBucketControlFieldName).Obj();
    BSONObj controlMin = control.getField(timeseries::kBucketControlMinFieldName).Obj();
    BSONObj controlMax = control.getField(timeseries::kBucketControlMaxFieldName).Obj();

    // Builds a hash map for the fields to avoid repeated traversals.
    auto buildFieldTable = [&](StringMap<BSONElement>* table, const BSONObj& fields) {
        for (const auto& field : fields) {
            table->insert({field.fieldNameStringData().toString(), field});
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
    auto timeFieldName = coll->getTimeseriesOptions().value().getTimeField().toString();
    if (Status status = _validateTimeseriesDataFieldTypes(dataFields[timeFieldName], bucketVersion);
        !status.isOK()) {
        return status;
    }

    if (Status status = _validateTimeSeriesDataTimeField(coll,
                                                         dataFields[timeFieldName],
                                                         controlMinFields[timeFieldName],
                                                         controlMaxFields[timeFieldName],
                                                         timeFieldName,
                                                         bucketVersion,
                                                         &bucketCount);
        !status.isOK()) {
        return status;
    }

    if (Status status = _validateTimeseriesCount(control, bucketCount, bucketVersion);
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
                                                             bucketVersion,
                                                             bucketCount);
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
Status _validateTimeSeriesBucketRecord(const CollectionPtr& collection,
                                       const BSONObj& recordBson,
                                       ValidateResults* results) {
    int bucketVersion = recordBson.getField(timeseries::kBucketControlFieldName)
                            .Obj()
                            .getIntField(timeseries::kBucketControlVersionFieldName);

    if (Status status = _validateTimeSeriesIdTimestamp(collection, recordBson); !status.isOK()) {
        return status;
    }

    if (Status status = _validateTimeseriesControlVersion(recordBson, bucketVersion);
        !status.isOK()) {
        return status;
    }

    if (Status status = _validateTimeSeriesDataFields(collection, recordBson, bucketVersion);
        !status.isOK()) {
        return status;
    }

    return Status::OK();
}


void _timeseriesValidationFailed(CollectionValidation::ValidateState* state,
                                 ValidateResults* results) {
    if (state->isTimeseriesDataInconsistent()) {
        // Only report the warning message once.
        return;
    }
    state->setTimeseriesDataInconsistent();

    results->warnings.push_back(kTimeseriesValidationInconsistencyReason);
}

void _BSONSpecValidationFailed(CollectionValidation::ValidateState* state,
                               ValidateResults* results) {
    if (state->isBSONDataNonConformant()) {
        // Only report the warning message once.
        return;
    }
    state->setBSONDataNonConformant();

    results->warnings.push_back(kBSONValidationNonConformantReason);
}
}  // namespace

Status ValidateAdaptor::validateRecord(OperationContext* opCtx,
                                       const RecordId& recordId,
                                       const RecordData& record,
                                       long long* nNonCompliantDocuments,
                                       size_t* dataSize,
                                       ValidateResults* results) {
    auto validateBSONMode = BSONValidateMode::kDefault;
    if (feature_flags::gExtendValidateCommand.isEnabled(serverGlobalParams.featureCompatibility)) {
        validateBSONMode = _validateState->getBSONValidateMode();
    }
    const Status status = validateBSON(record.data(), record.size(), validateBSONMode);
    if (!status.isOK()) {
        if (status.code() != ErrorCodes::NonConformantBSON) {
            return status;
        }
        LOGV2_WARNING(6825900,
                      "Document is not conformant to BSON specifications",
                      "recordId"_attr = recordId,
                      "reason"_attr = status);
        (*nNonCompliantDocuments)++;
        _BSONSpecValidationFailed(_validateState, results);
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

    SharedBufferFragmentBuilder pool(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

    for (const auto& index : _validateState->getIndexes()) {
        const IndexDescriptor* descriptor = index->descriptor();
        if (descriptor->isPartial() && !index->getFilterExpression()->matchesBSON(recordBson))
            continue;


        this->traverseRecord(opCtx, coll, index.get(), recordId, recordBson, results);
    }
    return Status::OK();
}

void ValidateAdaptor::traverseRecordStore(OperationContext* opCtx,
                                          ValidateResults* results,
                                          BSONObjBuilder* output) {
    _numRecords = 0;  // need to reset it because this function can be called more than once.
    long long dataSizeTotal = 0;
    long long interruptIntervalNumBytes = 0;
    long long nInvalid = 0;
    long long nNonCompliantDocuments = 0;
    long long numCorruptRecordsSizeBytes = 0;

    ON_BLOCK_EXIT([&]() {
        output->appendNumber("nInvalidDocuments", nInvalid);
        output->appendNumber("nNonCompliantDocuments", nNonCompliantDocuments);
        output->appendNumber("nrecords", _numRecords);
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
    const auto totalRecords = coll->getRecordStore()->numRecords(opCtx);
    const auto rs = coll->getRecordStore();
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        _progress.set(lk, CurOp::get(opCtx)->setProgress_inlock(curopMessage, totalRecords), opCtx);
    }

    if (_validateState->getFirstRecordId().isNull()) {
        // The record store is empty if the first RecordId isn't initialized.
        return;
    }

    bool corruptRecordsSizeLimitWarning = false;
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
        Status status = validateRecord(
            opCtx, record->id, record->data, &nNonCompliantDocuments, &validatedSize, results);

        // validatedSize = dataSize is not a general requirement as some storage engines may use
        // padding, but we still require that they return the unpadded record data.
        if (!status.isOK() || validatedSize != static_cast<size_t>(dataSize)) {
            // If status is not okay, dataSize is not reliable.
            if (!status.isOK()) {
                LOGV2(4835001,
                      "Document corruption details - Document validation failed with error",
                      "recordId"_attr = record->id,
                      "error"_attr = status);
            } else {
                LOGV2(4835002,
                      "Document corruption details - Document validation failure; size mismatch",
                      "recordId"_attr = record->id,
                      "validatedBytes"_attr = validatedSize,
                      "recordBytes"_attr = dataSize);
            }

            if (_validateState->fixErrors()) {
                writeConflictRetry(
                    opCtx, "corrupt record removal", _validateState->nss().ns(), [&] {
                        WriteUnitOfWork wunit(opCtx);
                        rs->deleteRecord(opCtx, record->id);
                        wunit.commit();
                    });
                results->repaired = true;
                results->numRemovedCorruptRecords++;
                _numRecords--;
            } else {
                if (results->valid) {
                    results->errors.push_back("Detected one or more invalid documents. See logs.");
                    results->valid = false;
                }

                numCorruptRecordsSizeBytes += record->id.memUsage();
                if (numCorruptRecordsSizeBytes <= kMaxErrorSizeBytes) {
                    results->corruptRecords.push_back(record->id);
                } else if (!corruptRecordsSizeLimitWarning) {
                    results->warnings.push_back(
                        "Not all corrupted records are listed due to size limitations.");
                    corruptRecordsSizeLimitWarning = true;
                }

                nInvalid++;
            }
        } else {
            // If the document is not corrupted, validate the document against this collection's
            // schema validator. Don't treat invalid documents as errors since documents can bypass
            // document validation when being inserted or updated.
            auto result = coll->checkValidation(opCtx, record->data.toBson());

            if (result.first != Collection::SchemaValidationResult::kPass) {
                LOGV2_WARNING(5363500,
                              "Document is not compliant with the collection's schema",
                              logAttrs(coll->ns()),
                              "recordId"_attr = record->id,
                              "reason"_attr = result.second);

                nNonCompliantDocuments++;
                schemaValidationFailed(_validateState, result.first, results);
            } else if (feature_flags::gExtendValidateCommand.isEnabled(
                           serverGlobalParams.featureCompatibility) &&
                       coll->getTimeseriesOptions()) {
                // Checks for time-series collection consistency.
                Status bucketStatus =
                    _validateTimeSeriesBucketRecord(coll, record->data.toBson(), results);
                // This log id should be kept in sync with the associated warning messages that are
                // returned to the client.
                if (!bucketStatus.isOK()) {
                    LOGV2_WARNING(6698300,
                                  "Document is not compliant with time-series specifications",
                                  logAttrs(coll->ns()),
                                  "recordId"_attr = record->id,
                                  "reason"_attr = bucketStatus);
                    nNonCompliantDocuments++;
                    _timeseriesValidationFailed(_validateState, results);
                }
            }
        }

        prevRecordId = record->id;

        if (_numRecords % IndexConsistency::kInterruptIntervalNumRecords == 0 ||
            interruptIntervalNumBytes >= kInterruptIntervalNumBytes) {
            // Periodically checks for interrupts and yields.
            opCtx->checkForInterrupt();
            _validateState->yield(opCtx);

            if (interruptIntervalNumBytes >= kInterruptIntervalNumBytes) {
                interruptIntervalNumBytes = 0;
            }
        }
    }

    if (results->numRemovedCorruptRecords > 0) {
        results->warnings.push_back(str::stream() << "Removed " << results->numRemovedCorruptRecords
                                                  << " invalid documents.");
    }

    const auto fastCount = coll->numRecords(opCtx);
    if (_validateState->shouldEnforceFastCount() && fastCount != _numRecords) {
        results->errors.push_back(str::stream() << "fast count (" << fastCount
                                                << ") does not match number of records ("
                                                << _numRecords << ") for collection '"
                                                << coll->ns().toStringForErrorMsg() << "'");
        results->valid = false;
    }

    // Do not update the record store stats if we're in the background as we've validated a
    // checkpoint and it may not have the most up-to-date changes.
    if (results->valid && !_validateState->isBackground()) {
        coll->getRecordStore()->updateStatsAfterRepair(opCtx, _numRecords, dataSizeTotal);
    }
}

bool isColumnStoreIndex(const IndexCatalogEntry* index) {
    return index->descriptor()->getAccessMethodName() == IndexNames::COLUMN;
}

void ValidateAdaptor::validateIndexKeyCount(OperationContext* opCtx,
                                            const IndexCatalogEntry* index,
                                            IndexValidateResults& results) {
    if (isColumnStoreIndex(index)) {
        _columnIndexConsistency.validateIndexKeyCount(opCtx, index, &_numRecords, results);
    } else {
        _keyBasedIndexConsistency.validateIndexKeyCount(opCtx, index, &_numRecords, results);
    }
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
                      CurOp::get(opCtx)->setProgress_inlock(
                          curopMessage,
                          isColumnStoreIndex(index)
                              ? _columnIndexConsistency.getTotalIndexKeys()
                              : _keyBasedIndexConsistency.getTotalIndexKeys()),
                      opCtx);
    }

    int64_t numKeys = 0;
    if (isColumnStoreIndex(index)) {
        numKeys += _columnIndexConsistency.traverseIndex(opCtx, index, _progress, results);
    } else {
        numKeys += _keyBasedIndexConsistency.traverseIndex(opCtx, index, _progress, results);
    }

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
    if (isColumnStoreIndex(index)) {
        _columnIndexConsistency.traverseRecord(opCtx, coll, index, recordId, record, results);
    } else {
        _keyBasedIndexConsistency.traverseRecord(opCtx, coll, index, recordId, record, results);
    }
}

void ValidateAdaptor::setSecondPhase() {
    _columnIndexConsistency.setSecondPhase();
    _keyBasedIndexConsistency.setSecondPhase();
}

bool ValidateAdaptor::limitMemoryUsageForSecondPhase(ValidateResults* result) {
    bool retVal = true;
    retVal &= _columnIndexConsistency.limitMemoryUsageForSecondPhase(result);
    retVal &= _keyBasedIndexConsistency.limitMemoryUsageForSecondPhase(result);
    return retVal;
}

bool ValidateAdaptor::haveEntryMismatch() const {
    bool retVal = false;
    retVal |= _columnIndexConsistency.haveEntryMismatch();
    retVal |= _keyBasedIndexConsistency.haveEntryMismatch();
    return retVal;
}

void ValidateAdaptor::repairIndexEntries(OperationContext* opCtx, ValidateResults* results) {
    _columnIndexConsistency.repairIndexEntries(opCtx, results);
    _keyBasedIndexConsistency.repairIndexEntries(opCtx, results);
}

void ValidateAdaptor::addIndexEntryErrors(OperationContext* opCtx, ValidateResults* results) {
    _columnIndexConsistency.addIndexEntryErrors(opCtx, results);
    _keyBasedIndexConsistency.addIndexEntryErrors(opCtx, results);
}

}  // namespace mongo
