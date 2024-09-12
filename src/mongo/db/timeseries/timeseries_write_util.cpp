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


#include "mongo/db/timeseries/timeseries_write_util.h"

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_exec_util.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/bucket_catalog/measurement_map.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/bucket_compression_failure.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/decorable.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo::timeseries {
namespace {

MONGO_FAIL_POINT_DEFINE(timeseriesDataIntegrityCheckFailureUpdate);

// Return a verifierFunction that is used to perform a data integrity check on inserts into
// a compressed column.
doc_diff::VerifierFunc makeVerifierFunction(std::vector<details::Measurement> sortedMeasurements,
                                            std::shared_ptr<bucket_catalog::WriteBatch> batch,
                                            OperationSource source) {
    return [sortedMeasurements = std::move(sortedMeasurements), batch, source](
               const BSONObj& docToWrite) {
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
        auto failed = [&sortedMeasurements, &batch, &docToWrite](StringData reason,
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
            auto seqLogDataFields = [](const details::Measurement& measurement) {
                return logv2::seqLog(measurement.dataFields);
            };
            auto measurementsAttr = logv2::seqLog(
                boost::make_transform_iterator(sortedMeasurements.begin(), seqLogDataFields),
                boost::make_transform_iterator(sortedMeasurements.end(), seqLogDataFields));
            attrs.add("measurements", measurementsAttr);
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
        for (const auto& measurement : sortedMeasurements) {
            for (const BSONElement& elem : measurement.dataFields) {
                auto measurementKey = elem.fieldNameStringData();
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

// Builds the data field of a bucket document. Computes the min and max fields if necessary.
boost::optional<std::pair<BSONObj, BSONObj>> processTimeseriesMeasurements(
    const std::vector<BSONObj>& measurements,
    const BSONObj& metadata,
    StringDataMap<BSONObjBuilder>& dataBuilders,
    const boost::optional<TimeseriesOptions>& options = boost::none,
    const boost::optional<const StringDataComparator*>& comparator = boost::none) {
    TrackingContext trackingContext;
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
        auto minTime = roundTimestampToGranularity(
            minmax.min().getField(options->getTimeField()).Date(), *options);
        auto controlDoc =
            bucket_catalog::buildControlMinTimestampDoc(options->getTimeField(), minTime);
        minmax.update(controlDoc, /*metaField=*/boost::none, *comparator);
        return {{minmax.min(), minmax.max()}};
    }

    return boost::none;
}

// Builds a complete and new bucket document.
BucketDocument makeNewDocument(const OID& bucketId,
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

    BucketDocument bucketDoc{builder.obj()};
    if (!feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return bucketDoc;
    }

    const bool validateCompression = gValidateTimeseriesCompression.load();
    auto compressed = timeseries::compressBucket(
        bucketDoc.uncompressedBucket, timeField, nss, validateCompression);
    uassert(BucketCompressionFailure(collectionUUID, bucketId, keySignature),
            "Failed to compress time-series bucket",
            compressed.compressedBucket);
    bucketDoc.compressedBucket = std::move(*compressed.compressedBucket);

    return bucketDoc;
}

// Makes a write command request base and sets the statement Ids if provided a non-empty vector.
write_ops::WriteCommandRequestBase makeTimeseriesWriteOpBase(std::vector<StmtId>&& stmtIds) {
    write_ops::WriteCommandRequestBase base;

    // The schema validation configured in the bucket collection is intended for direct
    // operations by end users and is not applicable here.
    base.setBypassDocumentValidation(true);

    if (!stmtIds.empty()) {
        base.setStmtIds(std::move(stmtIds));
    }

    return base;
}

/**
 * Generates the compressed diff using the BSONColumnBuilders stored in the batch and the
 * intermediate() interface.
 */
write_ops::UpdateOpEntry makeTimeseriesCompressedDiffEntry(
    OperationContext* opCtx,
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    bool changedToUnsorted,
    const std::vector<details::Measurement>& sortedMeasurements) {

    // Verifier function that will be called when we apply the diff to our bucket and verify that
    // the measurements we inserted appear correctly in the resulting bucket's BSONColumns.
    doc_diff::VerifierFunc verifierFunction = nullptr;
    if (gPerformTimeseriesCompressionIntermediateDataIntegrityCheckOnInsert.load() ||
        (gPerformTimeseriesCompressionIntermediateDataIntegrityCheckOnReopening.load() &&
         batch->isReopened)) {
        verifierFunction =
            makeVerifierFunction(sortedMeasurements, batch, OperationSource::kTimeseriesUpdate);
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

    write_ops::UpdateModification::DiffOptions options;
    options.mustCheckExistenceForInsertOperations =
        static_cast<bool>(repl::tenantMigrationInfo(opCtx));
    write_ops::UpdateModification u(
        updateBuilder.obj(), write_ops::UpdateModification::DeltaTag{}, options);
    u.verifierFunction = std::move(verifierFunction);
    auto oid = batch->bucketId.oid;
    write_ops::UpdateOpEntry update(BSON("_id" << oid), std::move(u));
    invariant(!update.getMulti(), oid.toString());
    invariant(!update.getUpsert(), oid.toString());
    return update;
}

// Builds the delta update oplog entry from a time-series insert write batch.
write_ops::UpdateOpEntry makeTimeseriesUpdateOpEntry(
    OperationContext* opCtx,
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    const BSONObj& metadata) {
    BSONObjBuilder updateBuilder;
    {
        if (!batch->min.isEmpty() || !batch->max.isEmpty()) {
            BSONObjBuilder controlBuilder(updateBuilder.subobjStart(kControlFieldNameDocDiff));
            if (!batch->min.isEmpty()) {
                controlBuilder.append(kMinFieldNameDocDiff, batch->min);
            }
            if (!batch->max.isEmpty()) {
                controlBuilder.append(kMaxFieldNameDocDiff, batch->max);
            }
        }
    }
    {  // doc_diff::kSubDiffSectionFieldPrefix + <field name> => {<index_0>: ..., <index_1>:}
        StringDataMap<BSONObjBuilder> dataFieldBuilders;
        auto metadataElem = metadata.firstElement();
        DecimalCounter<uint32_t> count(batch->numPreviouslyCommittedMeasurements);
        for (const auto& doc : batch->measurements) {
            for (const auto& elem : doc) {
                auto key = elem.fieldNameStringData();
                if (metadataElem && key == metadataElem.fieldNameStringData()) {
                    continue;
                }
                auto& builder = dataFieldBuilders[key];
                builder.appendAs(elem, count);
            }
            ++count;
        }

        BSONObjBuilder dataBuilder(updateBuilder.subobjStart(kDataFieldNameDocDiff));
        BSONObjBuilder newDataFieldsBuilder;
        for (auto& pair : dataFieldBuilders) {
            // Existing 'data' fields with measurements require different treatment from fields
            // not observed before (missing from control.min and control.max).
            if (batch->newFieldNamesToBeInserted.count(pair.first)) {
                newDataFieldsBuilder.append(pair.first, pair.second.obj());
            }
        }
        auto newDataFields = newDataFieldsBuilder.obj();
        if (!newDataFields.isEmpty()) {
            dataBuilder.append(doc_diff::kInsertSectionFieldName, newDataFields);
        }
        for (auto& pair : dataFieldBuilders) {
            // Existing 'data' fields with measurements require different treatment from fields
            // not observed before (missing from control.min and control.max).
            if (!batch->newFieldNamesToBeInserted.count(pair.first)) {
                dataBuilder.append(doc_diff::kSubDiffSectionFieldPrefix + pair.first.toString(),
                                   BSON(doc_diff::kInsertSectionFieldName << pair.second.obj()));
            }
        }
    }
    write_ops::UpdateModification::DiffOptions options;
    options.mustCheckExistenceForInsertOperations =
        static_cast<bool>(repl::tenantMigrationInfo(opCtx));
    write_ops::UpdateModification u(
        updateBuilder.obj(), write_ops::UpdateModification::DeltaTag{}, options);
    auto oid = batch->bucketId.oid;
    write_ops::UpdateOpEntry update(BSON("_id" << oid), std::move(u));
    invariant(!update.getMulti(), oid.toString());
    invariant(!update.getUpsert(), oid.toString());
    return update;
}

// Performs the storage write of an update to a time-series bucket document.
void updateTimeseriesDocument(OperationContext* opCtx,
                              const CollectionPtr& coll,
                              const write_ops::UpdateCommandRequest& op,
                              OpDebug* opDebug,
                              bool fromMigrate,
                              StmtId stmtId) {
    invariant(op.getUpdates().size() == 1);
    auto& update = op.getUpdates().front();

    invariant(coll->isClustered());
    auto recordId = record_id_helpers::keyForOID(update.getQ()["_id"].OID());

    auto original = coll->docFor(opCtx, recordId);

    CollectionUpdateArgs args{original.value()};
    args.criteria = update.getQ();
    args.stmtIds = {stmtId};
    if (fromMigrate) {
        args.source = OperationSource::kFromMigrate;
    }

    BSONObj updated;
    BSONObj diffFromUpdate;
    const BSONObj* diffOnIndexes =
        collection_internal::kUpdateAllIndexes;  // Assume all indexes are affected.
    if (update.getU().type() == write_ops::UpdateModification::Type::kDelta) {
        diffFromUpdate = update.getU().getDiff();
        updated = doc_diff::applyDiff(original.value(),
                                      diffFromUpdate,
                                      static_cast<bool>(repl::tenantMigrationInfo(opCtx)),
                                      update.getU().verifierFunction);
        diffOnIndexes = &diffFromUpdate;
        args.update = update_oplog_entry::makeDeltaOplogEntry(diffFromUpdate);
    } else if (update.getU().type() == write_ops::UpdateModification::Type::kTransform) {
        const auto& transform = update.getU().getTransform();
        auto transformed = transform(original.value());
        tassert(7667900,
                "Could not apply transformation to time series bucket document",
                transformed.has_value());
        updated = std::move(transformed.value());
        args.update = update_oplog_entry::makeReplacementOplogEntry(updated);
    } else if (update.getU().type() == write_ops::UpdateModification::Type::kReplacement) {
        updated = update.getU().getUpdateReplacement();
        args.update = update_oplog_entry::makeReplacementOplogEntry(updated);
    } else {
        invariant(false, "Unexpected update type");
    }

    collection_internal::updateDocument(opCtx,
                                        coll,
                                        recordId,
                                        original,
                                        updated,
                                        diffOnIndexes,
                                        nullptr /*indexesAffected*/,
                                        opDebug,
                                        &args);
}

std::shared_ptr<bucket_catalog::WriteBatch>& extractFromSelf(
    std::shared_ptr<bucket_catalog::WriteBatch>& batch) {
    return batch;
}

BSONObj getSuitableBucketForReopening(OperationContext* opCtx,
                                      const Collection* bucketsColl,
                                      const TimeseriesOptions& options,
                                      bucket_catalog::ReopeningContext& reopeningContext) {
    return visit(
        OverloadedVisitor{
            [](const std::monostate&) { return BSONObj{}; },
            [&](const OID& bucketId) {
                reopeningContext.fetchedBucket = true;
                return DBDirectClient{opCtx}.findOne(bucketsColl->ns(), BSON("_id" << bucketId));
            },
            [&](const std::vector<BSONObj>& pipeline) {
                // Ensure we have a index on meta and time for the time-series collection before
                // performing the query. Without the index we will perform a full collection scan
                // which could cause us to take a performance hit.
                if (auto index = getIndexSupportingReopeningQuery(
                        opCtx, bucketsColl->getIndexCatalog(), options)) {
                    // Resort to Query-Based reopening approach.
                    reopeningContext.queriedBucket = true;
                    DBDirectClient client{opCtx};

                    // Run an aggregation to find a suitable bucket to reopen.
                    AggregateCommandRequest aggRequest(bucketsColl->ns(), pipeline);
                    aggRequest.setHint(index);

                    // TODO SERVER-86094: remove after fixing perf regression.
                    query_settings::QuerySettings querySettings;
                    querySettings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);
                    aggRequest.setQuerySettings(querySettings);

                    auto swCursor = DBClientCursor::fromAggregationRequest(
                        &client, aggRequest, false /* secondaryOk */, false /* useExhaust */);
                    if (swCursor.isOK() && swCursor.getValue()->more()) {
                        return swCursor.getValue()->next();
                    }
                }
                return BSONObj{};
            },
        },
        reopeningContext.candidate);
}

StatusWith<bucket_catalog::InsertResult> attemptInsertIntoBucketWithReopening(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& bucketCatalog,
    const Collection* bucketsColl,
    const TimeseriesOptions& options,
    const BSONObj& measurementDoc,
    bucket_catalog::CombineWithInsertsFromOtherClients combine,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    bucket_catalog::InsertContext& insertContext,
    const Date_t& time) {
    boost::optional<bucket_catalog::BucketId> uncompressedBucketId{boost::none};
    // The purpose of this scope is to destroy the ReopeningContext for the
    // compress-and-write-uncompressed-bucket scenario.
    {
        auto swResult = bucket_catalog::tryInsert(opCtx,
                                                  bucketCatalog,
                                                  bucketsColl->ns().getTimeseriesViewNamespace(),
                                                  bucketsColl->getDefaultCollator(),
                                                  measurementDoc,
                                                  combine,
                                                  insertContext,
                                                  time);
        if (!swResult.isOK()) {
            return swResult;
        }

        auto visitResult = visit(
            OverloadedVisitor{
                [&](const bucket_catalog::SuccessfulInsertion&)
                    -> StatusWith<bucket_catalog::InsertResult> { return std::move(swResult); },
                [&](bucket_catalog::ReopeningContext& reopeningContext)
                    -> StatusWith<bucket_catalog::InsertResult> {
                    auto suitableBucket = getSuitableBucketForReopening(
                        opCtx, bucketsColl, options, reopeningContext);

                    if (!suitableBucket.isEmpty()) {
                        reopeningContext.bucketToReopen = bucket_catalog::BucketToReopen{
                            suitableBucket, [&](OperationContext* opCtx, const BSONObj& bucketDoc) {
                                return bucketsColl->checkValidation(opCtx, bucketDoc);
                            }};
                    }

                    if (feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
                            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
                        !suitableBucket.isEmpty() &&
                        !timeseries::isCompressedBucket(suitableBucket)) {
                        uncompressedBucketId = extractBucketId(bucketCatalog,
                                                               options,
                                                               bucketsColl->getDefaultCollator(),
                                                               bucketsColl->uuid(),
                                                               suitableBucket);
                        return StatusWith<bucket_catalog::InsertResult>{
                            bucket_catalog::InsertResult{
                                std::in_place_type<bucket_catalog::ReopeningContext>,
                                std::move(reopeningContext)}};
                    }

                    return bucket_catalog::insertWithReopeningContext(
                        opCtx,
                        bucketCatalog,
                        bucketsColl->ns().getTimeseriesViewNamespace(),
                        bucketsColl->getDefaultCollator(),
                        measurementDoc,
                        combine,
                        reopeningContext,
                        insertContext,
                        time);
                },
                [](bucket_catalog::InsertWaiter& waiter)
                    -> StatusWith<bucket_catalog::InsertResult> {
                    // Need to wait for another operation to finish, then retry. This could be
                    // another reopening request or a previously prepared write batch for the same
                    // series (metaField value). The easiest way to retry here is to return a write
                    // conflict.
                    bucket_catalog::waitToInsert(&waiter);
                    return Status{ErrorCodes::WriteConflict, "waited to retry"};
                },
            },
            swResult.getValue());

        if (!uncompressedBucketId.has_value()) {
            return visitResult;
        }
    }

    try {
        LOGV2(8654200,
              "Compressing uncompressed bucket upon bucket reopen",
              "bucketId"_attr = uncompressedBucketId.get().oid);
        // Compress the uncompressed bucket and write to disk.
        if (compressAndWriteBucketFunc) {
            compressAndWriteBucketFunc(
                opCtx, uncompressedBucketId.get(), bucketsColl->ns(), options.getTimeField());
        }
    } catch (...) {
        bucket_catalog::freeze(bucketCatalog, uncompressedBucketId.get());
        LOGV2_WARNING(8654201,
                      "Failed to compress bucket for time-series insert upon reopening, will retry "
                      "insert on a new bucket",
                      "bucketId"_attr = uncompressedBucketId.get().oid);
        return Status{timeseries::BucketCompressionFailure(bucketsColl->uuid(),
                                                           uncompressedBucketId.get().oid,
                                                           uncompressedBucketId.get().keySignature),
                      "Failed to compress bucket for time-series insert upon reopening"};
    }
    return Status{ErrorCodes::WriteConflict,
                  "existing uncompressed bucket was compressed, retry insert on compressed bucket"};
}

BSONObj makeTimeseriesInsertCompressedBucketDocument(
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    const BSONObj& metadata,
    const std::vector<
        std::pair<StringData, BSONColumnBuilder<TrackingAllocator<void>>::BinaryDiff>>&
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

    auto metadataElem = metadata.firstElement();
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

}  // namespace

namespace details {
std::vector<Measurement> sortMeasurementsOnTimeField(
    std::shared_ptr<bucket_catalog::WriteBatch> batch) {
    std::vector<Measurement> measurements;

    // Convert measurements in batch from BSONObj to vector of data fields.
    // Store timefield separate to allow simple sort.
    for (auto& measurementObj : batch->measurements) {
        Measurement measurement;
        for (auto& dataField : measurementObj) {
            StringData key = dataField.fieldNameStringData();
            if (key == batch->bucketKey.metadata.getMetaField()) {
                continue;
            } else if (key == batch->timeField) {
                // Add time field to both members of Measurement, fallthrough expected.
                measurement.timeField = dataField;
            }
            measurement.dataFields.push_back(dataField);
        }
        measurements.push_back(std::move(measurement));
    }

    std::sort(measurements.begin(),
              measurements.end(),
              [](const Measurement& lhs, const Measurement& rhs) {
                  return lhs.timeField.timestamp() < rhs.timeField.timestamp();
              });

    return measurements;
}
}  // namespace details

write_ops::UpdateCommandRequest buildSingleUpdateOp(const write_ops::UpdateCommandRequest& wholeOp,
                                                    size_t opIndex) {
    write_ops::UpdateCommandRequest singleUpdateOp(wholeOp.getNamespace(),
                                                   {wholeOp.getUpdates()[opIndex]});
    auto& commandBase = singleUpdateOp.getWriteCommandRequestBase();
    commandBase.setOrdered(wholeOp.getOrdered());
    commandBase.setBypassDocumentValidation(wholeOp.getBypassDocumentValidation());
    commandBase.setBypassEmptyTsReplacement(wholeOp.getBypassEmptyTsReplacement());

    return singleUpdateOp;
}

void assertTimeseriesBucketsCollection(const Collection* bucketsColl) {
    uassert(
        8555700,
        "Catalog changed during operation, could not find time series buckets collection for write",
        bucketsColl);
    uassert(8555701,
            "Catalog changed during operation, missing time-series options",
            bucketsColl->getTimeseriesOptions());
}

BSONObj makeBSONColumnDocDiff(
    const BSONColumnBuilder<TrackingAllocator<void>>::BinaryDiff& binaryDiff) {
    return BSON(
        "o" << binaryDiff.offset() << "d"
            << BSONBinData(binaryDiff.data(), binaryDiff.size(), BinDataType::BinDataGeneral));
}

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
    const boost::optional<const StringDataComparator*>& comparator) {
    StringDataMap<BSONObjBuilder> dataBuilders;
    auto minmax =
        processTimeseriesMeasurements(measurements, metadata, dataBuilders, options, comparator);

    invariant(minmax);

    return makeNewDocument(
        bucketId,
        metadata,
        minmax->first,
        minmax->second,
        dataBuilders,
        options.getTimeField(),
        nss,
        collectionUUID,
        bucket_catalog::getKeySignature(
            options, comparator ? comparator.value() : nullptr, collectionUUID, metadata));
}

BSONObj makeBucketDocument(const std::vector<BSONObj>& measurements,
                           const NamespaceString& nss,
                           const UUID& collectionUUID,
                           const TimeseriesOptions& options,
                           const StringDataComparator* comparator) {
    TrackingContext trackingContext;
    auto res = uassertStatusOK(bucket_catalog::internal::extractBucketingParameters(
        trackingContext, collectionUUID, comparator, options, measurements[0]));
    auto time = res.second;
    auto [oid, _] = bucket_catalog::internal::generateBucketOID(time, options);
    BucketDocument bucketDoc = makeNewDocumentForWrite(
        nss, collectionUUID, oid, measurements, res.first.metadata.toBSON(), options, comparator);

    invariant(bucketDoc.compressedBucket ||
              !feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
                  serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
    if (bucketDoc.compressedBucket) {
        return *bucketDoc.compressedBucket;
    }
    return bucketDoc.uncompressedBucket;
}

std::variant<write_ops::UpdateCommandRequest, write_ops::DeleteCommandRequest> makeModificationOp(
    const OID& bucketId, const CollectionPtr& coll, const std::vector<BSONObj>& measurements) {
    // A bucket will be fully deleted if no measurements are passed in.
    if (measurements.empty()) {
        write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId), false);
        write_ops::DeleteCommandRequest op(coll->ns(), {deleteEntry});
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
                                                       coll->getDefaultCollator());
    BSONObj bucketToReplace = bucketDoc.uncompressedBucket;
    invariant(bucketDoc.compressedBucket ||
              !feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
                  serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
    if (bucketDoc.compressedBucket) {
        bucketToReplace = *bucketDoc.compressedBucket;
    }

    write_ops::UpdateModification u(bucketToReplace);
    write_ops::UpdateOpEntry updateEntry(BSON("_id" << bucketId), std::move(u));
    write_ops::UpdateCommandRequest op(coll->ns(), {updateEntry});
    return op;
}

write_ops::UpdateOpEntry makeTimeseriesTransformationOpEntry(
    OperationContext* opCtx,
    const OID& bucketId,
    write_ops::UpdateModification::TransformFunc transformationFunc) {
    write_ops::UpdateModification u(std::move(transformationFunc));
    write_ops::UpdateOpEntry update(BSON("_id" << bucketId), std::move(u));
    invariant(!update.getMulti(), bucketId.toString());
    invariant(!update.getUpsert(), bucketId.toString());
    return update;
}

void getOpTimeAndElectionId(OperationContext* opCtx,
                            boost::optional<repl::OpTime>* opTime,
                            boost::optional<OID>* electionId) {
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
    const auto isReplSet = replCoord->getSettings().isReplSet();

    *opTime = isReplSet
        ? boost::make_optional(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp())
        : boost::none;
    *electionId = isReplSet ? boost::make_optional(replCoord->getElectionId()) : boost::none;
}

write_ops::InsertCommandRequest makeTimeseriesInsertOp(
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    const NamespaceString& bucketsNs,
    const BSONObj& metadata,
    std::vector<StmtId>&& stmtIds) {
    invariant(!batch->isReopened);

    BSONObj bucketToInsert;
    BucketDocument bucketDoc;
    if (feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        std::vector<details::Measurement> sortedMeasurements =
            details::sortMeasurementsOnTimeField(batch);

        // Insert measurements, and appropriate skips, into all column builders.
        for (const auto& measurement : sortedMeasurements) {
            batch->measurementMap.insertOne(measurement.dataFields);
        }
        int32_t compressedSizeDelta;
        auto intermediates = batch->measurementMap.intermediate(compressedSizeDelta);
        batch->sizes.uncommittedVerifiedSize = compressedSizeDelta;
        bucketToInsert =
            makeTimeseriesInsertCompressedBucketDocument(batch, metadata, intermediates);

        // Extra verification that the insert op decompresses to the same values put in.
        if (gPerformTimeseriesCompressionIntermediateDataIntegrityCheckOnInsert.load()) {
            auto verifierFunction =
                makeVerifierFunction(sortedMeasurements, batch, OperationSource::kTimeseriesInsert);
            verifierFunction(bucketToInsert);
        }
    } else {
        bucketDoc = makeNewDocumentForWrite(bucketsNs, batch, metadata);
        bucketToInsert = bucketDoc.uncompressedBucket;
    }

    write_ops::InsertCommandRequest op{bucketsNs, {bucketToInsert}};
    op.setWriteCommandRequestBase(makeTimeseriesWriteOpBase(std::move(stmtIds)));
    return op;
}

write_ops::UpdateCommandRequest makeTimeseriesUpdateOp(
    OperationContext* opCtx,
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    const NamespaceString& bucketsNs,
    const BSONObj& metadata,
    std::vector<StmtId>&& stmtIds) {
    invariant(!feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
    write_ops::UpdateCommandRequest op(bucketsNs,
                                       {makeTimeseriesUpdateOpEntry(opCtx, batch, metadata)});
    op.setWriteCommandRequestBase(makeTimeseriesWriteOpBase(std::move(stmtIds)));
    return op;
}

/**
 * Build the before and after data fields of the bucket documents efficiently with the column
 * builders, but do not build out the rest of the bucket document (control field, etc). Then
 * generate an update op based on the diff of the data fields, and relevant fields of control field.
 */
write_ops::UpdateCommandRequest makeTimeseriesCompressedDiffUpdateOp(
    OperationContext* opCtx,
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    const NamespaceString& bucketsNs,
    std::vector<StmtId>&& stmtIds) {
    using namespace details;
    invariant(feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));


    bool changedToUnsorted = false;
    std::vector<Measurement> sortedMeasurements = sortMeasurementsOnTimeField(batch);
    if (batch->bucketIsSortedByTime &&
        sortedMeasurements.begin()->timeField.timestamp() <
            batch->measurementMap.timeOfLastMeasurement(batch->timeField)) {
        batch->bucketIsSortedByTime = false;
        changedToUnsorted = true;
        batch->stats.incNumCompressedBucketsConvertedToUnsorted();
    }

    // Insert new measurements, and appropriate skips, into all column builders.
    for (const auto& measurement : sortedMeasurements) {
        batch->measurementMap.insertOne(measurement.dataFields);
    }

    // Generates a delta update request using the before and after compressed bucket documents' data
    // fields. The only other items that will be different are the min, max, and count fields in the
    // control block, and the version field if it was promoted to a v3 bucket.
    const auto updateEntry =
        makeTimeseriesCompressedDiffEntry(opCtx, batch, changedToUnsorted, sortedMeasurements);
    write_ops::UpdateCommandRequest op(bucketsNs, {updateEntry});
    op.setWriteCommandRequestBase(makeTimeseriesWriteOpBase(std::move(stmtIds)));
    return op;
}

StatusWith<bucket_catalog::InsertResult> attemptInsertIntoBucket(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& bucketCatalog,
    const Collection* bucketsColl,
    TimeseriesOptions& timeSeriesOptions,
    const BSONObj& measurementDoc,
    BucketReopeningPermittance reopening,
    bucket_catalog::CombineWithInsertsFromOtherClients combine,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc) {
    auto insertContextAndDate = bucket_catalog::prepareInsert(bucketCatalog,
                                                              bucketsColl->uuid(),
                                                              bucketsColl->getDefaultCollator(),
                                                              timeSeriesOptions,
                                                              measurementDoc);

    if (!insertContextAndDate.isOK()) {
        return insertContextAndDate.getStatus();
    }
    switch (reopening) {
        case BucketReopeningPermittance::kAllowed:
            while (true) {
                auto result = attemptInsertIntoBucketWithReopening(
                    opCtx,
                    bucketCatalog,
                    bucketsColl,
                    timeSeriesOptions,
                    measurementDoc,
                    combine,
                    compressAndWriteBucketFunc,
                    std::get<bucket_catalog::InsertContext>(insertContextAndDate.getValue()),
                    std::get<Date_t>(insertContextAndDate.getValue()));
                if (!result.isOK()) {
                    if (result.getStatus().code() == ErrorCodes::WriteConflict) {
                        // If there is an era offset (between the bucket we want to reopen and the
                        // catalog's current era), we could hit a WriteConflict error indicating we
                        // will need to refetch a bucket document as it is potentially stale. The
                        // other scenario this will happen is when an uncompressed bucket is
                        // encountered and must be compressed before reattempting a write to the
                        // compressed bucket.

                        for (auto& closedBucket : std::get<bucket_catalog::InsertContext>(
                                                      insertContextAndDate.getValue())
                                                      .closedBuckets) {
                            compressAndWriteBucketFunc(opCtx,
                                                       closedBucket.bucketId,
                                                       bucketsColl->ns(),
                                                       closedBucket.timeField);
                        }

                        // Will update state in the bucket catalog to clear out the closed buckets.
                        std::get<bucket_catalog::InsertContext>(insertContextAndDate.getValue())
                            .closedBuckets.clear();
                        continue;
                    } else if (result.getStatus().code() ==
                               ErrorCodes::TimeseriesBucketCompressionFailed) {
                        // This is the case where the reopened bucket was corrupted. Retry the
                        // insert directly on a new bucket.
                        return bucket_catalog::insert(
                            opCtx,
                            bucketCatalog,
                            bucketsColl->ns().getTimeseriesViewNamespace(),
                            bucketsColl->getDefaultCollator(),
                            measurementDoc,
                            combine,
                            std::get<bucket_catalog::InsertContext>(
                                insertContextAndDate.getValue()),
                            std::get<Date_t>(insertContextAndDate.getValue()));
                    }
                }
                return result;
            }
        case BucketReopeningPermittance::kDisallowed:
            return bucket_catalog::insert(
                opCtx,
                bucketCatalog,
                bucketsColl->ns().getTimeseriesViewNamespace(),
                bucketsColl->getDefaultCollator(),
                measurementDoc,
                combine,
                std::get<bucket_catalog::InsertContext>(insertContextAndDate.getValue()),
                std::get<Date_t>(insertContextAndDate.getValue()));
    }
    MONGO_UNREACHABLE;
}

void makeWriteRequest(OperationContext* opCtx,
                      std::shared_ptr<bucket_catalog::WriteBatch> batch,
                      const BSONObj& metadata,
                      TimeseriesStmtIds& stmtIds,
                      const NamespaceString& bucketsNs,
                      std::vector<write_ops::InsertCommandRequest>* insertOps,
                      std::vector<write_ops::UpdateCommandRequest>* updateOps) {
    if (batch->numPreviouslyCommittedMeasurements == 0) {
        insertOps->push_back(makeTimeseriesInsertOp(
            batch, bucketsNs, metadata, std::move(stmtIds[batch->bucketId.oid])));
        return;
    }
    if (batch->generateCompressedDiff) {
        updateOps->push_back(makeTimeseriesCompressedDiffUpdateOp(
            opCtx, batch, bucketsNs, std::move(stmtIds[batch->bucketId.oid])));
    } else {
        updateOps->push_back(makeTimeseriesUpdateOp(
            opCtx, batch, bucketsNs, metadata, std::move(stmtIds[batch->bucketId.oid])));
    }
}

TimeseriesBatches insertIntoBucketCatalogForUpdate(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& bucketCatalog,
    const CollectionPtr& bucketsColl,
    const std::vector<BSONObj>& measurements,
    const NamespaceString& bucketsNs,
    TimeseriesOptions& timeSeriesOptions,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc) {
    TimeseriesBatches batches;

    for (const auto& measurement : measurements) {
        auto result = uassertStatusOK(
            attemptInsertIntoBucket(opCtx,
                                    bucketCatalog,
                                    bucketsColl.get(),
                                    timeSeriesOptions,
                                    measurement,
                                    BucketReopeningPermittance::kDisallowed,
                                    bucket_catalog::CombineWithInsertsFromOtherClients::kDisallow,
                                    compressAndWriteBucketFunc));
        auto* insertResult = get_if<bucket_catalog::SuccessfulInsertion>(&result);
        invariant(insertResult);
        batches.emplace_back(std::move(insertResult->batch));
    }

    return batches;
}

void performAtomicWrites(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const RecordId& recordId,
    const boost::optional<std::variant<write_ops::UpdateCommandRequest,
                                       write_ops::DeleteCommandRequest>>& modificationOp,
    const std::vector<write_ops::InsertCommandRequest>& insertOps,
    const std::vector<write_ops::UpdateCommandRequest>& updateOps,
    bool fromMigrate,
    StmtId stmtId) {
    tassert(
        7655102, "must specify at least one type of write", modificationOp || !insertOps.empty());
    NamespaceString ns = coll->ns();

    DisableDocumentValidation disableDocumentValidation{opCtx};

    write_ops_exec::LastOpFixer lastOpFixer{opCtx};
    lastOpFixer.startingOp(ns);

    auto curOp = CurOp::get(opCtx);
    curOp->raiseDbProfileLevel(CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns.dbName()));

    write_ops_exec::assertCanWrite_inlock(opCtx, ns);

    // Groups all operations in one or several chained oplog entries to ensure the writes are
    // replicated atomically.
    auto groupOplogEntries =
        !opCtx->getTxnNumber() && (!insertOps.empty() || !updateOps.empty()) && modificationOp
        ? WriteUnitOfWork::kGroupForTransaction
        : WriteUnitOfWork::kDontGroup;
    WriteUnitOfWork wuow{opCtx, groupOplogEntries};

    if (modificationOp) {
        visit(
            OverloadedVisitor{[&](const write_ops::UpdateCommandRequest& updateOp) {
                                  updateTimeseriesDocument(
                                      opCtx, coll, updateOp, &curOp->debug(), fromMigrate, stmtId);
                              },
                              [&](const write_ops::DeleteCommandRequest& deleteOp) {
                                  invariant(deleteOp.getDeletes().size() == 1);
                                  auto deleteId = record_id_helpers::keyForOID(
                                      deleteOp.getDeletes().front().getQ()["_id"].OID());
                                  invariant(recordId == deleteId);
                                  collection_internal::deleteDocument(
                                      opCtx, coll, stmtId, recordId, &curOp->debug(), fromMigrate);
                              }},
            *modificationOp);
    }

    if (!insertOps.empty()) {
        std::vector<InsertStatement> insertStatements;
        for (auto& op : insertOps) {
            invariant(op.getDocuments().size() == 1);
            if (modificationOp) {
                insertStatements.emplace_back(op.getDocuments().front());
            } else {
                // Appends the stmtId for upsert.
                insertStatements.emplace_back(stmtId, op.getDocuments().front());
            }
        }
        uassertStatusOK(collection_internal::insertDocuments(
            opCtx, coll, insertStatements.begin(), insertStatements.end(), &curOp->debug()));
    }

    for (auto& updateOp : updateOps) {
        updateTimeseriesDocument(opCtx, coll, updateOp, &curOp->debug(), fromMigrate, stmtId);
    }

    wuow.commit();

    lastOpFixer.finishedOpSuccessfully();
}

void commitTimeseriesBucketsAtomically(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& sideBucketCatalog,
    const CollectionPtr& coll,
    const RecordId& recordId,
    const boost::optional<std::variant<write_ops::UpdateCommandRequest,
                                       write_ops::DeleteCommandRequest>>& modificationOp,
    TimeseriesBatches* batches,
    const NamespaceString& bucketsNs,
    bool fromMigrate,
    StmtId stmtId,
    std::set<bucket_catalog::BucketId>* bucketIds) {
    auto batchesToCommit = determineBatchesToCommit(*batches, extractFromSelf);
    if (batchesToCommit.empty()) {
        return;
    }

    Status abortStatus = Status::OK();
    ScopeGuard batchGuard{[&] {
        for (auto batch : batchesToCommit) {
            if (batch.get()) {
                abort(sideBucketCatalog, batch, abortStatus);
            }
        }
    }};

    try {
        std::vector<write_ops::InsertCommandRequest> insertOps;
        std::vector<write_ops::UpdateCommandRequest> updateOps;

        auto& mainBucketCatalog = bucket_catalog::BucketCatalog::get(opCtx);
        for (auto batch : batchesToCommit) {
            auto metadata = getMetadata(sideBucketCatalog, batch.get()->bucketId);
            auto prepareCommitStatus =
                prepareCommit(sideBucketCatalog, coll->ns().getTimeseriesViewNamespace(), batch);
            if (!prepareCommitStatus.isOK()) {
                abortStatus = prepareCommitStatus;
                return;
            }

            TimeseriesStmtIds emptyStmtIds = {};
            makeWriteRequest(
                opCtx, batch, metadata, emptyStmtIds, bucketsNs, &insertOps, &updateOps);

            // Starts tracking the newly inserted bucket in the main bucket catalog as a direct
            // write to prevent other writers from modifying it.
            if (batch.get()->numPreviouslyCommittedMeasurements == 0) {
                directWriteStart(mainBucketCatalog.bucketStateRegistry, batch.get()->bucketId);
                bucketIds->insert(batch.get()->bucketId);
            }
        }

        performAtomicWrites(
            opCtx, coll, recordId, modificationOp, insertOps, updateOps, fromMigrate, stmtId);

        boost::optional<repl::OpTime> opTime;
        boost::optional<OID> electionId;
        getOpTimeAndElectionId(opCtx, &opTime, &electionId);

        for (auto batch : batchesToCommit) {
            finish(opCtx,
                   sideBucketCatalog,
                   coll->ns(),
                   batch,
                   bucket_catalog::CommitInfo{opTime, electionId});
            batch.get().reset();
        }
    } catch (const DBException& ex) {
        abortStatus = ex.toStatus();
        throw;
    }

    batchGuard.dismiss();
}

void performAtomicWritesForDelete(OperationContext* opCtx,
                                  const CollectionPtr& coll,
                                  const RecordId& recordId,
                                  const std::vector<BSONObj>& unchangedMeasurements,
                                  bool fromMigrate,
                                  StmtId stmtId) {
    OID bucketId = record_id_helpers::toBSONAs(recordId, "_id")["_id"].OID();
    auto modificationOp = makeModificationOp(bucketId, coll, unchangedMeasurements);
    performAtomicWrites(opCtx, coll, recordId, modificationOp, {}, {}, fromMigrate, stmtId);
}

void performAtomicWritesForUpdate(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const RecordId& recordId,
    const boost::optional<std::vector<BSONObj>>& unchangedMeasurements,
    const std::vector<BSONObj>& modifiedMeasurements,
    bucket_catalog::BucketCatalog& sideBucketCatalog,
    bool fromMigrate,
    StmtId stmtId,
    std::set<bucket_catalog::BucketId>* bucketIds,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc) {
    auto timeSeriesOptions = *coll->getTimeseriesOptions();
    auto batches = insertIntoBucketCatalogForUpdate(opCtx,
                                                    sideBucketCatalog,
                                                    coll,
                                                    modifiedMeasurements,
                                                    coll->ns(),
                                                    timeSeriesOptions,
                                                    compressAndWriteBucketFunc);

    auto modificationRequest = unchangedMeasurements
        ? boost::make_optional(
              makeModificationOp(record_id_helpers::toBSONAs(recordId, "_id")["_id"].OID(),
                                 coll,
                                 *unchangedMeasurements))
        : boost::none;
    commitTimeseriesBucketsAtomically(opCtx,
                                      sideBucketCatalog,
                                      coll,
                                      recordId,
                                      modificationRequest,
                                      &batches,
                                      coll->ns(),
                                      fromMigrate,
                                      stmtId,
                                      bucketIds);
}

BSONObj timeseriesViewCommand(const BSONObj& cmd, std::string cmdName, StringData viewNss) {
    BSONObjBuilder b;
    for (auto&& e : cmd) {
        if (e.fieldNameStringData() == cmdName) {
            b.append(cmdName, viewNss);
        } else {
            b.append(e);
        }
    }
    return b.obj();
}

void deleteRequestCheckFunction(DeleteRequest* request, const TimeseriesOptions& options) {
    if (!feature_flags::gTimeseriesDeletesSupport.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform a delete with a non-empty query on a time-series "
                "collection that "
                "does not have a metaField ",
                options.getMetaField() || request->getQuery().isEmpty());

        uassert(ErrorCodes::IllegalOperation,
                "Cannot perform a non-multi delete on a time-series collection",
                request->getMulti());
        if (auto metaField = options.getMetaField()) {
            request->setQuery(timeseries::translateQuery(request->getQuery(), *metaField));
        }
    }
}

void updateRequestCheckFunction(UpdateRequest* request, const TimeseriesOptions& options) {
    if (!feature_flags::gTimeseriesUpdatesSupport.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform a non-multi update on a time-series collection",
                request->isMulti());

        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform an upsert on a time-series collection",
                !request->isUpsert());

        auto metaField = options.getMetaField();
        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform an update on a time-series collection that does not have a "
                "metaField",
                options.getMetaField());

        request->setQuery(timeseries::translateQuery(request->getQuery(), *metaField));
        auto modification = uassertStatusOK(
            timeseries::translateUpdate(request->getUpdateModification(), *metaField));
        request->setUpdateModification(modification);
    }
}
}  // namespace mongo::timeseries
