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

#include <cstdint>
#include <string>

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"

namespace mongo::timeseries::bucket_catalog {

/**
 * Function that should run validation against the bucket to ensure it's a proper bucket document.
 * Typically, this should execute Collection::checkValidation.
 */
using BucketDocumentValidator = std::function<std::pair<Collection::SchemaValidationResult, Status>(
    OperationContext*, const BSONObj&)>;

/**
 * Used to pass a bucket document into the BucketCatalog to reopen.
 */
struct BucketToReopen {
    BSONObj bucketDocument;
    BucketDocumentValidator validator;
    uint64_t catalogEra = 0;
};

/**
 * Communicates to the BucketCatalog whether an attempt was made to fetch a query or bucket, and
 * the resulting bucket document that was found, if any.
 */
struct BucketFindResult {
    bool fetchedBucket{false};
    bool queriedBucket{false};
    boost::optional<BucketToReopen> bucketToReopen{boost::none};
};

/**
 * Information of a Bucket that got archived while performing an operation on the BucketCatalog.
 */
struct ArchivedBucket {
    ArchivedBucket() = delete;
    ArchivedBucket(const BucketId& bucketId, const std::string& timeField);

    BucketId bucketId;
    std::string timeField;
};

}  // namespace mongo::timeseries::bucket_catalog
