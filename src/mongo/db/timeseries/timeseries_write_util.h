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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_options.h"

namespace mongo::timeseries {
/**
 * Returns the document for writing a new bucket with a write batch.
 */
BSONObj makeNewDocumentForWrite(std::shared_ptr<timeseries::bucket_catalog::WriteBatch> batch,
                                const BSONObj& metadata);

/**
 * Returns the document for writing a new bucket with 'measurements'. Calculates the min and max
 * fields while building the document.
 *
 * The measurements must already be known to fit in the same bucket. No checks will be done.
 */
BSONObj makeNewDocumentForWrite(
    const OID& bucketId,
    const std::vector<BSONObj>& measurements,
    const BSONObj& metadata,
    const boost::optional<TimeseriesOptions>& options,
    const boost::optional<const StringData::ComparatorInterface*>& comparator);

/**
 * Performs modifications atomically for a user command on a time-series collection.
 * Replaces the bucket document for a partial bucket modification and removes the bucket for a full
 * bucket modification.
 *
 * Guarantees write atomicity per bucket document.
 */
Status performAtomicWrites(OperationContext* opCtx,
                           const CollectionPtr& coll,
                           const RecordId& recordId,
                           const stdx::variant<write_ops::UpdateCommandRequest,
                                               write_ops::DeleteCommandRequest>& modificationOp,
                           bool fromMigrate,
                           StmtId stmtId);
}  // namespace mongo::timeseries
