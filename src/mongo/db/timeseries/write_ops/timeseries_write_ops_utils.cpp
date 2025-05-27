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

#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils.h"

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils_internal.h"

namespace mongo::timeseries::write_ops {

mongo::write_ops::UpdateCommandRequest buildSingleUpdateOp(
    const mongo::write_ops::UpdateCommandRequest& wholeOp, size_t opIndex) {
    mongo::write_ops::UpdateCommandRequest singleUpdateOp(wholeOp.getNamespace(),
                                                          {wholeOp.getUpdates()[opIndex]});
    auto& commandBase = singleUpdateOp.getWriteCommandRequestBase();
    commandBase.setOrdered(wholeOp.getOrdered());
    commandBase.setBypassDocumentValidation(wholeOp.getBypassDocumentValidation());
    commandBase.setBypassEmptyTsReplacement(wholeOp.getBypassEmptyTsReplacement());

    return singleUpdateOp;
}

void assertTimeseriesBucketsCollectionNotFound(const mongo::NamespaceString& ns) {
    uasserted(ErrorCodes::NamespaceNotFound,
              str::stream() << "Buckets collection not found for time-series collection "
                            << ns.getTimeseriesViewNamespace().toStringForErrorMsg());
}

BSONObj makeBucketDocument(const std::vector<BSONObj>& measurements,
                           const NamespaceString& nss,
                           const UUID& collectionUUID,
                           const TimeseriesOptions& options,
                           const StringDataComparator* comparator) {
    tracking::Context trackingContext;
    auto res = uassertStatusOK(bucket_catalog::extractBucketingParameters(
        trackingContext, collectionUUID, options, measurements[0]));
    auto time = res.second;
    auto [oid, _] = bucket_catalog::generateBucketOID(time, options);
    write_ops_utils::BucketDocument bucketDoc =
        write_ops_utils::makeNewDocumentForWrite(nss,
                                                 collectionUUID,
                                                 oid,
                                                 measurements,
                                                 res.first.metadata.toBSON(),
                                                 options,
                                                 comparator,
                                                 boost::none);

    invariant(bucketDoc.compressedBucket);
    return *bucketDoc.compressedBucket;
}

}  // namespace mongo::timeseries::write_ops
