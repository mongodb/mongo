// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Given a shardKeyPattern, and the corresponding values extracted from a query specifying
 * equalities (see extractShardKeyFromQuery), extract the first element which is incompatible with
 * single shard targeting, because:
 *
 * * The value is affected by collation, and the query collation does not match the sharding
 * collation
 * * The field is hashed.
 *
 * For specific use cases (see cluster_find_and_modify_cmd targetSingleShard), hashed fields may not
 * prevent single shard targeting; such callers can set permitHashedFields.
 *
 * e.g.,
 *
 *  shardKeyPattern         : <any>
 *  shardKey                : <any>
 *  queryHasSimpleCollation : true
 *  permitHashedFields      : <any>
 *  -> eoo() : collation is simple (matches sharding collation), so all elements are compatible.
 *
 *  shardKeyPattern         : {a: 1, b: 1}
 *  shardKey                : {a: 123, b: null}
 *  queryHasSimpleCollation : false
 *  permitHashedFields      : <any>
 *  -> eoo() : no fields are affected by collation.
 *
 *  shardKeyPattern         : {a: 1, b: 1}
 *  shardKey                : {a: 123, b: "foobar"}
 *  queryHasSimpleCollation : false
 *  permitHashedFields      : <any>
 *  -> {b: "foobar"} : strings are affected by collation, and collation differs from sharding
 * collation.
 *
 *  shardKeyPattern         : {a: "hashed", b: 1}
 *  shardKey                : {a: 123, b: "foobar"}
 *  queryHasSimpleCollation : false
 *  permitHashedFields      : false
 *  -> {a: 123} : field a is hashed in shardKeyPattern
 *
 *  shardKeyPattern         : {a: "hashed", b: 1}
 *  shardKey                : {a: 123, b: "foobar"}
 *  queryHasSimpleCollation : false
 *  permitHashedFields      : true
 *  -> {b: "foobar"} : strings are affected by collation, and collation differs from sharding
 * collation, and hashed fields are allowed.
 *
 * Precondition: shardKey must contain values for all fields of shardKeyPattern.
 *
 * @return BSONElement identified element, or eoo() if none.
 */
[[MONGO_MOD_PUBLIC]] BSONElement getFirstFieldWithIncompatibleCollation(
    const BSONObj& shardKey,
    const ShardKeyPattern& shardKeyPattern,
    bool queryHasSimpleCollation,
    bool permitHashedFields);


/**
 * Given a shardKeyPattern, and the corresponding values extracted from a query specifying
 * equalities (see extractShardKeyFromQuery), check if the query can be targeted to a single shard.
 *
 * See getFirstFieldWithIncompatibleCollation() for examples. There must be no incompatible fields
 * for a query to be single shard targeted.
 *
 * Precondition: shardKey must contain values for all fields of shardKeyPattern.
 */
bool isSingleShardTargetable(const BSONObj& shardKey,
                             const ShardKeyPattern& shardKeyPattern,
                             bool queryHasSimpleCollation);
}  // namespace mongo
