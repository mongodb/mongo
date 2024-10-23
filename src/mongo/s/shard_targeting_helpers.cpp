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

#include "mongo/s/shard_targeting_helpers.h"

#include "mongo/db/query/collation/collation_index_key.h"


namespace mongo {
BSONElement getFirstFieldWithIncompatibleCollation(const BSONObj& shardKey,
                                                   const ShardKeyPattern& shardKeyPattern,
                                                   bool queryHasSimpleCollation,
                                                   bool permitHashedFields) {
    if (queryHasSimpleCollation) {
        return {};
    }

    for (BSONElement elt : shardKey) {
        // We must assume that if the field is specified as "hashed" in the shard key pattern,
        // then the hash value could have come from a collatable type.
        const bool isFieldHashed =
            (shardKeyPattern.isHashedPattern() &&
             shardKeyPattern.getHashedField().fieldNameStringData() == elt.fieldNameStringData());

        // If we want to skip the check in the special case where the _id field is hashed and
        // used as the shard key, set bypassIsFieldHashedCheck. This assumes that a request with
        // a query that contains an _id field can target a specific shard.
        if (CollationIndexKey::isCollatableType(elt.type()) ||
            (isFieldHashed && !permitHashedFields)) {
            return elt;
        }
    }
    return {};
}

bool isSingleShardTargetable(const BSONObj& shardKey,
                             const ShardKeyPattern& shardKeyPattern,
                             bool queryHasSimpleCollation) {
    return getFirstFieldWithIncompatibleCollation(
               shardKey, shardKeyPattern, queryHasSimpleCollation, false)
        .eoo();
}
}  // namespace mongo
