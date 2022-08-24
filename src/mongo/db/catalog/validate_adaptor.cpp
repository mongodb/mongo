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
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/catalog/throttle_cursor.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
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
#include "mongo/db/timeseries/flat_bson.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(crashOnMultikeyValidateFailure);

// Set limit for size of corrupted records that will be reported.
const long long kMaxErrorSizeBytes = 1 * 1024 * 1024;
const long long kInterruptIntervalNumRecords = 4096;
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

    // TODO SERVER-65078: remove the testing proctor check.
    // When testing is enabled, only warn about non-compliant documents to prevent test failures.
    if (TestingProctor::instance().isEnabled() ||
        Collection::SchemaValidationResult::kWarn == result) {
        results->warnings.push_back(kSchemaValidationFailedReason);
    } else if (Collection::SchemaValidationResult::kError == result) {
        results->errors.push_back(kSchemaValidationFailedReason);
        results->valid = false;
    }
}

int _getTimeseriesBucketVersion(const BSONObj& recordBson) {
    return recordBson.getField(timeseries::kBucketControlFieldName)
        .Obj()
        .getField(timeseries::kBucketControlVersionFieldName)
        .Number();
}

// Checks that 'control.count' matches the actual number of measurements in a version 2 document.
Status _validateTimeseriesCount(const BSONObj& recordBson, StringData timeFieldName) {
    if (_getTimeseriesBucketVersion(recordBson) == 1) {
        return Status::OK();
    }
    size_t controlCount = recordBson.getField(timeseries::kBucketControlFieldName)
                              .Obj()
                              .getField(timeseries::kBucketControlCountFieldName)
                              .Number();
    BSONColumn col{
        recordBson.getField(timeseries::kBucketDataFieldName).Obj().getField(timeFieldName)};
    if (controlCount != col.size()) {
        return Status(ErrorCodes::BadValue,
                      fmt::format("The 'control.count' field ({}) does not match the actual number "
                                  "of measurements in the document ({}).",
                                  controlCount,
                                  col.size()));
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
 * Checks the value of the bucket's version and if it matches the types of 'data' fields.
 */
Status _validateTimeseriesControlVersion(const BSONObj& recordBson) {
    int controlVersion = _getTimeseriesBucketVersion(recordBson);
    if (controlVersion != 1 && controlVersion != 2) {
        return Status(
            ErrorCodes::BadValue,
            fmt::format("Invalid value for 'control.version'. Expected 1 or 2, but got {}.",
                        controlVersion));
    }
    auto dataType = controlVersion == 1 ? BSONType::Object : BSONType::BinData;
    // In addition to checking dataType, make sure that closed buckets have BinData Column subtype.
    auto isCorrectType = [&](BSONElement el) {
        if (controlVersion == 1) {
            return el.type() == BSONType::Object;
        } else {
            return el.type() == BSONType::BinData && el.binDataType() == BinDataType::Column;
        }
    };
    BSONObj data = recordBson.getField(timeseries::kBucketDataFieldName).Obj();
    for (BSONObjIterator bi(data); bi.more();) {
        BSONElement e = bi.next();
        if (!isCorrectType(e)) {
            return Status(ErrorCodes::TypeMismatch,
                          fmt::format("Mismatch between time-series schema version and data field "
                                      "type. Expected type {}, but got {}.",
                                      mongo::typeName(dataType),
                                      mongo::typeName(e.type())));
        }
    }
    return Status::OK();
}

/**
 * Checks the equivalence between the min and max fields in 'control' for a bucket and
 * the corresponding value in 'data'.
 */
Status _validateTimeseriesMinMax(const BSONObj& recordBson, const CollectionPtr& coll) {
    BSONObj data = recordBson.getField(timeseries::kBucketDataFieldName).Obj();
    BSONObj control = recordBson.getField(timeseries::kBucketControlFieldName).Obj();
    BSONObj controlMin = control.getField(timeseries::kBucketControlMinFieldName).Obj();
    BSONObj controlMax = control.getField(timeseries::kBucketControlMaxFieldName).Obj();

    auto dataFields = data.getFieldNames<std::set<std::string>>();
    auto controlMinFields = controlMin.getFieldNames<std::set<std::string>>();
    auto controlMaxFields = controlMax.getFieldNames<std::set<std::string>>();
    int version = _getTimeseriesBucketVersion(recordBson);

    // Checks that the number of 'control.min' and 'control.max' fields agrees with number of 'data'
    // fields.
    if (dataFields.size() != controlMinFields.size() ||
        dataFields.size() != controlMaxFields.size()) {
        return Status(
            ErrorCodes::BadValue,
            fmt::format(
                "Mismatch between the number of time-series control fields and the number "
                "of data fields. "
                "Control had {} min fields and {} max fields, but observed data had {} fields.",
                controlMinFields.size(),
                controlMaxFields.size(),
                dataFields.size()));
    };

    // Validates that the 'control.min' and 'control.max' field values agree with 'data' field
    // values.
    for (auto fieldName : dataFields) {
        timeseries::MinMax minmax;
        auto field = data.getField(fieldName);

        if (version == 1) {
            for (const BSONElement& el : field.Obj()) {
                minmax.update(el.wrap(fieldName), boost::none, coll->getDefaultCollator());
            }
        } else {
            BSONColumn col{field};
            for (const BSONElement& el : col) {
                if (!el.eoo()) {
                    minmax.update(el.wrap(fieldName), boost::none, coll->getDefaultCollator());
                }
            }
        }

        auto controlFieldMin = controlMin.getField(fieldName);
        auto controlFieldMax = controlMax.getField(fieldName);
        auto min = minmax.min();
        auto max = minmax.max();

        // Checks whether the min and max values between 'control' and 'data' match, taking
        // timestamp granularity into account.
        auto checkMinAndMaxMatch = [&]() {
            // Needed for granularity, which determines how the min timestamp is rounded down .
            const auto options = coll->getTimeseriesOptions().value();
            if (fieldName == options.getTimeField()) {
                return controlFieldMin.Date() ==
                    timeseries::roundTimestampToGranularity(min.getField(fieldName).Date(),
                                                            options) &&
                    controlFieldMax.Date() == max.getField(fieldName).Date();
            } else {
                return controlFieldMin.wrap().woCompare(min) == 0 &&
                    controlFieldMax.wrap().woCompare(max) == 0;
            }
        };

        if (!checkMinAndMaxMatch()) {
            return Status(
                ErrorCodes::BadValue,
                fmt::format(
                    "Mismatch between time-series control and observed min or max for field {}. "
                    "Control had min {} and max {}, but observed data had min {} and max {}.",
                    fieldName,
                    controlFieldMin.toString(),
                    controlFieldMax.toString(),
                    min.toString(),
                    max.toString()));
        }
    }

    return Status::OK();
}


Status _validateTimeSeriesDataIndexes(const BSONObj& recordBson, const CollectionPtr& coll) {
    BSONObj data = recordBson.getField(timeseries::kBucketDataFieldName).Obj();
    int maxIndex;

    // go through once, validating id and timestamps and getting count of total number of entries
    // for maxIndex.
    for (auto field : data) {
        // Checks that indices are consecutively increasing numbers starting from 0.
        auto fieldName = field.fieldNameStringData();
        if (fieldName == coll->getTimeseriesOptions()->getTimeField() || fieldName == "_id") {
            int counter = 0;
            for (auto idx : field.Obj()) {
                if (auto idxAsNum = idx.fieldNameStringData();
                    std::stoi(idxAsNum.toString()) != counter) {
                    return Status(ErrorCodes::BadValue, "wrong index");
                }
                ++counter;
            }
            maxIndex = counter;
        }
    }

    for (auto field : data) {
        auto fieldName = field.fieldNameStringData();
        if (fieldName != coll->getTimeseriesOptions()->getTimeField() && fieldName != "_id") {
            // Checks that indices are in increasing order and within correct range.
            // Smallest int
            int prev = INT_MIN;
            for (auto idx : field.Obj()) {
                auto idxAsNum = std::stoi(idx.fieldNameStringData().toString());
                if (idxAsNum <= prev || idxAsNum > maxIndex || idxAsNum < 0) {
                    return Status(ErrorCodes::BadValue, "wrong index");
                }
                prev = idxAsNum;
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

    if (Status status = _validateTimeseriesControlVersion(recordBson); !status.isOK()) {
        return status;
    }

    if (Status status = _validateTimeSeriesIdTimestamp(collection, recordBson); !status.isOK()) {
        return status;
    }

    if (Status status = _validateTimeseriesMinMax(recordBson, collection); !status.isOK()) {
        return status;
    }

    if (_getTimeseriesBucketVersion(recordBson) == 1) {
        if (Status status = _validateTimeSeriesDataIndexes(recordBson, collection);
            !status.isOK()) {
            return status;
        }
    }

    if (Status status = _validateTimeseriesCount(
            recordBson, collection->getTimeseriesOptions()->getTimeField());
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
    if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        feature_flags::gExtendValidateCommand.isEnabled(serverGlobalParams.featureCompatibility)) {
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

    if (MONGO_unlikely(_validateState->extraLoggingForTest())) {
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

    auto& executionCtx = StorageExecutionContext::get(opCtx);
    SharedBufferFragmentBuilder pool(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);

    for (const auto& index : _validateState->getIndexes()) {
        const IndexDescriptor* descriptor = index->descriptor();
        auto iam = index->accessMethod()->asSortedData();

        if (descriptor->isPartial() && !index->getFilterExpression()->matchesBSON(recordBson))
            continue;

        auto documentKeySet = executionCtx.keys();
        auto multikeyMetadataKeys = executionCtx.multikeyMetadataKeys();
        auto documentMultikeyPaths = executionCtx.multikeyPaths();

        iam->getKeys(opCtx,
                     coll,
                     pool,
                     recordBson,
                     InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints,
                     SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                     documentKeySet.get(),
                     multikeyMetadataKeys.get(),
                     documentMultikeyPaths.get(),
                     recordId);

        bool shouldBeMultikey = iam->shouldMarkIndexAsMultikey(
            documentKeySet->size(),
            {multikeyMetadataKeys->begin(), multikeyMetadataKeys->end()},
            *documentMultikeyPaths);

        if (!index->isMultikey(opCtx, coll) && shouldBeMultikey) {
            if (_validateState->fixErrors()) {
                writeConflictRetry(opCtx, "setIndexAsMultikey", coll->ns().ns(), [&] {
                    WriteUnitOfWork wuow(opCtx);
                    coll->getIndexCatalog()->setMultikeyPaths(
                        opCtx, coll, descriptor, *multikeyMetadataKeys, *documentMultikeyPaths);
                    wuow.commit();
                });

                LOGV2(4614700,
                      "Index set to multikey",
                      "indexName"_attr = descriptor->indexName(),
                      "collection"_attr = coll->ns().ns());
                results->warnings.push_back(str::stream() << "Index " << descriptor->indexName()
                                                          << " set to multikey.");
                results->repaired = true;
            } else {
                auto& curRecordResults = (results->indexResultsMap)[descriptor->indexName()];
                std::string msg = fmt::format(
                    "Index {} is not multikey but has more than one key in document with "
                    "RecordId({}) and {}",
                    descriptor->indexName(),
                    recordId.toString(),
                    recordBson.getField("_id").toString());
                curRecordResults.errors.push_back(msg);
                curRecordResults.valid = false;
                if (crashOnMultikeyValidateFailure.shouldFail()) {
                    invariant(false, msg);
                }
            }
        }

        if (index->isMultikey(opCtx, coll)) {
            const MultikeyPaths& indexPaths = index->getMultikeyPaths(opCtx, coll);
            if (!MultikeyPathTracker::covers(indexPaths, *documentMultikeyPaths.get())) {
                if (_validateState->fixErrors()) {
                    writeConflictRetry(opCtx, "increaseMultikeyPathCoverage", coll->ns().ns(), [&] {
                        WriteUnitOfWork wuow(opCtx);
                        coll->getIndexCatalog()->setMultikeyPaths(
                            opCtx, coll, descriptor, *multikeyMetadataKeys, *documentMultikeyPaths);
                        wuow.commit();
                    });

                    LOGV2(4614701,
                          "Multikey paths updated to cover multikey document",
                          "indexName"_attr = descriptor->indexName(),
                          "collection"_attr = coll->ns().ns());
                    results->warnings.push_back(str::stream() << "Index " << descriptor->indexName()
                                                              << " multikey paths updated.");
                    results->repaired = true;
                } else {
                    std::string msg = fmt::format(
                        "Index {} multikey paths do not cover a document with RecordId({}) and {}",
                        descriptor->indexName(),
                        recordId.toString(),
                        recordBson.getField("_id").toString());
                    auto& curRecordResults = (results->indexResultsMap)[descriptor->indexName()];
                    curRecordResults.errors.push_back(msg);
                    curRecordResults.valid = false;
                }
            }
        }

        IndexInfo& indexInfo = _indexConsistency->getIndexInfo(descriptor->indexName());
        if (shouldBeMultikey) {
            indexInfo.multikeyDocs = true;
        }

        // An empty set of multikey paths indicates that this index does not track path-level
        // multikey information and we should do no tracking.
        if (shouldBeMultikey && documentMultikeyPaths->size()) {
            _indexConsistency->addDocumentMultikeyPaths(&indexInfo, *documentMultikeyPaths);
        }

        for (const auto& keyString : *multikeyMetadataKeys) {
            try {
                _indexConsistency->addMultikeyMetadataPath(keyString, &indexInfo);
            } catch (...) {
                return exceptionToStatus();
            }
        }

        for (const auto& keyString : *documentKeySet) {
            try {
                _totalIndexKeys++;
                _indexConsistency->addDocKey(opCtx, keyString, &indexInfo, recordId);
            } catch (...) {
                return exceptionToStatus();
            }
        }
    }
    return Status::OK();
}

namespace {
// Ensures that index entries are in increasing or decreasing order.
void _validateKeyOrder(OperationContext* opCtx,
                       const IndexCatalogEntry* index,
                       const KeyString::Value& currKey,
                       const KeyString::Value& prevKey,
                       IndexValidateResults* results) {
    auto descriptor = index->descriptor();
    bool unique = descriptor->unique();

    // KeyStrings will be in strictly increasing order because all keys are sorted and they are in
    // the format (Key, RID), and all RecordIDs are unique.
    if (currKey.compare(prevKey) <= 0) {
        if (results && results->valid) {
            results->errors.push_back(str::stream()
                                      << "index '" << descriptor->indexName()
                                      << "' is not in strictly ascending or descending order");
        }
        if (results) {
            results->valid = false;
        }
        return;
    }

    if (unique) {
        // Unique indexes must not have duplicate keys.
        int cmp = currKey.compareWithoutRecordIdLong(prevKey);
        if (cmp != 0) {
            return;
        }

        if (results && results->valid) {
            auto bsonKey = KeyString::toBson(currKey, Ordering::make(descriptor->keyPattern()));
            auto firstRecordId =
                KeyString::decodeRecordIdLongAtEnd(prevKey.getBuffer(), prevKey.getSize());
            auto secondRecordId =
                KeyString::decodeRecordIdLongAtEnd(currKey.getBuffer(), currKey.getSize());
            results->errors.push_back(str::stream() << "Unique index '" << descriptor->indexName()
                                                    << "' has duplicate key: " << bsonKey
                                                    << ", first record: " << firstRecordId
                                                    << ", second record: " << secondRecordId);
        }
        if (results) {
            results->valid = false;
        }
    }
}
}  // namespace

void ValidateAdaptor::traverseIndex(OperationContext* opCtx,
                                    const IndexCatalogEntry* index,
                                    int64_t* numTraversedKeys,
                                    ValidateResults* results) {
    const IndexDescriptor* descriptor = index->descriptor();
    auto indexName = descriptor->indexName();
    auto& indexResults = results->indexResultsMap[indexName];
    IndexInfo& indexInfo = _indexConsistency->getIndexInfo(indexName);
    int64_t numKeys = 0;

    bool isFirstEntry = true;

    // The progress meter will be inactive after traversing the record store to allow the message
    // and the total to be set to different values.
    if (!_progress->isActive()) {
        const char* curopMessage = "Validate: scanning index entries";
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        _progress.set(CurOp::get(opCtx)->setProgress_inlock(curopMessage, _totalIndexKeys));
    }

    const KeyString::Version version =
        index->accessMethod()->asSortedData()->getSortedDataInterface()->getKeyStringVersion();

    KeyString::Builder firstKeyStringBuilder(
        version, BSONObj(), indexInfo.ord, KeyString::Discriminator::kExclusiveBefore);
    KeyString::Value firstKeyString = firstKeyStringBuilder.getValueCopy();
    KeyString::Value prevIndexKeyStringValue;

    // Ensure that this index has an open index cursor.
    const auto indexCursorIt = _validateState->getIndexCursors().find(indexName);
    invariant(indexCursorIt != _validateState->getIndexCursors().end());

    const std::unique_ptr<SortedDataInterfaceThrottleCursor>& indexCursor = indexCursorIt->second;

    boost::optional<KeyStringEntry> indexEntry;
    try {
        indexEntry = indexCursor->seekForKeyString(opCtx, firstKeyString);
    } catch (const DBException& ex) {
        if (TestingProctor::instance().isEnabled() && ex.code() != ErrorCodes::WriteConflict) {
            LOGV2_FATAL(5318400,
                        "Error seeking to first key",
                        "error"_attr = ex.toString(),
                        "index"_attr = indexName,
                        "key"_attr = firstKeyString.toString());
        }
        throw;
    }

    const auto keyFormat =
        index->accessMethod()->asSortedData()->getSortedDataInterface()->rsKeyFormat();
    const RecordId kWildcardMultikeyMetadataRecordId = record_id_helpers::reservedIdFor(
        record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, keyFormat);

    // Warn about unique indexes with keys in old format (without record id).
    bool foundOldUniqueIndexKeys = false;

    while (indexEntry) {
        if (!isFirstEntry) {
            _validateKeyOrder(
                opCtx, index, indexEntry->keyString, prevIndexKeyStringValue, &indexResults);
        }

        if (!foundOldUniqueIndexKeys && !descriptor->isIdIndex() && descriptor->unique() &&
            !indexCursor->isRecordIdAtEndOfKeyString()) {
            results->warnings.push_back(
                fmt::format("Unique index {} has one or more keys in the old format (without "
                            "embedded record id). First record: {}",
                            indexInfo.indexName,
                            indexEntry->loc.toString()));
            foundOldUniqueIndexKeys = true;
        }

        bool isMetadataKey = indexEntry->loc == kWildcardMultikeyMetadataRecordId;
        if (descriptor->getIndexType() == IndexType::INDEX_WILDCARD && isMetadataKey) {
            _indexConsistency->removeMultikeyMetadataPath(indexEntry->keyString, &indexInfo);
        } else {
            try {
                _indexConsistency->addIndexKey(
                    opCtx, indexEntry->keyString, &indexInfo, indexEntry->loc, results);
            } catch (const DBException& e) {
                StringBuilder ss;
                ss << "Parsing index key for " << indexInfo.indexName << " recId "
                   << indexEntry->loc << " threw exception " << e.toString();
                results->errors.push_back(ss.str());
                results->valid = false;
            }
        }

        _progress->hit();
        numKeys++;
        isFirstEntry = false;
        prevIndexKeyStringValue = indexEntry->keyString;

        if (numKeys % kInterruptIntervalNumRecords == 0) {
            // Periodically checks for interrupts and yields.
            opCtx->checkForInterrupt();
            _validateState->yield(opCtx);
        }

        try {
            indexEntry = indexCursor->nextKeyString(opCtx);
        } catch (const DBException& ex) {
            if (TestingProctor::instance().isEnabled() && ex.code() != ErrorCodes::WriteConflict) {
                LOGV2_FATAL(5318401,
                            "Error advancing index cursor",
                            "error"_attr = ex.toString(),
                            "index"_attr = indexName,
                            "prevKey"_attr = prevIndexKeyStringValue.toString());
            }
            throw;
        }
    }

    if (results && _indexConsistency->getMultikeyMetadataPathCount(&indexInfo) > 0) {
        results->errors.push_back(str::stream()
                                  << "Index '" << descriptor->indexName()
                                  << "' has one or more missing multikey metadata index keys");
        results->valid = false;
    }

    // Adjust multikey metadata when allowed. These states are all allowed by the design of
    // multikey. A collection should still be valid without these adjustments.
    if (_validateState->adjustMultikey()) {

        // If this collection has documents that make this index multikey, then check whether those
        // multikey paths match the index's metadata.
        auto indexPaths = index->getMultikeyPaths(opCtx, _validateState->getCollection());
        auto& documentPaths = indexInfo.docMultikeyPaths;
        if (indexInfo.multikeyDocs && documentPaths != indexPaths) {
            LOGV2(5367500,
                  "Index's multikey paths do not match those of its documents",
                  "index"_attr = descriptor->indexName(),
                  "indexPaths"_attr = MultikeyPathTracker::dumpMultikeyPaths(indexPaths),
                  "documentPaths"_attr = MultikeyPathTracker::dumpMultikeyPaths(documentPaths));

            // Since we have the correct multikey path information for this index, we can tighten up
            // its metadata to improve query performance. This may apply in two distinct scenarios:
            // 1. Collection data has changed such that the current multikey paths on the index
            // are too permissive and certain document paths are no longer multikey.
            // 2. This index was built before 3.4, and there is no multikey path information for
            // the index. We can effectively 'upgrade' the index so that it does not need to be
            // rebuilt to update this information.
            writeConflictRetry(opCtx, "updateMultikeyPaths", _validateState->nss().ns(), [&]() {
                WriteUnitOfWork wuow(opCtx);
                auto writeableIndex = const_cast<IndexCatalogEntry*>(index);
                const bool isMultikey = true;
                writeableIndex->forceSetMultikey(
                    opCtx, _validateState->getCollection(), isMultikey, documentPaths);
                wuow.commit();
            });

            if (results) {
                results->warnings.push_back(str::stream() << "Updated index multikey metadata"
                                                          << ": " << descriptor->indexName());
                results->repaired = true;
            }
        }

        // If this index does not need to be multikey, then unset the flag.
        if (index->isMultikey(opCtx, _validateState->getCollection()) && !indexInfo.multikeyDocs) {
            invariant(!indexInfo.docMultikeyPaths.size());

            LOGV2(5367501,
                  "Index is multikey but there are no multikey documents",
                  "index"_attr = descriptor->indexName());

            // This makes an improvement in the case that no documents make the index multikey and
            // the flag can be unset entirely. This may be due to a change in the data or historical
            // multikey bugs that have persisted incorrect multikey infomation.
            writeConflictRetry(opCtx, "unsetMultikeyPaths", _validateState->nss().ns(), [&]() {
                WriteUnitOfWork wuow(opCtx);
                auto writeableIndex = const_cast<IndexCatalogEntry*>(index);
                const bool isMultikey = false;
                writeableIndex->forceSetMultikey(
                    opCtx, _validateState->getCollection(), isMultikey, {});
                wuow.commit();
            });

            if (results) {
                results->warnings.push_back(str::stream() << "Unset index multikey metadata"
                                                          << ": " << descriptor->indexName());
                results->repaired = true;
            }
        }
    }

    if (numTraversedKeys) {
        *numTraversedKeys = numKeys;
    }
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
        _progress->finished();
    });

    RecordId prevRecordId;

    // In case validation occurs twice and the progress meter persists after index traversal
    if (_progress.get() && _progress->isActive()) {
        _progress->finished();
    }

    // Because the progress meter is intended as an approximation, it's sufficient to get the number
    // of records when we begin traversing, even if this number may deviate from the final number.
    const auto& coll = _validateState->getCollection();
    const char* curopMessage = "Validate: scanning documents";
    const auto totalRecords = coll->getRecordStore()->numRecords(opCtx);
    const auto rs = coll->getRecordStore();
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        _progress.set(CurOp::get(opCtx)->setProgress_inlock(curopMessage, totalRecords));
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
        _progress->hit();
        ++_numRecords;
        auto dataSize = record->data.size();
        interruptIntervalNumBytes += dataSize;
        dataSizeTotal += dataSize;
        size_t validatedSize = 0;
        Status status = validateRecord(
            opCtx, record->id, record->data, &nNonCompliantDocuments, &validatedSize, results);

        // RecordStores are required to return records in RecordId order.
        if (prevRecordId.isValid()) {
            invariant(prevRecordId < record->id);
        }

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
            } else if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
                       feature_flags::gExtendValidateCommand.isEnabled(
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

        if (_numRecords % kInterruptIntervalNumRecords == 0 ||
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
        results->errors.push_back(
            str::stream() << "fast count (" << fastCount << ") does not match number of records ("
                          << _numRecords << ") for collection '" << coll->ns() << "'");
        results->valid = false;
    }

    // Do not update the record store stats if we're in the background as we've validated a
    // checkpoint and it may not have the most up-to-date changes.
    if (results->valid && !_validateState->isBackground()) {
        coll->getRecordStore()->updateStatsAfterRepair(opCtx, _numRecords, dataSizeTotal);
    }
}

void ValidateAdaptor::validateIndexKeyCount(OperationContext* opCtx,
                                            const IndexCatalogEntry* index,
                                            IndexValidateResults& results) {
    // Fetch the total number of index entries we previously found traversing the index.
    const IndexDescriptor* desc = index->descriptor();
    const std::string indexName = desc->indexName();
    IndexInfo* indexInfo = &_indexConsistency->getIndexInfo(indexName);
    auto numTotalKeys = indexInfo->numKeys;

    // Update numRecords by subtracting number of records removed from record store in repair mode
    // when validating index consistency
    _numRecords -= results.keysRemovedFromRecordStore;

    // Do not fail on finding too few index entries compared to collection entries when full:false.
    bool hasTooFewKeys = false;
    bool noErrorOnTooFewKeys = !_validateState->isFullIndexValidation();

    if (desc->isIdIndex() && numTotalKeys != _numRecords) {
        hasTooFewKeys = (numTotalKeys < _numRecords);
        std::string msg = str::stream()
            << "number of _id index entries (" << numTotalKeys
            << ") does not match the number of documents in the index (" << _numRecords << ")";
        if (noErrorOnTooFewKeys && (numTotalKeys < _numRecords)) {
            results.warnings.push_back(msg);
        } else {
            results.errors.push_back(msg);
            results.valid = false;
        }
    }

    // Hashed indexes may never be multikey.
    if (desc->getAccessMethodName() == IndexNames::HASHED &&
        index->isMultikey(opCtx, _validateState->getCollection())) {
        results.errors.push_back(str::stream() << "Hashed index is incorrectly marked multikey: "
                                               << desc->indexName());
        results.valid = false;
    }

    // Confirm that the number of index entries is not greater than the number of documents in the
    // collection. This check is only valid for indexes that are not multikey (indexed arrays
    // produce an index key per array entry) and not $** indexes which can produce index keys for
    // multiple paths within a single document.
    if (results.valid && !index->isMultikey(opCtx, _validateState->getCollection()) &&
        desc->getIndexType() != IndexType::INDEX_WILDCARD && numTotalKeys > _numRecords) {
        std::string err = str::stream()
            << "index " << desc->indexName() << " is not multi-key, but has more entries ("
            << numTotalKeys << ") than documents in the index (" << _numRecords << ")";
        results.errors.push_back(err);
        results.valid = false;
    }

    // Ignore any indexes with a special access method. If an access method name is given, the
    // index may be a full text, geo or special index plugin with different semantics.
    if (results.valid && !desc->isSparse() && !desc->isPartial() && !desc->isIdIndex() &&
        desc->getAccessMethodName() == "" && numTotalKeys < _numRecords) {
        hasTooFewKeys = true;
        std::string msg = str::stream()
            << "index " << desc->indexName() << " is not sparse or partial, but has fewer entries ("
            << numTotalKeys << ") than documents in the index (" << _numRecords << ")";
        if (noErrorOnTooFewKeys) {
            results.warnings.push_back(msg);
        } else {
            results.errors.push_back(msg);
            results.valid = false;
        }
    }

    if (!_validateState->isFullIndexValidation() && hasTooFewKeys) {
        std::string warning = str::stream()
            << "index " << desc->indexName() << " has fewer keys than records."
            << " Please re-run the validate command with {full: true}";
        results.warnings.push_back(warning);
    }
}
}  // namespace mongo
