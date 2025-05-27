/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils_internal.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/bucket_compression_failure.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/write_ops/measurement.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/tracking/context.h"

#include <boost/iterator/transform_iterator.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries::write_ops_utils {

namespace {

MONGO_FAIL_POINT_DEFINE(timeseriesDataIntegrityCheckFailureUpdate);

// Return a verifierFunction that is used to perform a data integrity check on inserts into
// a compressed column.
doc_diff::VerifierFunc makeVerifierFunction(std::shared_ptr<bucket_catalog::WriteBatch> batch,
                                            OperationSource source) {
    return [measurements = batch->measurements, batch, source](const BSONObj& docToWrite,
                                                               const BSONObj& pre) {
        timeseriesDataIntegrityCheckFailureUpdate.executeIf(
            [&](const BSONObj&) {
                uasserted(  // In testing, we want any failures within this check to invariant.
                            // In production,
                    timeseries::BucketCompressionFailure(batch->bucketId.collectionUUID,
                                                         batch->bucketId.oid,
                                                         batch->bucketId.keySignature),
                    "Failpoint-triggered data integrity check failure");
            },
            [&source](const BSONObj&) { return source == OperationSource::kTimeseriesUpdate; });

        using AddAttrsFn = std::function<void(logv2::DynamicAttributes&)>;
        auto failed = [&measurements, &batch, &docToWrite, &pre](StringData reason,
                                                                 AddAttrsFn addAttrsWithoutData,
                                                                 AddAttrsFn addAttrsWithData) {
            logv2::DynamicAttributes attrs;
            attrs.add("reason", reason);
            attrs.add("bucketId", batch->bucketId.oid);
            attrs.add("collectionUUID", batch->bucketId.collectionUUID);
            addAttrsWithoutData(attrs);

            LOGV2_WARNING(
                8807500, "Failed data verification inserting into compressed column", attrs);

            attrs = {};
            auto seqLogDataFields = [](const BSONObj& measurement) {
                return logv2::seqLog(measurement);
            };
            auto measurementsAttr = logv2::seqLog(
                boost::make_transform_iterator(measurements.begin(), seqLogDataFields),
                boost::make_transform_iterator(measurements.end(), seqLogDataFields));
            attrs.add("measurements", measurementsAttr);
            auto preAttr = base64::encode(pre.objdata(), pre.objsize());
            attrs.add("pre", preAttr);
            auto bucketAttr = base64::encode(docToWrite.objdata(), docToWrite.objsize());
            attrs.add("bucket", bucketAttr);
            addAttrsWithData(attrs);

            LOGV2_WARNING_OPTIONS(8807501,
                                  logv2::LogTruncation::Disabled,
                                  "Failed data verification inserting into compressed column",
                                  attrs);

            invariant(!TestingProctor::instance().isEnabled());
            tasserted(timeseries::BucketCompressionFailure(batch->bucketId.collectionUUID,
                                                           batch->bucketId.oid,
                                                           batch->bucketId.keySignature),
                      "Failed data verification inserting into compressed column");
        };

        auto actualMeta = docToWrite.getField(kBucketMetaFieldName);
        auto expectedMeta = batch->bucketKey.metadata.element();
        if (!actualMeta.binaryEqualValues(expectedMeta)) {
            failed(
                "mismatched metaField value",
                [](logv2::DynamicAttributes&) {},
                [&actualMeta, &expectedMeta](logv2::DynamicAttributes& attrs) {
                    attrs.add("expectedMetaFieldValue", expectedMeta);
                    attrs.add("actualMetaFieldValue", actualMeta);
                });
        }

        auto data = docToWrite.getObjectField(kBucketDataFieldName);

        // Iterate through each key-value pair in the data. This is a mapping from String keys
        // representing data field names to a tuple of an iterator on a BSONColumn, the column
        // itself, and a size_t type counter. The iterator allows us to iterate across the
        // BSONColumn as we inspect each element and compare it to the expected value in the actual
        // measurement we inserted. The column is stored to prevent the iterator on it from going
        // out of scope. The StringData representation of the binary allows us to log the binary in
        // the case that we encounter an error. The size_t counter represents how many times the
        // iterator has been advanced - this allows us to detect when we didn't have a value set for
        // a field in a particular measurement, so that we can check the corresponding BSONColumn
        // for a skip value.
        StringDataMap<std::tuple<BSONColumn::Iterator, BSONColumn, StringData, size_t>>
            fieldsToDataAndNextCountMap;

        // First, populate our map.
        for (auto&& [key, columnValue] : data) {
            int binLength = 0;
            const char* binData = columnValue.binData(binLength);
            // Decompress the binary data for this field.
            try {
                BSONColumn c(binData, binLength);
                auto it = c.begin();
                std::advance(it, batch->numPreviouslyCommittedMeasurements);
                fieldsToDataAndNextCountMap.emplace(
                    key, std::make_tuple(it, std::move(c), StringData(binData, binLength), 0));
            } catch (const DBException& e) {
                failed(
                    "exception",
                    [&e](logv2::DynamicAttributes& attrs) { attrs.add("error", e); },
                    [&binData, binLength](logv2::DynamicAttributes& attrs) {
                        auto columnAttr = base64::encode(binData, binLength);
                        attrs.add("column", columnAttr);
                    });
            };
        }
        // Now, iterate through each measurement, and check if the value for each field of the
        // measurement matches with what was compressed. numIterations will keep track of how many
        // measurements we've gone through so far - this is useful because not every measurement
        // necessarily has a value for each field, as this counter allows us to identify when
        // a measurement skipped a value for a field.
        size_t numIterations = 0;
        for (const auto& measurement : measurements) {
            for (const BSONElement& elem : measurement) {
                auto measurementKey = elem.fieldNameStringData();
                // Skip the meta field values because they are not compressed in a BSONColumn, but
                // rather stored uncompressed once in the Bucket.
                if (measurementKey == batch->bucketKey.metadata.getMetaField()) {
                    continue;
                }
                if (!fieldsToDataAndNextCountMap.contains(measurementKey)) {
                    failed(
                        "missing column",
                        [](logv2::DynamicAttributes& attrs) {},
                        [measurementKey](logv2::DynamicAttributes& attrs) {
                            attrs.add("key", measurementKey);
                        });
                }
                auto& [iterator, column, binaryString, nextCount] =
                    fieldsToDataAndNextCountMap.at(measurementKey);
                if (!iterator->binaryEqualValues(elem)) {
                    failed(
                        "value not matching expected",
                        [](logv2::DynamicAttributes& attrs) {},
                        [binaryString = binaryString, measurementKey, &iterator = iterator, elem](
                            logv2::DynamicAttributes& attrs) {
                            auto columnAttr = base64::encode(binaryString);
                            attrs.add("column", columnAttr);
                            attrs.add("key", measurementKey);
                            attrs.add("value", *iterator);
                            attrs.add("expected", elem);
                        });
                }
                ++nextCount;
                ++iterator;
            }
            ++numIterations;
            // Advance the iterators for fields not present in this measurement.
            for (auto& [key, value] : fieldsToDataAndNextCountMap) {
                auto& [iterator, column, binaryString, nextCount] = value;
                // If a value was not present for a field in a measurement, check that the
                // corresponding element in the column is a skip value.
                if (nextCount < numIterations) {
                    invariant(nextCount == numIterations - 1);
                    if (!iterator->eoo()) {
                        failed(
                            "value unexpectedly not EOO",
                            [](logv2::DynamicAttributes& attrs) {},
                            [binaryString = binaryString, key = key, &iterator = iterator](
                                logv2::DynamicAttributes& attrs) {
                                auto columnAttr = base64::encode(binaryString);
                                attrs.add("column", columnAttr);
                                attrs.add("key", key);
                                attrs.add("value", *iterator);
                            });
                    }
                    ++iterator;
                    ++nextCount;
                }
            }
        }
    };
};

// Builds a complete and new bucket document.
write_ops_utils::BucketDocument makeNewDocument(const OID& bucketId,
                                                const BSONObj& metadata,
                                                const BSONObj& min,
                                                const BSONObj& max,
                                                StringDataMap<BSONObjBuilder>& dataBuilders,
                                                StringData timeField,
                                                const NamespaceString& nss,
                                                const UUID& collectionUUID,
                                                std::uint32_t keySignature) {
    auto metadataElem = metadata.firstElement();
    BSONObjBuilder builder;
    builder.append("_id", bucketId);
    {
        BSONObjBuilder bucketControlBuilder(builder.subobjStart("control"));
        bucketControlBuilder.append(kBucketControlVersionFieldName,
                                    kTimeseriesControlUncompressedVersion);
        bucketControlBuilder.append(kBucketControlMinFieldName, min);
        bucketControlBuilder.append(kBucketControlMaxFieldName, max);
    }
    if (metadataElem) {
        builder.appendAs(metadataElem, kBucketMetaFieldName);
    }
    {
        BSONObjBuilder bucketDataBuilder(builder.subobjStart(kBucketDataFieldName));
        for (auto& dataBuilder : dataBuilders) {
            bucketDataBuilder.append(dataBuilder.first, dataBuilder.second.obj());
        }
    }

    write_ops_utils::BucketDocument bucketDoc{builder.obj()};

    const bool validateCompression = gValidateTimeseriesCompression.load();
    auto compressed = timeseries::compressBucket(
        bucketDoc.uncompressedBucket, timeField, nss, validateCompression);
    uassert(BucketCompressionFailure(collectionUUID, bucketId, keySignature),
            "Failed to compress time-series bucket",
            compressed.compressedBucket);
    bucketDoc.compressedBucket = std::move(*compressed.compressedBucket);

    return bucketDoc;
}

/**
 * Builds the data field of a bucket document. Computes the min and max fields if necessary. If a
 * minTime is passed in, this means that we want to preserve the currentMinTime in the updated
 * bucket; we will update all min and max values to reflect the new data except for the minTime in
 * this case, which will remain unchanged.
 */
boost::optional<std::pair<BSONObj, BSONObj>> processTimeseriesMeasurements(
    const std::vector<BSONObj>& measurements,
    const BSONObj& metadata,
    StringDataMap<BSONObjBuilder>& dataBuilders,
    const boost::optional<TimeseriesOptions>& options = boost::none,
    const boost::optional<const StringDataComparator*>& comparator = boost::none,
    const boost::optional<Date_t> currentMinTime = boost::none) {
    tracking::Context trackingContext;
    bucket_catalog::MinMax minmax{trackingContext};
    bool computeMinmax = options && comparator;

    auto metadataElem = metadata.firstElement();
    boost::optional<StringData> metaFieldName;
    if (metadataElem) {
        metaFieldName = metadataElem.fieldNameStringData();
    }

    DecimalCounter<uint32_t> count;
    for (const auto& doc : measurements) {
        if (computeMinmax) {
            minmax.update(doc, metaFieldName, *comparator);
        }
        for (const auto& elem : doc) {
            auto key = elem.fieldNameStringData();
            if (key == metaFieldName) {
                continue;
            }
            dataBuilders[key].appendAs(elem, count);
        }
        ++count;
    }

    // Rounds the minimum timestamp and updates the min time field.
    if (computeMinmax) {
        auto minTime = (currentMinTime != boost::none)
            ? currentMinTime.get()
            : roundTimestampToGranularity(minmax.min().getField(options->getTimeField()).Date(),
                                          *options);
        auto controlDoc =
            bucket_catalog::buildControlMinTimestampDoc(options->getTimeField(), minTime);
        minmax.update(controlDoc, /*metaField=*/boost::none, *comparator);
        return {{minmax.min(), minmax.max()}};
    }

    return boost::none;
}
}  // namespace


