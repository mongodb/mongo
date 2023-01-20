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

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_manager.h"

namespace mongo::timeseries::bucket_catalog {

/**
 * Information of a Bucket that got closed while performing an operation on this BucketCatalog.
 * The object is move-only--when it is destructed, it will notify the BucketCatalog that we are
 * done compressing the bucket (or have decided not to) and it can forget about the bucket's
 * state, making it eligible for reopening.
 */
class ClosedBucket {
public:
    ClosedBucket() = default;
    ~ClosedBucket();
    ClosedBucket(
        BucketStateManager*, const BucketId&, const std::string&, boost::optional<uint32_t>, bool);
    ClosedBucket(ClosedBucket&&);
    ClosedBucket& operator=(ClosedBucket&&);
    ClosedBucket(const ClosedBucket&) = delete;
    ClosedBucket& operator=(const ClosedBucket&) = delete;

    BucketId bucketId;
    std::string timeField;
    boost::optional<uint32_t> numMeasurements;
    bool eligibleForReopening = false;

private:
    BucketStateManager* _bucketStateManager = nullptr;
};
using ClosedBuckets = std::vector<ClosedBucket>;

}  // namespace mongo::timeseries::bucket_catalog
