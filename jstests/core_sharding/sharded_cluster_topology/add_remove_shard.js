/*
 * Basic coverage of the addShard and removeShard command; the test restores the original
 * topology of the cluster before completing.
 *
 * @tags: [
 *  # A single-sharded cluster cannot accept a removeShard command
 *  requires_2_or_more_shards,
 *  # Test cases assume a stable number of shards
 *  assumes_stable_shard_list,
 *  # SERVER-99344 remove the following exclusion tag
 *  does_not_support_stepdowns,
 * ]
 */

import {getShardDescriptors, getShardNames} from 'jstests/libs/sharded_cluster_fixture_helpers.js';
import {removeShard} from 'jstests/sharding/libs/remove_shard_util.js';

const originalShardDescriptors = (() => {
    Random.setRandomSeed(1);
    let descriptors = Array.shuffle(getShardDescriptors(db));

    // In case of config shard, ensure that this will be the last standing one.
    let configShardIdx = descriptors.findIndex(descriptor => descriptor._id === 'config');
    if (configShardIdx !== -1 && configShardIdx !== descriptors.length - 1) {
        const tmp = descriptors[configShardIdx];
        descriptors[configShardIdx] = descriptors[descriptors.length - 1];
        descriptors[descriptors.length - 1] = tmp;
    }
    return descriptors;
})();

const originalShardNames = originalShardDescriptors.map(descriptor => descriptor._id);
const lastStandingShardId = originalShardNames[originalShardNames.length - 1];

jsTest.log('Testing cluster downscaling until reaching the minimum amount of shards...');
{
    // Multiple removeShard operations may be started in parallel.
    for (let i = 0; i < originalShardNames.length - 1; ++i) {
        const shardToRemove = originalShardNames[i];
        const shardListBeforeRemoval = getShardNames(db);

        // Create a new database and assign it to the soon-to-be removed shard.
        assert.commandWorked(
            db.adminCommand({enableSharding: `${jsTestName()}`, primaryShard: shardToRemove}));

        const removeShardResponseUponStart =
            assert.commandWorked(db.adminCommand({removeShard: shardToRemove}));
        assert.contains(
            removeShardResponseUponStart.state,
            ['started', 'ongoing'],
            `Unexpected state reported by removeShard: ${tojson(removeShardResponseUponStart)}`);
        assert.gte(
            removeShardResponseUponStart.dbsToMove.length,
            1,
            `Unexpected state reported by removeShard: ${tojson(removeShardResponseUponStart)}`);

        // The presence of a database with the shard being removed as primary prevents the operation
        // from being completed.
        const removeShardResponseWithPendingActions =
            assert.commandWorked(db.adminCommand({removeShard: shardToRemove}));
        assert.eq('ongoing', removeShardResponseWithPendingActions.state);
        assert.eq(removeShardResponseUponStart.dbsToMove,
                  removeShardResponseWithPendingActions.dbsToMove);

        assert.sameMembers(
            shardListBeforeRemoval,
            getShardNames(db),
            `Unexpected response of listShard after starting the removal of ${shardToRemove}`);

        // Drop the DB to unblock the completion of the shard removal upon the next command
        // invocation.
        for (let dbName of removeShardResponseUponStart.dbsToMove) {
            assert.commandWorked(db.getSiblingDB(dbName).dropDatabase());
        }
    }

    // The removeShard operation on the last operating shard cannot be started.
    assert.commandFailedWithCode(db.adminCommand({removeShard: lastStandingShardId}),
                                 ErrorCodes.IllegalOperation,
                                 'Downscale of the cluster to 0 shards succeeded unexpectedly');

    // Complete each pending shard removal through the stepdown-resilient helper function and verify
    // post-conditions.
    for (let i = 0; i < originalShardNames.length - 1; ++i) {
        const shardToRemove = originalShardNames[i];
        removeShard(db, shardToRemove);

        const shardListAfterRemoval = getShardNames(db);
        assert.sameMembers(
            shardListAfterRemoval,
            originalShardNames.slice(i + 1),
            `Unexpected response of listShard after completing the removal of ${shardToRemove}`);

        // Once removed, the shard should not be target-able anymore.
        assert.commandFailedWithCode(db.adminCommand({removeShard: shardToRemove}),
                                     ErrorCodes.ShardNotFound,
                                     'Removal on non-existing shard succeeded unexpectedly');
    }

    // A single-sharded cluster cannot be downscaled any further.
    assert.commandFailedWithCode(db.adminCommand({removeShard: lastStandingShardId}),
                                 ErrorCodes.IllegalOperation,
                                 'Downscale of the cluster to 0 shards succeeded unexpectedly');
}

jsTest.log('Testing cluster upscaling (and restoring its original topology)...');
{
    for (let i = originalShardDescriptors.length - 2; i >= 0; --i) {
        const shardToAdd = originalShardDescriptors[i];
        assert.commandWorked(db.adminCommand({addShard: shardToAdd.host, name: shardToAdd._id}));

        // Repeat the command and verify that is a no-op.
        assert.commandWorked(db.adminCommand({addShard: shardToAdd.host, name: shardToAdd._id}),
                             'addShard is an idempotent operation');
        const shardListAfterAddition = getShardNames(db);
        assert.sameMembers(shardListAfterAddition,
                           originalShardNames.slice(i),
                           `Unexpected response of listShard after re-adding ${shardToAdd}`);
    }
}