BucketDocument makeNewDocumentForWrite(const NamespaceString& nss,
                                       std::shared_ptr<bucket_catalog::WriteBatch> batch,
                                       const BSONObj& metadata) {
    StringDataMap<BSONObjBuilder> dataBuilders;
    processTimeseriesMeasurements(
        {batch->measurements.begin(), batch->measurements.end()}, metadata, dataBuilders);

    return makeNewDocument(batch->bucketId.oid,
                           metadata,
                           batch->min,
                           batch->max,
                           dataBuilders,
                           batch->timeField,
                           nss,
                           batch->bucketId.collectionUUID,
                           batch->bucketId.keySignature);
}

BucketDocument makeNewDocumentForWrite(
    const NamespaceString& nss,
    const UUID& collectionUUID,
    const OID& bucketId,
    const std::vector<BSONObj>& measurements,
    const BSONObj& metadata,
    const TimeseriesOptions& options,
    const boost::optional<const StringDataComparator*>& comparator,
    const boost::optional<Date_t>& currentMinTime) {
    StringDataMap<BSONObjBuilder> dataBuilders;
    auto minmax = processTimeseriesMeasurements(
        measurements, metadata, dataBuilders, options, comparator, currentMinTime);

    invariant(minmax);

    return makeNewDocument(bucketId,
                           metadata,
                           minmax->first,
                           minmax->second,
                           dataBuilders,
                           options.getTimeField(),
                           nss,
                           collectionUUID,
                           bucket_catalog::getKeySignature(options, collectionUUID, metadata));
}

