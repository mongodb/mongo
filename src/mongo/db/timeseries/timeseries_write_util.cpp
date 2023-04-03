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
#include "mongo/db/curop.h"
#include "mongo/db/ops/write_ops_exec_util.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"

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

Status performAtomicWrites(OperationContext* opCtx,
                           const CollectionPtr& coll,
                           const RecordId& recordId,
                           const stdx::variant<write_ops::UpdateCommandRequest,
                                               write_ops::DeleteCommandRequest>& modificationOp,
                           bool fromMigrate,
                           StmtId stmtId) try {
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    invariant(!opCtx->inMultiDocumentTransaction());

    NamespaceString ns = coll->ns();

    DisableDocumentValidation disableDocumentValidation{opCtx};

    write_ops_exec::LastOpFixer lastOpFixer{opCtx, ns};
    lastOpFixer.startingOp();

    auto curOp = CurOp::get(opCtx);
    curOp->raiseDbProfileLevel(CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns.dbName()));

    write_ops_exec::assertCanWrite_inlock(opCtx, ns);

    WriteUnitOfWork wuow{opCtx};

    stdx::visit(
        OverloadedVisitor{
            [&](const write_ops::UpdateCommandRequest& updateOp) {
                invariant(updateOp.getUpdates().size() == 1);
                auto& update = updateOp.getUpdates().front();

                invariant(coll->isClustered());

                auto original = coll->docFor(opCtx, recordId);

                CollectionUpdateArgs args{original.value()};
                args.criteria = update.getQ();
                args.stmtIds = {stmtId};
                if (fromMigrate) {
                    args.source = OperationSource::kFromMigrate;
                }

                BSONObj diffFromUpdate;
                const BSONObj* diffOnIndexes =
                    collection_internal::kUpdateAllIndexes;  // Assume all indexes are affected.

                // Overwrites the original bucket.
                invariant(update.getU().type() ==
                          write_ops::UpdateModification::Type::kReplacement);
                auto updated = update.getU().getUpdateReplacement();
                args.update = update_oplog_entry::makeReplacementOplogEntry(updated);

                collection_internal::updateDocument(opCtx,
                                                    coll,
                                                    recordId,
                                                    original,
                                                    updated,
                                                    diffOnIndexes,
                                                    &curOp->debug(),
                                                    &args);
            },
            [&](const write_ops::DeleteCommandRequest& deleteOp) {
                invariant(deleteOp.getDeletes().size() == 1);
                auto deleteId =
                    record_id_helpers::keyForOID(deleteOp.getDeletes().front().getQ()["_id"].OID());
                invariant(recordId == deleteId);
                collection_internal::deleteDocument(
                    opCtx, coll, stmtId, recordId, &curOp->debug(), fromMigrate);
            }},
        modificationOp);

    wuow.commit();

    lastOpFixer.finishedOpSuccessfully();

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

}  // namespace mongo::timeseries
