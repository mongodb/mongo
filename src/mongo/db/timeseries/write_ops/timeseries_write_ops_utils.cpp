// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    commandBase.setCollectionUUID(wholeOp.getCollectionUUID());

    return singleUpdateOp;
}

void assertTimeseriesBucketsCollectionNotFound(const mongo::NamespaceString& ns) {
    uasserted(ErrorCodes::NamespaceNotFound,
              str::stream() << "Buckets collection not found for time-series collection "
                            << (ns.isTimeseriesBucketsCollection()
                                    ? ns.getTimeseriesViewNamespace().toStringForErrorMsg()
                                    : ns.toStringForErrorMsg()));
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