BSONObj makeBSONColumnDocDiff(
    const BSONColumnBuilder<tracking::Allocator<void>>::BinaryDiff& binaryDiff) {
    return BSON(
        "o" << binaryDiff.offset() << "d"
            << BSONBinData(binaryDiff.data(), binaryDiff.size(), BinDataType::BinDataGeneral));
}

BSONObj makeTimeseriesInsertCompressedBucketDocument(
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    const std::vector<
        std::pair<StringData, BSONColumnBuilder<tracking::Allocator<void>>::BinaryDiff>>&
        intermediates) {
    BSONObjBuilder insertBuilder;
    insertBuilder.append(kBucketIdFieldName, batch->bucketId.oid);

    {
        BSONObjBuilder bucketControlBuilder(insertBuilder.subobjStart(kBucketControlFieldName));
        bucketControlBuilder.append(kBucketControlVersionFieldName,
                                    kTimeseriesControlCompressedSortedVersion);
        bucketControlBuilder.append(kBucketControlMinFieldName, batch->min);
        bucketControlBuilder.append(kBucketControlMaxFieldName, batch->max);
        bucketControlBuilder.append(kBucketControlCountFieldName,
                                    static_cast<int32_t>(batch->measurements.size()));
    }

    auto metadataElem = batch->bucketKey.metadata.element();
    if (metadataElem) {
        insertBuilder.appendAs(metadataElem, kBucketMetaFieldName);
    }

    {
        BSONObjBuilder dataBuilder(insertBuilder.subobjStart(kBucketDataFieldName));
        for (const auto& [fieldName, binData] : intermediates) {
            invariant(binData.offset() == 0,
                      "Intermediate must be called exactly once prior to insert.");
            auto columnBinary = BSONBinData(binData.data(), binData.size(), BinDataType::Column);
            dataBuilder.append(fieldName, columnBinary);
        }
    }

    return insertBuilder.obj();
}

