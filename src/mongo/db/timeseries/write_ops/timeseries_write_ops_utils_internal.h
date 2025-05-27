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

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/write_ops/measurement.h"
#include "mongo/util/tracking/allocator.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/optional.hpp>


/**
 * This file is organized by return type:
 * - BucketDocument
 * - BSONObj
 * - write_ops::<>
 * - other
 */

namespace mongo::timeseries::write_ops_utils {

/**
 * Holds the bucket document used for writing to disk. The uncompressed bucket document is always
 * set. The compressed bucket document is also set unless compression fails.
 */
struct BucketDocument {
    BSONObj uncompressedBucket;
    boost::optional<BSONObj> compressedBucket;
    bool compressionFailed = false;
};

inline NamespaceString makeTimeseriesBucketsNamespace(const NamespaceString& nss) {
    return nss.isTimeseriesBucketsCollection() ? nss : nss.makeTimeseriesBucketsNamespace();
}

/**
 * Returns the document for writing a new bucket with a write batch.
 */
BucketDocument makeNewDocumentForWrite(
    const NamespaceString& nss,
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const BSONObj& metadata);

/**
 * Returns the document for writing a new bucket with 'measurements'. Calculates the min and max
 * fields while building the document.
 *
 * The measurements must already be known to fit in the same bucket. No checks will be done.
 */
BucketDocument makeNewDocumentForWrite(
    const NamespaceString& nss,
    const UUID& collectionUUID,
    const OID& bucketId,
    const std::vector<BSONObj>& measurements,
    const BSONObj& metadata,
    const TimeseriesOptions& options,
    const boost::optional<const StringDataComparator*>& comparator,
    const boost::optional<Date_t>& currentMinTime);

/**
 * Constructs a BSONColumn DocDiff entry.
 *
 * {
 *     o(ffset): Number,    // Offset into existing BSONColumn
 *     d(ata):   BinData    // Binary data to copy to existing BSONColumn
 * }
 */
BSONObj makeBSONColumnDocDiff(
    const BSONColumnBuilder<tracking::Allocator<void>>::BinaryDiff& binaryDiff);

BSONObj makeTimeseriesInsertCompressedBucketDocument(
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    const std::vector<
        std::pair<StringData, BSONColumnBuilder<tracking::Allocator<void>>::BinaryDiff>>&
        intermediates);

/**
 * Makes a write command request base and sets the statement Ids if provided a non-empty vector.
 */
mongo::write_ops::WriteCommandRequestBase makeTimeseriesWriteOpBase(std::vector<StmtId>&& stmtIds);

/**
 * Builds insert command request, as above, but expects StmtId's in WriteBatch.
 */
mongo::write_ops::InsertCommandRequest makeTimeseriesInsertOpFromBatch(
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const NamespaceString& bucketsNs);

/**
 * Builds update command request, as above, but expects StmtId's in WriteBatch.
 */
mongo::write_ops::UpdateCommandRequest makeTimeseriesCompressedDiffUpdateOpFromBatch(
    OperationContext* opCtx,
    std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
    const NamespaceString& bucketsNs);

/**
 * Returns an update request to the bucket when the 'measurements' is non-empty. Otherwise, returns
 * a delete request to the bucket.
 */
std::variant<mongo::write_ops::UpdateCommandRequest, mongo::write_ops::DeleteCommandRequest>
makeModificationOp(const OID& bucketId,
                   const CollectionPtr& coll,
                   const std::vector<BSONObj>& measurements,
                   const boost::optional<Date_t>& currentMinTime);

/**
 * Generates the compressed diff using the BSONColumnBuilders stored in the batch and the
 * intermediate() interface.
 */
mongo::write_ops::UpdateOpEntry makeTimeseriesCompressedDiffEntry(
    OperationContext* opCtx,
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    bool changedToUnsorted);

mongo::write_ops::UpdateCommandRequest makeTimeseriesTransformationOp(
    OperationContext* opCtx,
    const OID& bucketId,
    mongo::write_ops::UpdateModification::TransformFunc transformationFunc,
    const mongo::write_ops::InsertCommandRequest& request);

/**
 * Builds the transform update oplog entry with a transform function.
 */
mongo::write_ops::UpdateOpEntry makeTimeseriesTransformationOpEntry(
    OperationContext* opCtx,
    const OID& bucketId,
    mongo::write_ops::UpdateModification::TransformFunc transformationFunc);

/**
 * Builds the insert and update requests, as above, but expects StmtId's in WriteBatch.
 */
void makeWriteRequestFromBatch(OperationContext* opCtx,
                               std::shared_ptr<bucket_catalog::WriteBatch> batch,
                               const NamespaceString& bucketsNs,
                               std::vector<mongo::write_ops::InsertCommandRequest>* insertOps,
                               std::vector<mongo::write_ops::UpdateCommandRequest>* updateOps);

}  // namespace mongo::timeseries::write_ops_utils
