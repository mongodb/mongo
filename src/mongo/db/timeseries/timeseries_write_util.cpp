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

#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/ops/write_ops_exec_util.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "timeseries_index_schema_conversion_functions.h"

namespace mongo::timeseries {
namespace {

// Builds the data field of a bucket document. Computes the min and max fields if necessary.
boost::optional<bucket_catalog::MinMax> processTimeseriesMeasurements(
    const std::vector<BSONObj>& measurements,
    const BSONObj& metadata,
    StringDataMap<BSONObjBuilder>& dataBuilders,
    const boost::optional<TimeseriesOptions>& options = boost::none,
    const boost::optional<const StringData::ComparatorInterface*>& comparator = boost::none) {
    bucket_catalog::MinMax minmax;
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
        return minmax;
    }

    return boost::none;
}

// Builds a complete and new bucket document.
BSONObj makeNewDocument(const OID& bucketId,
                        const BSONObj& metadata,
                        const BSONObj& min,
                        const BSONObj& max,
                        StringDataMap<BSONObjBuilder>& dataBuilders) {
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

    return builder.obj();
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

// Builds the delta update oplog entry from a time-series insert write batch.
write_ops::UpdateOpEntry makeTimeseriesUpdateOpEntry(
    OperationContext* opCtx,
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const BSONObj& metadata) {
    BSONObjBuilder updateBuilder;
    {
        if (!batch->min.isEmpty() || !batch->max.isEmpty()) {
            BSONObjBuilder controlBuilder(updateBuilder.subobjStart(
                str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "control"));
            if (!batch->min.isEmpty()) {
                controlBuilder.append(
                    str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "min", batch->min);
            }
            if (!batch->max.isEmpty()) {
                controlBuilder.append(
                    str::stream() << doc_diff::kSubDiffSectionFieldPrefix << "max", batch->max);
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

        // doc_diff::kSubDiffSectionFieldPrefix + <field name>
        BSONObjBuilder dataBuilder(updateBuilder.subobjStart("sdata"));
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
    auto oid = batch->bucketHandle.bucketId.oid;
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
        updated = doc_diff::applyDiff(
            original.value(), diffFromUpdate, static_cast<bool>(repl::tenantMigrationInfo(opCtx)));
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
}  // namespace

BSONObj makeNewDocumentForWrite(std::shared_ptr<bucket_catalog::WriteBatch> batch,
                                const BSONObj& metadata) {
    StringDataMap<BSONObjBuilder> dataBuilders;
    processTimeseriesMeasurements(
        {batch->measurements.begin(), batch->measurements.end()}, metadata, dataBuilders);

    return makeNewDocument(
        batch->bucketHandle.bucketId.oid, metadata, batch->min, batch->max, dataBuilders);
}

BSONObj makeNewDocumentForWrite(
    const OID& bucketId,
    const std::vector<BSONObj>& measurements,
    const BSONObj& metadata,
    const boost::optional<TimeseriesOptions>& options,
    const boost::optional<const StringData::ComparatorInterface*>& comparator) {
    StringDataMap<BSONObjBuilder> dataBuilders;
    auto minmax =
        processTimeseriesMeasurements(measurements, metadata, dataBuilders, options, comparator);

    invariant(minmax);

    return makeNewDocument(bucketId, metadata, minmax->min(), minmax->max(), dataBuilders);
}

std::vector<write_ops::InsertCommandRequest> makeInsertsToNewBuckets(
    const std::vector<BSONObj>& measurements,
    const NamespaceString& nss,
    const TimeseriesOptions& options,
    const StringData::ComparatorInterface* comparator) {
    std::vector<write_ops::InsertCommandRequest> insertOps;
    for (const auto& measurement : measurements) {
        auto res = uassertStatusOK(bucket_catalog::internal::extractBucketingParameters(
            nss, comparator, options, measurement));
        auto time = res.second;
        auto [oid, _] = bucket_catalog::internal::generateBucketOID(time, options);
        insertOps.push_back(
            {nss,
             {makeNewDocumentForWrite(
                 oid, {measurement}, res.first.metadata.toBSON(), options, comparator)}});
    }
    return insertOps;
}

stdx::variant<write_ops::UpdateCommandRequest, write_ops::DeleteCommandRequest> makeModificationOp(
    const OID& bucketId, const CollectionPtr& coll, const std::vector<BSONObj>& measurements) {
    if (measurements.empty()) {
        write_ops::DeleteOpEntry deleteEntry(BSON("_id" << bucketId), false);
        write_ops::DeleteCommandRequest op(coll->ns(), {deleteEntry});
        return op;
    }
    auto timeseriesOptions = coll->getTimeseriesOptions();
    auto metaFieldName = timeseriesOptions->getMetaField();
    auto metadata = [&] {
        if (!metaFieldName) {  // Collection has no metadata field.
            return BSONObj();
        }
        // Look for the metadata field on this bucket and return it if present.
        auto metaField = measurements[0].getField(*metaFieldName);
        return metaField ? metaField.wrap() : BSONObj();
    }();
    auto replaceBucket = timeseries::makeNewDocumentForWrite(
        bucketId, measurements, metadata, timeseriesOptions, coll->getDefaultCollator());

    write_ops::UpdateModification u(replaceBucket);
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
    const auto replMode = replCoord->getReplicationMode();

    *opTime = replMode != repl::ReplicationCoordinator::modeNone
        ? boost::make_optional(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp())
        : boost::none;
    *electionId = replMode == repl::ReplicationCoordinator::modeReplSet
        ? boost::make_optional(replCoord->getElectionId())
        : boost::none;
}

write_ops::InsertCommandRequest makeTimeseriesInsertOp(
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const NamespaceString& bucketsNs,
    const BSONObj& metadata,
    std::vector<StmtId>&& stmtIds) {
    write_ops::InsertCommandRequest op{bucketsNs, {makeNewDocumentForWrite(batch, metadata)}};
    op.setWriteCommandRequestBase(makeTimeseriesWriteOpBase(std::move(stmtIds)));
    return op;
}

write_ops::UpdateCommandRequest makeTimeseriesUpdateOp(
    OperationContext* opCtx,
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const NamespaceString& bucketsNs,
    const BSONObj& metadata,
    std::vector<StmtId>&& stmtIds) {
    write_ops::UpdateCommandRequest op(bucketsNs,
                                       {makeTimeseriesUpdateOpEntry(opCtx, batch, metadata)});
    op.setWriteCommandRequestBase(makeTimeseriesWriteOpBase(std::move(stmtIds)));
    return op;
}

write_ops::UpdateCommandRequest makeTimeseriesDecompressAndUpdateOp(
    OperationContext* opCtx,
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const NamespaceString& bucketsNs,
    const BSONObj& metadata,
    std::vector<StmtId>&& stmtIds) {
    // Generate the diff and apply it against the previously decompressed bucket document.
    const bool mustCheckExistenceForInsertOperations =
        static_cast<bool>(repl::tenantMigrationInfo(opCtx));
    auto diff = makeTimeseriesUpdateOpEntry(opCtx, batch, metadata).getU().getDiff();
    auto after = doc_diff::applyDiff(
        batch->decompressed.value().after, diff, mustCheckExistenceForInsertOperations);

    auto bucketDecompressionFunc =
        [before = std::move(batch->decompressed.value().before),
         after = std::move(after)](const BSONObj& bucketDoc) -> boost::optional<BSONObj> {
        // Make sure the document hasn't changed since we read it into the BucketCatalog.
        // This should not happen, but since we can double-check it here, we can guard
        // against the missed update that would result from simply replacing with 'after'.
        if (!bucketDoc.binaryEqual(before)) {
            throwWriteConflictException("Bucket document changed between initial read and update");
        }
        return after;
    };

    write_ops::UpdateCommandRequest op(
        bucketsNs,
        {makeTimeseriesTransformationOpEntry(
            opCtx, batch->bucketHandle.bucketId.oid, std::move(bucketDecompressionFunc))});
    op.setWriteCommandRequestBase(makeTimeseriesWriteOpBase(std::move(stmtIds)));
    return op;
}

StatusWith<timeseries::bucket_catalog::InsertResult> attemptInsertIntoBucket(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& bucketCatalog,
    const NamespaceString& viewNs,
    const Collection* bucketsColl,
    TimeseriesOptions& timeSeriesOptions,
    const BSONObj& measurementDoc,
    bucket_catalog::CombineWithInsertsFromOtherClients combine,
    bool fromUpdates) {
    StatusWith<timeseries::bucket_catalog::InsertResult> swResult =
        Status{ErrorCodes::BadValue, "Uninitialized InsertResult"};
    do {
        if (!fromUpdates &&
            feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
                serverGlobalParams.featureCompatibility)) {
            swResult = timeseries::bucket_catalog::tryInsert(opCtx,
                                                             bucketCatalog,
                                                             viewNs,
                                                             bucketsColl->getDefaultCollator(),
                                                             timeSeriesOptions,
                                                             measurementDoc,
                                                             combine);

            if (swResult.isOK()) {
                const auto& insertResult = swResult.getValue();

                // If the InsertResult doesn't contain a batch, we failed to insert the
                // measurement into an open bucket and need to create/reopen a bucket.
                if (!insertResult.batch) {
                    timeseries::bucket_catalog::BucketFindResult bucketFindResult;
                    BSONObj suitableBucket;

                    if (auto* bucketId = stdx::get_if<OID>(&insertResult.candidate)) {
                        DBDirectClient client{opCtx};
                        suitableBucket =
                            client.findOne(bucketsColl->ns(), BSON("_id" << *bucketId));
                        bucketFindResult.fetchedBucket = true;
                    } else if (auto* pipeline =
                                   stdx::get_if<std::vector<BSONObj>>(&insertResult.candidate)) {
                        // Resort to Query-Based reopening approach.
                        DBDirectClient client{opCtx};

                        // Ensure we have a index on meta and time for the time-series
                        // collection before performing the query. Without the index we
                        // will perform a full collection scan which could cause us to
                        // take a performance hit.
                        if (timeseries::collectionHasIndexSupportingReopeningQuery(
                                opCtx, bucketsColl->getIndexCatalog(), timeSeriesOptions)) {

                            // Run an aggregation to find a suitable bucket to reopen.
                            AggregateCommandRequest aggRequest(bucketsColl->ns(), *pipeline);

                            auto cursor = uassertStatusOK(
                                    DBClientCursor::fromAggregationRequest(&client,
                                                                           aggRequest,
                                                                           false /* secondaryOk
                                                                           */, false /*
                                                                           useExhaust*/));

                            if (cursor->more()) {
                                suitableBucket = cursor->next();
                            }
                            bucketFindResult.queriedBucket = true;
                        }
                    }

                    boost::optional<timeseries::bucket_catalog::BucketToReopen> bucketToReopen =
                        boost::none;
                    if (!suitableBucket.isEmpty()) {
                        auto validator = [&](OperationContext * opCtx,
                                             const BSONObj& bucketDoc) -> auto {
                            return bucketsColl->checkValidation(opCtx, bucketDoc);
                        };
                        auto bucketToReopen = timeseries::bucket_catalog::BucketToReopen{
                            suitableBucket, validator, insertResult.catalogEra};
                        bucketFindResult.bucketToReopen = std::move(bucketToReopen);
                    }

                    swResult = timeseries::bucket_catalog::insert(opCtx,
                                                                  bucketCatalog,
                                                                  viewNs,
                                                                  bucketsColl->getDefaultCollator(),
                                                                  timeSeriesOptions,
                                                                  measurementDoc,
                                                                  combine,
                                                                  std::move(bucketFindResult));
                }
            }
        } else {
            timeseries::bucket_catalog::BucketFindResult bucketFindResult;
            swResult = timeseries::bucket_catalog::insert(opCtx,
                                                          bucketCatalog,
                                                          viewNs,
                                                          bucketsColl->getDefaultCollator(),
                                                          timeSeriesOptions,
                                                          measurementDoc,
                                                          combine,
                                                          bucketFindResult);
        }

        // If there is an era offset (between the bucket we want to reopen and the
        // catalog's current era), we could hit a WriteConflict error indicating we will
        // need to refetch a bucket document as it is potentially stale.
    } while (!swResult.isOK() && (swResult.getStatus().code() == ErrorCodes::WriteConflict));
    return swResult;
}

void makeWriteRequest(OperationContext* opCtx,
                      std::shared_ptr<bucket_catalog::WriteBatch> batch,
                      const BSONObj& metadata,
                      TimeseriesStmtIds& stmtIds,
                      const NamespaceString& bucketsNs,
                      std::vector<write_ops::InsertCommandRequest>* insertOps,
                      std::vector<write_ops::UpdateCommandRequest>* updateOps) {
    if (batch.get()->numPreviouslyCommittedMeasurements == 0) {
        insertOps->push_back(
            makeTimeseriesInsertOp(batch,
                                   bucketsNs,
                                   metadata,
                                   std::move(stmtIds[batch.get()->bucketHandle.bucketId.oid])));
        return;
    }
    if (batch.get()->decompressed.has_value()) {
        updateOps->push_back(makeTimeseriesDecompressAndUpdateOp(
            opCtx,
            batch,
            bucketsNs,
            metadata,
            std::move(stmtIds[batch.get()->bucketHandle.bucketId.oid])));
    } else {
        updateOps->push_back(
            makeTimeseriesUpdateOp(opCtx,
                                   batch,
                                   bucketsNs,
                                   metadata,
                                   std::move(stmtIds[batch.get()->bucketHandle.bucketId.oid])));
    }
}

void performAtomicWrites(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const RecordId& recordId,
    const boost::optional<stdx::variant<write_ops::UpdateCommandRequest,
                                        write_ops::DeleteCommandRequest>>& modificationOp,
    const std::vector<write_ops::InsertCommandRequest>& insertOps,
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
    // TODO(SERVER-76432): Handle the updateOne case for retryable writes.
    auto groupOplogEntries = !opCtx->getTxnNumber() && !insertOps.empty() && modificationOp;
    WriteUnitOfWork wuow{opCtx, groupOplogEntries};

    if (modificationOp) {
        stdx::visit(
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
            insertStatements.emplace_back(op.getDocuments().front());
        }
        uassertStatusOK(collection_internal::insertDocuments(
            opCtx, coll, insertStatements.begin(), insertStatements.end(), &curOp->debug()));
    }

    wuow.commit();

    lastOpFixer.finishedOpSuccessfully();
}

}  // namespace mongo::timeseries
