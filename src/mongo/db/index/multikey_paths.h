// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <string>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo {

// If non-empty, a vector with size equal to the number of elements in the index key pattern. Each
// element in the vector is an ordered set of positions (starting at 0) into the corresponding
// indexed field that represent what prefixes of the indexed field cause the index to be multikey.
//
// For example, with the index {'a.b': 1, 'a.c': 1} where the paths "a" and "a.b" cause the
// index to be multikey, we'd have a std::vector<std::set<size_t>>{{0U, 1U}, {0U}}.
//
// Further Examples:
// Index                  PathsThatAreMultiKey  MultiKeyPaths
// --------------------   --------------------  --------------------
// {'a.b': 1, 'a.c': 1}   "a", "a.b"            {{0U, 1U}, {0U}}
// {a: 1, b: 1}           "b"                   {{}, {0U}}
// {a: 1, b: 1}           "a"                   {{0U}, {}}
// {'a.b.c': 1, d: 1}     "a.b.c"               {{2U}, {}}
// {'a.b': 1, c: 1, d: 1} "a.b", "d"            {{1U}, {}, {0U}}
// {a: 1, b: 1}           none                  {{}, {}}
// {a: 1, b: 1}           no multikey metadata  {}
//
// Use small_vector as data structure to be able to store a few multikey components and paths
// without needing to allocate memory. This optimizes for the common case.
constexpr std::size_t kFewMultikeyComponents = 4;
using MultikeyComponents = boost::container::flat_set<
    BSONDepthIndex,
    std::less<BSONDepthIndex>,
    boost::container::small_vector<BSONDepthIndex, kFewMultikeyComponents>>;
// An empty vector is used to represent that the index doesn't support path-level multikey tracking.
constexpr std::size_t kFewCompoundIndexFields = 4;
using MultikeyPaths = boost::container::small_vector<MultikeyComponents, kFewCompoundIndexFields>;

namespace multikey_paths {

std::string toString(const MultikeyPaths& paths);

std::string multikeyComponentsToString(const MultikeyComponents& paths);

/**
 * Encodes 'paths' as binary data and appends it to 'builder'.
 *
 * For example, consider the index {'a.b': 1, 'a.c': 1} where the paths "a" and "a.b" cause it to be
 * multikey. The object {'a.b': HexData('0101'), 'a.c': HexData('0100')} would then be appended to
 * 'builder'.
 */
void serialize(const BSONObj& keyPattern, const MultikeyPaths& paths, BSONObjBuilder& builder);

/**
 * Returns 'multikeyPaths' encoded as binary data into BSON.
 *
 * For example, consider the index {'a.b': 1, 'a.c': 1} where the paths "a" and "a.b" cause it to be
 * multikey. The object {'a.b': HexData('0101'), 'a.c': HexData('0100')} would then be returned.
 */
BSONObj serialize(const BSONObj& keyPattern, const MultikeyPaths& paths);

/**
 * Returns the path-level multikey information encoded as binary data in 'obj'.
 *
 * For example, consider the index {'a.b': 1, 'a.c': 1} where the paths "a" and "a.b" cause it to be
 * multikey. The binary data {'a.b': HexData('0101'), 'a.c': HexData('0100')} would then be parsed
 * into std::vector<std::set<size_t>>{{0U, 1U}, {0U}}.
 */
StatusWith<MultikeyPaths> parse(const BSONObj& obj);

}  // namespace multikey_paths
}  // namespace mongo
