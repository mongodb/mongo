'use strict';

/**
 * Runs updateOne, deleteOne, and findAndModify without shard key against a sharded cluster while
 * concurrently refining the collection's shard key.
 *
 * @tags: [
 *  featureFlagUpdateOneWithoutShardKey,
 *  requires_fcv_70,
 *  requires_sharding,
 *  uses_transactions,
 * ]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/write_without_shard_key_base.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.startState = "init";

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
            assertAlways.commandWorked(
                db.adminCommand({shardCollection: coll.getFullName(), key: this.defaultShardKey}));
            assertAlways.commandWorked(coll.createIndex(this.newShardKey));
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

            assertAlways.commandWorked(db.adminCommand(cmdObj));
        } catch (e) {
            // There is a race that could occur where two threads run refineCollectionShardKey
            // concurrently on the same collection. Since the epoch of the collection changes,
            // the later thread may receive a StaleEpoch error, which is an acceptable error.
            //
            // It is also possible to receive a LockBusy error if refineCollectionShardKey is unable
            // to acquire the distlock before timing out due to ongoing migrations acquiring the
            // distlock first.
            // TODO SERVER-68551: Remove lockbusy error since the balancer won't acquire anymore the
            // DDL lock for migrations
            if (e.code == ErrorCodes.StaleEpoch || e.code == ErrorCodes.LockBusy) {
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
        init:
            {refineCollectionShardKey: 0.25, updateOne: 0.25, deleteOne: 0.25, findAndModify: 0.25},
        updateOne: {
            refineCollectionShardKey: 0.2,
            updateOne: 0.2,
            deleteOne: 0.2,
            findAndModify: 0.2,
            flushRouterConfig: 0.2
        },
        deleteOne: {
            refineCollectionShardKey: 0.2,
            updateOne: 0.2,
            deleteOne: 0.2,
            findAndModify: 0.2,
            flushRouterConfig: 0.2
        },
        findAndModify: {
            refineCollectionShardKey: 0.2,
            updateOne: 0.2,
            deleteOne: 0.2,
            findAndModify: 0.2,
            flushRouterConfig: 0.2
        },
        refineCollectionShardKey: {
            refineCollectionShardKey: 0.2,
            updateOne: 0.2,
            deleteOne: 0.2,
            findAndModify: 0.2,
            flushRouterConfig: 0.2
        },
        flushRouterConfig:
            {refineCollectionShardKey: 0.25, updateOne: 0.25, deleteOne: 0.25, findAndModify: 0.25},
    };

    return $config;
});
