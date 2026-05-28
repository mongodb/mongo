/**
 * Simulates a failover prior to removing the recipient doc while resharding is aborting from an
 * unrecoverable error on the donor. Resharding should abort successfully after stepUp.
 *
 * See BF-32038 for more details.
 * @tags: [
 *  requires_fcv_80,
 * ]
 */
import {runFailoverDuringAbort} from "jstests/sharding/libs/resharding_failover_helpers.js";

runFailoverDuringAbort({
    shardKeyPattern: {oldKey: 1},
    chunks: (donorShardNames, recipientShardNames) => [
        {min: {oldKey: MinKey}, max: {oldKey: 10}, shard: donorShardNames[0]},
        {min: {oldKey: 10}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
    newShardKeyPattern: {newKey: 1},
    newChunks: (donorShardNames, recipientShardNames) => [
        {min: {newKey: MinKey}, max: {newKey: 10}, shard: recipientShardNames[0]},
        {min: {newKey: 10}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
    ],
    getRecipientDocNs: (coll) => coll.getFullName(),
});
