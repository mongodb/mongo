// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/shard_targeting_collation_helpers.h"

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
