/**
 * Tests that if a reshardCollection command with a user-provided reshardingUUID is completed,
 * then after failover the result is available to retries.
 *
 * @tags: [
 *   requires_fcv_72,
 *   uses_atclustertime,
 * ]
 */

import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {runRetryAfterFailover} from "jstests/sharding/libs/resharding_failover_helpers.js";

runRetryAfterFailover({
    shardKeyPattern: {oldKey: 1},
    chunks: (donorShardNames, recipientShardNames) => [
        {min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]},
    ],
    newShardKeyPattern: {newKey: 1},
    newChunks: (donorShardNames, recipientShardNames) => [
        {min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]},
    ],
    getAbortNs: (coll) => coll.getFullName(),
    getCollUUID: (db, coll) => getUUIDFromListCollections(db, coll.getName()),
});
