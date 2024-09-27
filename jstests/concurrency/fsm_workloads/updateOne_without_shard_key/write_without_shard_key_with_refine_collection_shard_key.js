/**
 * Runs updateOne, deleteOne, and findAndModify without shard key against a sharded cluster while
 * concurrently refining the collection's shard key.
 *
 * @tags: [
 *  requires_fcv_81,
 *  requires_sharding,
 *  uses_transactions,
 *  assumes_balancer_off,
 *  operations_longer_than_stepdown_interval_in_txns,
 * ]
 */
import "jstests/libs/parallelTester.js";

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from
    "jstests/concurrency/fsm_workloads/updateOne_without_shard_key/write_without_shard_key_base.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.startState = "init";
    $config.iterations = 25;
    $config.threadCount = 5;

    // Use a CountDownLatch as if it were a std::atomic<long long> shared between all of the
    // threads. The collection name is suffixed with the current this.latch.getCount() value
    // when concurrent CRUD operations are run against it. With every refineCollectionShardKey,
    // call this.latch.countDown() and run CRUD operations against the new collection suffixed
    // with this.latch.getCount(). This bypasses the need to drop and reshard the current
    // collection with every refineCollectionShardKey since it cannot be achieved in an atomic
    // fashion under the FSM infrastructure (meaning CRUD operations would fail).
    $config.data.latchCount = $config.iterations;
    $config.data.latch = new CountDownLatch($config.data.latchCount);

    $config.data.shardKey = {a: 1};
    $config.data.defaultShardKeyField = 'a';
    $config.data.defaultShardKey = {a: 1};

    // The variables used by the random_moveChunk_base config in order to move chunks.
    $config.data.newShardKey = {a: 1, b: 1};
    $config.data.newShardKeyFields = ["a", "b"];

    $config.setup = function setup(db, collName, cluster) {
        // Proactively create and shard all possible collections suffixed with this.latch.getCount()
        // that could receive CRUD operations over the course of the FSM workload. This prevents the
        // race that could occur between sharding a collection and creating an index on the new
        // shard key (if this step were done after every refineCollectionShardKey).
        for (let i = this.latchCount; i >= 0; --i) {
            const latchCollName = collName + '_' + i;
            let coll = db.getCollection(latchCollName);
            assert.commandWorked(
                db.adminCommand({shardCollection: coll.getFullName(), key: this.defaultShardKey}));
            assert.commandWorked(coll.createIndex(this.newShardKey));
            $super.setup.apply(this, [db, latchCollName, cluster]);
        }
    };

    // Occasionally flush the router's cached metadata to verify the metadata for the refined
    // collections can be successfully loaded.
    $config.states.flushRouterConfig = function flushRouterConfig(db, collName, connCache) {
        jsTestLog("Running flushRouterConfig state");
        assert.commandWorked(db.adminCommand({flushRouterConfig: db.getName()}));
    };

    $config.data.getCurrentLatchCollName = function(collName) {
        return collName + '_' + this.latch.getCount().toString();
    };

    $config.states.refineCollectionShardKey = function refineCollectionShardKey(
        db, collName, connCache) {
        jsTestLog("Running refineCollectionShardKey state.");
        const latchCollName = this.getCurrentLatchCollName(collName);

        try {
            const cmdObj = {
                refineCollectionShardKey: db.getCollection(latchCollName).getFullName(),
                key: this.newShardKey
            };

            assert.commandWorked(db.adminCommand(cmdObj));
        } catch (e) {
            // There is a race that could occur where two threads run refineCollectionShardKey
            // concurrently on the same collection. Since the epoch of the collection changes,
            // the later thread may receive a StaleEpoch error, which is an acceptable error.
            if (e.code == ErrorCodes.StaleEpoch) {
                print("Ignoring acceptable refineCollectionShardKey error: " + tojson(e));
                return;
            }
            throw e;
        }

        this.shardKeyField[latchCollName] = this.newShardKeyFields;
        this.latch.countDown();
    };

    $config.states.findAndModify = function findAndModify(db, collName, connCache) {
        $super.states.findAndModify.apply(this,
                                          [db, this.getCurrentLatchCollName(collName), connCache]);
    };

    $config.states.updateOne = function findAndModify(db, collName, connCache) {
        $super.states.updateOne.apply(this,
                                      [db, this.getCurrentLatchCollName(collName), connCache]);
    };

    $config.states.deleteOne = function findAndModify(db, collName, connCache) {
        $super.states.deleteOne.apply(this,
                                      [db, this.getCurrentLatchCollName(collName), connCache]);
    };

    $config.transitions = {
        init: {updateOne: 0.33, deleteOne: 0.33, findAndModify: 0.34},
        updateOne: {
            refineCollectionShardKey: 0.05,
            updateOne: 0.3,
            deleteOne: 0.3,
            findAndModify: 0.3,
            flushRouterConfig: 0.05
        },
        deleteOne: {
            refineCollectionShardKey: 0.05,
            updateOne: 0.3,
            deleteOne: 0.3,
            findAndModify: 0.3,
            flushRouterConfig: 0.05
        },
        findAndModify: {
            refineCollectionShardKey: 0.05,
            updateOne: 0.3,
            deleteOne: 0.3,
            findAndModify: 0.3,
            flushRouterConfig: 0.05
        },
        refineCollectionShardKey: {
            refineCollectionShardKey: 0.05,
            updateOne: 0.3,
            deleteOne: 0.3,
            findAndModify: 0.3,
            flushRouterConfig: 0.05
        },
        flushRouterConfig: {updateOne: 0.33, deleteOne: 0.33, findAndModify: 0.34},
    };

    return $config;
});
