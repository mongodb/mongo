// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::timeseries {

/**
 * Returns a compressed timeseries bucket in v2 format for a given uncompressed v1 bucket and time
 * field. The compressed bucket will have all measurements sorted by time. If
 * 'validateDecompression' is set to true we will validate that the bucket can be fully decompressed
 * without any data loss.
 */
struct CompressionResult {
    // The compressed bucket, boost::none if compression failed for any reason.
    boost::optional<BSONObj> compressedBucket;

    // How many times, in excess of one, subobject compression was started when compressing buckets.
    // Useful for statistics.
    int numInterleavedRestarts = 0;

    // Indicator if compression failed because we could not decompress without data loss.
    bool decompressionFailed = false;
};
CompressionResult compressBucket(const BSONObj& bucketDoc,
                                 std::string_view timeFieldName,
                                 const NamespaceString& nss,
                                 bool validateDecompression);

boost::optional<BSONObj> decompressBucket(const BSONObj& bucketDoc);

/**
 * Returns whether a timeseries bucket has been compressed to the v2 format.
 */
bool isCompressedBucket(const BSONObj& bucketDoc);

}  // namespace mongo::timeseries
