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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/platform/compiler.h"

#include <iosfwd>

#include <boost/any.hpp>
#include <boost/optional/optional.hpp>

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
 * Other than the bucketing and unbucketing mentioned above, and the return value, the function
 * should behave roughly like `mongo::dotted_path_support::extractAllElementsAlongPath'.
 *
 * In the case that the input bucket has been compressed, the function may need to decompress the
 * data in order to examine it and extract the requested data. Since the data is returned as
 * BSONElement, which does not own the data it references, we will need to provide storage for the
 * decompressed data that will outlive the function call so that the returned BSONElements remain
 * valid. This storage is provided via the optional return value. It need not be examined directly,
 * but it should not be discarded until after the extracted elements.
 *
 * An example:
 *
 *   Consider the document {data: {a: {0: {b: 1}, 2: {b: 2}}}} and the path "data.a.b". The elements
 *   {b: 1} and {b: 2}  would be added to the set. 'arrayComponents' would be set as
 *   std::set<size_t>{1U}.
 */
[[nodiscard]] boost::optional<BSONColumn> extractAllElementsAlongBucketPath(
    const BSONObj& obj,
    StringData path,
    BSONElementSet& elements,
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

// Indicates the truthy outcome of a decision-making process. The 'Undecided' value is for internal
// use only, and should not be returned to a caller outside this namespace.
enum class Decision { Yes, Maybe, No, Undecided };
std::ostream& operator<<(std::ostream& s, const Decision& i);

/**
 * Identifies if measurements in the given bucket contains array data in 'userField'.
 *
 * This function is meant to be used as an optimized check in indexing to see if we must examine the
 * full 'data.'-prefixed field contents. The function guarantees that if it returns Yes there is
 * definitely an array, and if it returns No there is definitely not an array. If it returns Maybe,
 * the caller must scan the actual data. It should never return Undecided.
 *
 * The 'userField' should be specified without any bucket field prefix. That is, if the
 * user defines an index on 'a.b.c', 'userField' should be 'a.b.c' and not 'data.a.b.c',
 * 'control.min.a.b.c', etc.
 */
Decision fieldContainsArrayData(const BSONObj& bucketObj, StringData userField);
}  // namespace dotted_path_support
}  // namespace timeseries
}  // namespace mongo
