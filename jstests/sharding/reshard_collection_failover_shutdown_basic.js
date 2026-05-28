/**
 * Tests that reshardCollection succeeds when a participant experiences a failover or clean/unclean
 * restart during the operation.
 * Multiversion testing does not support tests that kill and restart nodes. So we had to add the
 * 'multiversion_incompatible' tag.
 * @tags: [
 *   uses_atclustertime,
 *   multiversion_incompatible,
 *   requires_persistence,
 * ]
 */
import {runFailoverShutdownBasic} from "jstests/sharding/libs/resharding_failover_helpers.js";

runFailoverShutdownBasic({
    shardKeyPattern: {oldKey: 1},
    chunks: (donorShardNames, recipientShardNames) => [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
    newShardKeyPattern: {newKey: 1},
    newChunks: (donorShardNames, recipientShardNames) => [
        {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
        {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
    ],
    documents: [
        {_id: "stays on shard0", oldKey: -10, newKey: -10},
        {_id: "moves to shard0", oldKey: 10, newKey: -10},
        {_id: "moves to shard1", oldKey: -10, newKey: 10},
        {_id: "stays on shard1", oldKey: 10, newKey: 10},
    ],
});
