/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <set>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/shard_id.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {

/**
 * Given a simple BSON query, extracts the shard key corresponding to the key pattern from equality
 * matches in the query. The query expression *must not* be a complex query with sorts or other
 * attributes.
 *
 * Logically, the equalities in the BSON query can be serialized into a BSON document and then a
 * shard key is extracted from this equality document.
 *
 * NOTE: BSON queries and BSON documents look similar but are different languages. Use the correct
 * shard key extraction function.
 *
 * Returns !OK status if the query cannot be parsed.
 * Returns an empty BSONObj() if there is no shard key found in the query equalities.
 *
 * Examples:
 *  If the key pattern is { a : 1 }
 *   { a : "hi", b : 4 } --> returns { a : "hi" }
 *   { a : { $eq : "hi" }, b : 4 } --> returns { a : "hi" }
 *   { $and : [{a : { $eq : "hi" }}, { b : 4 }] } --> returns { a : "hi" }
 *  If the key pattern is { 'a.b' : 1 }
 *   { a : { b : "hi" } } --> returns { 'a.b' : "hi" }
 *   { 'a.b' : "hi" } --> returns { 'a.b' : "hi" }
 *   { a : { b : { $eq : "hi" } } } --> returns {} because the query language treats this as
 *                                                 a : { $eq : { b : ... } }
 */
StatusWith<BSONObj> extractShardKeyFromBasicQuery(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const ShardKeyPattern& shardKeyPattern,
                                                  const BSONObj& basicQuery);

/**
 * Variant of the above, which is used to parse queries that contain let parameters and runtime
 * constants.
 */
StatusWith<BSONObj> extractShardKeyFromBasicQueryWithContext(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const ShardKeyPattern& shardKeyPattern,
    const BSONObj& basicQuery);

BSONObj extractShardKeyFromQuery(const ShardKeyPattern& shardKeyPattern,
                                 const CanonicalQuery& query);

/**
 * Return an ordered list of bounds generated using a ShardKeyPattern and the bounds from the
 * IndexBounds. This function is used in sharding to determine where to route queries according to
 * the shard key pattern.
 *
 * Examples:
 *
 * Key { a: 1 }, Bounds a: [0] => { a: 0 } -> { a: 0 }
 * Key { a: 1 }, Bounds a: [2, 3) => { a: 2 } -> { a: 3 }  // bound inclusion ignored.
 *
 * The bounds returned by this function may be a superset of those defined by the constraints. For
 * instance, if this KeyPattern is {a : 1, b: 1} Bounds: { a : {$in : [1,2]} , b : {$in : [3,4,5]} }
 *         => {a : 1 , b : 3} -> {a : 1 , b : 5}, {a : 2 , b : 3} -> {a : 2 , b : 5}
 *
 * If the IndexBounds are not defined for all the fields in this keypattern, which means some fields
 * are unsatisfied, an empty BoundList could return.
 *
 */
BoundList flattenBounds(const ShardKeyPattern& shardKeyPattern, const IndexBounds& indexBounds);

/**
 * Transforms query into bounds for each field in the shard key.
 * For example :
 *   Key { a: 1, b: 1 },
 *   Query { a : { $gte : 1, $lt : 2 },
 *           b : { $gte : 3, $lt : 4 } }
 *   => Bounds { a : [1, 2), b : [3, 4) }
 */
IndexBounds getIndexBoundsForQuery(const BSONObj& key, const CanonicalQuery& canonicalQuery);

namespace shard_key_pattern_query_util {

struct QueryTargetingInfo {
    enum Description { kSingleKey, kMultipleKeys, kMinKeyToMaxKey };

    Description desc;
    std::set<ChunkRange> chunkRanges;
};

}  // namespace shard_key_pattern_query_util

/**
 * Populates 'shardIds' with the shard ids for a query with given filter and collation. If the
 * collation is empty, it uses the collection default collation for targeting. If 'info' is not
 * null, populates it with the ChunkRanges that the query targets and a description about whether
 * the query targets a single shard key value, multiple but not all shard key values or all shard
 * key values. If 'bypassIsFieldHashedCheck' is true, it skips checking if the shard key was hashed
 * and assumes that any non-collatable shard key was not hashed from a collatable type.
 */
void getShardIdsForQuery(boost::intrusive_ptr<ExpressionContext> expCtx,
                         const BSONObj& query,
                         const BSONObj& collation,
                         const ChunkManager& cm,
                         std::set<ShardId>* shardIds,
                         shard_key_pattern_query_util::QueryTargetingInfo* info = nullptr,
                         bool bypassIsFieldHashedCheck = false);

/**
 * Populates 'shardIds' with the shard ids for a query with given filter. If 'info' is
 * not null, populates it with the ChunkRanges that the query targets and a description about
 * whether the query targets a single shard key value, multiple but not all shard key values or all
 * shard key values.  If 'bypassIsFieldHashedCheck' is true, it skips checking if the shard key was
 * hashed and assumes that any non-collatable shard key was not hashed from a collatable type.
 */
void getShardIdsForCanonicalQuery(const CanonicalQuery& query,
                                  const ChunkManager& cm,
                                  std::set<ShardId>* shardIds,
                                  shard_key_pattern_query_util::QueryTargetingInfo* info = nullptr,
                                  bool bypassIsFieldHashedCheck = false);

}  // namespace mongo