mongo::write_ops::WriteCommandRequestBase makeTimeseriesWriteOpBase(std::vector<StmtId>&& stmtIds) {
    mongo::write_ops::WriteCommandRequestBase base;

    // The schema validation configured in the bucket collection is intended for direct
    // operations by end users and is not applicable here.
    base.setBypassDocumentValidation(true);

    if (!stmtIds.empty()) {
        base.setStmtIds(std::move(stmtIds));
    }

    return base;
}

void isMeasurementsSortedOnTime(std::shared_ptr<bucket_catalog::WriteBatch> batch) {
    std::vector<BSONElement> measurementTimestamps;
    for (const auto& measurement : batch->measurements) {
        measurementTimestamps.push_back(measurement[batch->timeField]);
    }
    bool isSorted = std::is_sorted(
        measurementTimestamps.begin(),
        measurementTimestamps.end(),
        [](const BSONElement& lhs, const BSONElement& rhs) { return lhs.date() < rhs.date(); });
    invariant(isSorted);
}

void makeWriteRequestFromBatch(OperationContext* opCtx,
                               std::shared_ptr<bucket_catalog::WriteBatch> batch,
                               const NamespaceString& bucketsNs,
                               std::vector<mongo::write_ops::InsertCommandRequest>* insertOps,
                               std::vector<mongo::write_ops::UpdateCommandRequest>* updateOps) {
    if (batch->numPreviouslyCommittedMeasurements == 0) {
        insertOps->push_back(makeTimeseriesInsertOpFromBatch(batch, bucketsNs));
        return;
    }
    updateOps->push_back(makeTimeseriesCompressedDiffUpdateOpFromBatch(opCtx, batch, bucketsNs));
}

