/**
 * Tests that when resharding is in building-index phase, failover happens and resharding should
 * still work correctly.
 *
 * @tags: [
 *  requires_fcv_72,
 * ]
 */

import {runBuildingIndexFailover} from "jstests/sharding/libs/resharding_failover_helpers.js";

runBuildingIndexFailover({
    shardKeyPattern: {oldKey: 1},
    chunks: (donorShardNames, recipientShardNames) => [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
    newShardKeyPattern: {newKey: 1},
    newChunks: (donorShardNames, recipientShardNames) => [
        {min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]},
    ],
    documents: [
        {oldKey: 1, newKey: -1},
        {oldKey: 2, newKey: -2},
    ],
    indexKey: {oldKey: 1},
    newShardKeyIndexField: "newKey",
});
