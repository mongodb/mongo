/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/timeseries/timeseries_gen.h"

namespace mongo {

namespace timeseries {

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
                                 StringData timeFieldName,
                                 const NamespaceString& nss,
                                 bool eligibleForReopening,
                                 bool validateDecompression);

boost::optional<BSONObj> decompressBucket(const BSONObj& bucketDoc);

/**
 * Returns whether a timeseries bucket has been compressed to the v2 format.
 */
bool isCompressedBucket(const BSONObj& bucketDoc);

}  // namespace timeseries
}  // namespace mongo