mongo::write_ops::InsertCommandRequest makeTimeseriesInsertOpFromBatch(
    std::shared_ptr<bucket_catalog::WriteBatch> batch, const NamespaceString& bucketsNs) {
    invariant(!batch->isReopened);
    BSONObj bucketToInsert;
    BucketDocument bucketDoc;
    // Invariant that the measurements are sorted from buildBatchedInsertContexts.
    if constexpr (kDebugBuild) {
        isMeasurementsSortedOnTime(batch);
    }
    auto metaField = batch->bucketKey.metadata.getMetaField();
    // Insert measurements, and appropriate skips, into all column builders.
    for (const auto& measurement : batch->measurements) {
        batch->measurementMap.insertOne(measurement, metaField);
    }
    int32_t compressedSizeDelta;
    auto intermediates = batch->measurementMap.intermediate(compressedSizeDelta);
    batch->sizes.uncommittedVerifiedSize = compressedSizeDelta;
    bucketToInsert = makeTimeseriesInsertCompressedBucketDocument(batch, intermediates);

    // Extra verification that the insert op decompresses to the same values put in.
    if (gPerformTimeseriesCompressionIntermediateDataIntegrityCheckOnInsert.load()) {
        auto verifierFunction = makeVerifierFunction(batch, OperationSource::kTimeseriesInsert);
        verifierFunction(bucketToInsert, BSONObj());
    }

    mongo::write_ops::InsertCommandRequest op{bucketsNs, {bucketToInsert}};
    op.setWriteCommandRequestBase(makeTimeseriesWriteOpBase(std::move(batch->stmtIds)));
    op.setCollectionUUID(batch->bucketId.collectionUUID);
    return op;
}

std::variant<mongo::write_ops::UpdateCommandRequest, mongo::write_ops::DeleteCommandRequest>
makeModificationOp(const OID& bucketId,
                   const CollectionPtr& coll,
                   const std::vector<BSONObj>& measurements,
                   const boost::optional<Date_t>& currentMinTime) {
    // A bucket will be fully deleted if no measurements are passed in.
    if (measurements.empty()) {
        mongo::write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId), false);
        mongo::write_ops::DeleteCommandRequest op(coll->ns(), {deleteEntry});
        return op;
    }
    auto timeseriesOptions = coll->getTimeseriesOptions();
    invariant(timeseriesOptions);

    auto metaFieldName = timeseriesOptions->getMetaField();
    auto metadata = [&] {
        if (!metaFieldName) {  // Collection has no metadata field.
            return BSONObj();
        }
        // Look for the metadata field on this bucket and return it if present.
        auto metaField = measurements[0].getField(*metaFieldName);
        return metaField ? metaField.wrap() : BSONObj();
    }();

    BucketDocument bucketDoc = makeNewDocumentForWrite(coll->ns(),
                                                       coll->uuid(),
                                                       bucketId,
                                                       measurements,
                                                       metadata,
                                                       *timeseriesOptions,
                                                       coll->getDefaultCollator(),
                                                       currentMinTime);

    invariant(bucketDoc.compressedBucket);
    BSONObj bucketToReplace = *bucketDoc.compressedBucket;

    mongo::write_ops::UpdateModification u(bucketToReplace);
    mongo::write_ops::UpdateOpEntry updateEntry(BSON("_id" << bucketId), std::move(u));
    mongo::write_ops::UpdateCommandRequest op(coll->ns(), {updateEntry});
    return op;
}

