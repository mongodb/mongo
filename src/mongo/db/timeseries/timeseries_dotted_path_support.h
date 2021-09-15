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

#include <cstddef>

#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/multikey_paths.h"

namespace mongo {
namespace timeseries {
namespace dotted_path_support {
/**
 * Expands arrays and unpacks bucketed data along the specified path and adds all elements to the
 * 'elements' set.
 *
 * The 'path' can be specified using a dotted notation in order to traverse through embedded objects
 * and array elements. It must point to path like 'data.a.b', referring to the data that would be
 * stored at 'a.b' in each of the documents resulting from calling $_internalUnpackBucket on the
 * original document. Note that the caller should include the 'data' prefix, but omit the depth-2
 * numeric path entry that results from pivoting the data into the bucketed format.
 *
 * Other than the bucketing and unbucketing mentioned above, the function should be have roughly
 * like `mongo::dotted_path_support::extractAllElementsAlongPath'.
 *
 * An example:
 *
 *   Consider the document {data: {a: {0: {b: 1}, 2: {b: 2}}}} and the path "data.a.b". The elements
 *   {b: 1} and {b: 2}  would be added to the set. 'arrayComponents' would be set as
 *   std::set<size_t>{1U}.
 */
void extractAllElementsAlongBucketPath(const BSONObj& obj,
                                       StringData path,
                                       BSONElementSet& elements,
                                       bool expandArrayOnTrailingField = true,
                                       MultikeyComponents* arrayComponents = nullptr);

void extractAllElementsAlongBucketPath(const BSONObj& obj,
                                       StringData path,
                                       BSONElementMultiSet& elements,
                                       bool expandArrayOnTrailingField = true,
                                       MultikeyComponents* arrayComponents = nullptr);

/**
 * Finds arrays in individual metrics along the specified data path.
 *
 * The 'path' can be specified using a dotted notation in order to traverse through embedded objects
 * and array elements. It must point to path like 'data.a.b', referring to the data that would be
 * stored at 'a.b' in each of the documents resulting from calling $_internalUnpackBucket on the
 * original document. Note that the caller should include the 'data' prefix, but omit the depth-2
 * numeric path entry that results from pivoting the data into the bucketed format. That is, if the
 * caller is interested in the field `a.b` in the original user-facing document, they should specify
 * `data.a.b`, and not worry about the fact that the bucket actually contains entries like
 * `data.a.0.b` and `data.a.3.b`.
 *
 * In the example above, with 'data.a.b', the function will return true if any individual
 * measurement contained in the bucket has an array value for 'a' or for 'a.b', and false otherwise.
 */
bool haveArrayAlongBucketDataPath(const BSONObj& bucketObj, StringData path);

}  // namespace dotted_path_support
}  // namespace timeseries
}  // namespace mongo
