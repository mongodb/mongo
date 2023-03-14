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

#include "mongo/db/timeseries/bucket_catalog/reopening.h"

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"

namespace mongo::timeseries::bucket_catalog {

ArchivedBucket::ArchivedBucket(const BucketId& b, const std::string& t)
    : bucketId{b}, timeField{t} {}

long long marginalMemoryUsageForArchivedBucket(const ArchivedBucket& bucket,
                                               bool onlyEntryForMatchingMetaHash) {
    return sizeof(Date_t) +        // key in set of archived buckets for meta hash
        sizeof(ArchivedBucket) +   // main data for archived bucket
        bucket.timeField.size() +  // allocated space for timeField string, ignoring SSO
        (onlyEntryForMatchingMetaHash ? sizeof(std::size_t) +           // key in set (meta hash)
                 sizeof(decltype(Stripe::archivedBuckets)::value_type)  // set container
                                      : 0);
}

}  // namespace mongo::timeseries::bucket_catalog