mongo::write_ops::UpdateCommandRequest makeTimeseriesCompressedDiffUpdateOpFromBatch(
    OperationContext* opCtx,
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    const NamespaceString& bucketsNs) {
    invariant(batch->measurements.size() > 0);
    auto firstMeasurementTimestamp = batch->measurements[0][batch->timeField].timestamp();
    bool changedToUnsorted = false;
    if (batch->bucketIsSortedByTime &&
        firstMeasurementTimestamp < batch->measurementMap.timeOfLastMeasurement(batch->timeField)) {
        batch->bucketIsSortedByTime = false;
        changedToUnsorted = true;
        batch->stats.incNumCompressedBucketsConvertedToUnsorted();
    }
    // Invariant that the measurements are sorted from buildBatchedInsertContexts.
    if constexpr (kDebugBuild) {
        isMeasurementsSortedOnTime(batch);
    }
    auto metaField = batch->bucketKey.metadata.getMetaField();
    // Insert new measurements, and appropriate skips, into all column builders.
    for (const auto& measurement : batch->measurements) {
        batch->measurementMap.insertOne(measurement, metaField);
    }
    // Generates a delta update request using the before and after compressed bucket documents' data
    // fields. The only other items that will be different are the min, max, and count fields in the
    // control block, and the version field if it was promoted to a v3 bucket.
    const auto updateEntry = makeTimeseriesCompressedDiffEntry(opCtx, batch, changedToUnsorted);
    mongo::write_ops::UpdateCommandRequest op(bucketsNs, {updateEntry});
    op.setWriteCommandRequestBase(makeTimeseriesWriteOpBase(std::move(batch->stmtIds)));
    op.setCollectionUUID(batch->bucketId.collectionUUID);
    return op;
}

