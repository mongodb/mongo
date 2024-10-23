/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/s/shard_key_pattern.h"

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
BSONElement getFirstFieldWithIncompatibleCollation(const BSONObj& shardKey,
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