mongo::write_ops::UpdateOpEntry makeTimeseriesCompressedDiffEntry(
    OperationContext* opCtx,
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    bool changedToUnsorted) {
    // Verifier function that will be called when we apply the diff to our bucket and verify that
    // the measurements we inserted appear correctly in the resulting bucket's BSONColumns.
    doc_diff::VerifierFunc verifierFunction = nullptr;
    if (gPerformTimeseriesCompressionIntermediateDataIntegrityCheckOnInsert.load() ||
        (gPerformTimeseriesCompressionIntermediateDataIntegrityCheckOnReopening.load() &&
         batch->isReopened)) {
        verifierFunction = makeVerifierFunction(batch, OperationSource::kTimeseriesUpdate);
    }

    BSONObjBuilder updateBuilder;
    {
        // Control builder.
        BSONObjBuilder controlBuilder(updateBuilder.subobjStart(kControlFieldNameDocDiff));
        BSONObjBuilder countAndVersionBuilder;
        countAndVersionBuilder.append(kBucketControlCountFieldName,
                                      static_cast<int>((batch->numPreviouslyCommittedMeasurements +
                                                        batch->measurements.size())));
        if (changedToUnsorted) {
            countAndVersionBuilder.append(kBucketControlVersionFieldName,
                                          kTimeseriesControlCompressedUnsortedVersion);
        }
        controlBuilder.append(doc_diff::kUpdateSectionFieldName, countAndVersionBuilder.obj());

        if (!batch->min.isEmpty() || !batch->max.isEmpty()) {
            if (!batch->min.isEmpty()) {
                controlBuilder.append(kMinFieldNameDocDiff, batch->min);
            }
            if (!batch->max.isEmpty()) {
                controlBuilder.append(kMaxFieldNameDocDiff, batch->max);
            }
        }
    }

    {
        // Data builder.

        BSONObjBuilder dataBuilder(updateBuilder.subobjStart(kDataFieldNameDocDiff));
        BSONObjBuilder newDataFieldsBuilder;
        BSONObjBuilder updatedDataFieldsBuilder;

        int32_t compressedSizeDelta;
        auto intermediates = batch->measurementMap.intermediate(compressedSizeDelta);
        batch->sizes.uncommittedVerifiedSize = compressedSizeDelta;

        for (const auto& [fieldName, diff] : intermediates) {
            if (batch->newFieldNamesToBeInserted.count(fieldName)) {
                // Insert new column.
                invariant(diff.offset() == 0);
                auto binary = BSONBinData(diff.data(), diff.size(), BinDataType::Column);
                newDataFieldsBuilder.append(fieldName, binary);
            } else {
                // Update existing column.
                BSONObj binaryObj = makeBSONColumnDocDiff(diff);
                updatedDataFieldsBuilder.append(fieldName, binaryObj);
            }
        }

        auto newDataFields = newDataFieldsBuilder.obj();
        if (!newDataFields.isEmpty()) {
            dataBuilder.append(doc_diff::kInsertSectionFieldName, newDataFields);
        }

        auto updatedDataFields = updatedDataFieldsBuilder.obj();
        if (!updatedDataFields.isEmpty()) {
            dataBuilder.append(doc_diff::kBinarySectionFieldName, updatedDataFields);
        }
    }

    mongo::write_ops::UpdateModification::DiffOptions options;
    mongo::write_ops::UpdateModification u(
        updateBuilder.obj(), mongo::write_ops::UpdateModification::DeltaTag{}, options);
    u.verifierFunction = std::move(verifierFunction);
    auto oid = batch->bucketId.oid;
    mongo::write_ops::UpdateOpEntry update(BSON("_id" << oid), std::move(u));
    invariant(!update.getMulti(), oid.toString());
    invariant(!update.getUpsert(), oid.toString());
    return update;
}

mongo::write_ops::UpdateCommandRequest makeTimeseriesTransformationOp(
    OperationContext* opCtx,
    const OID& bucketId,
    mongo::write_ops::UpdateModification::TransformFunc transformationFunc,
    const mongo::write_ops::InsertCommandRequest& request) {
    mongo::write_ops::UpdateCommandRequest op(
        makeTimeseriesBucketsNamespace(request.getNamespace()),
        {makeTimeseriesTransformationOpEntry(opCtx, bucketId, std::move(transformationFunc))});

    mongo::write_ops::WriteCommandRequestBase base;
    // The schema validation configured in the bucket collection is intended for direct
    // operations by end users and is not applicable here.
    base.setBypassDocumentValidation(true);

    base.setBypassEmptyTsReplacement(request.getBypassEmptyTsReplacement());

    // Timeseries compression operation is not a user operation and should not use a
    // statement id from any user op. Set to Uninitialized to bypass.
    base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});

    op.setWriteCommandRequestBase(std::move(base));
    return op;
}

mongo::write_ops::UpdateOpEntry makeTimeseriesTransformationOpEntry(
    OperationContext* opCtx,
    const OID& bucketId,
    mongo::write_ops::UpdateModification::TransformFunc transformationFunc) {
    mongo::write_ops::UpdateModification u(std::move(transformationFunc));
    mongo::write_ops::UpdateOpEntry update(BSON("_id" << bucketId), std::move(u));
    invariant(!update.getMulti(), bucketId.toString());
    invariant(!update.getUpsert(), bucketId.toString());
    return update;
}

}  // namespace mongo::timeseries::write_ops_utils
