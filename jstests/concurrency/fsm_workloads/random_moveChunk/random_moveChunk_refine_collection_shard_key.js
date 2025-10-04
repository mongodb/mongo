/**
 * Performs updates in transactions without the shard key while chunks are being moved. This
 * includes multi=true updates and multi=false updates with exact _id queries.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  uses_transactions,
 * ]
 */
import "jstests/libs/parallelTester.js";

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/random_moveChunk/random_moveChunk_base.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;

    // Number of documents per partition. Note that there is one chunk per partition and one
    // partition per thread.
    $config.data.partitionSize = 100;

    $config.data.defaultShardKeyField = "a";
    $config.data.defaultShardKey = {a: 1};

    // The variables used by the random_moveChunk_base config in order to move chunks.
    $config.data.shardKey = {a: 1};
    $config.data.newShardKey = {a: 1, b: 1};
    $config.data.newShardKeyFields = ["a", "b"];

    // Use a CountDownLatch as if it were a std::atomic<long long> shared between all of the
    // threads. The collection name is suffixed with the current this.latch.getCount() value
    // when concurrent CRUD operations are run against it. With every refineCollectionShardKey,
    // call this.latch.countDown() and run CRUD operations against the new collection suffixed
    // with this.latch.getCount(). This bypasses the need to drop and reshard the current
    // collection with every refineCollectionShardKey since it cannot be achieved in an atomic
    // fashion under the FSM infrastructure (meaning CRUD operations would fail).
    $config.data.latchCount = $config.iterations;
    $config.data.latch = new CountDownLatch($config.data.latchCount);

    $config.data.getCurrentLatchCollName = function (collName) {
        return collName + "_" + this.latch.getCount().toString();
    };

    $config.data.getCurrentOrPreviousLatchCollName = function (collName) {
        const latchNumber =
            Random.rand() < 0.5 ? this.latch.getCount() : Math.min(this.latch.getCount() + 1, this.latchCount);

        return collName + "_" + latchNumber.toString();
    };

    // Because updates don't have a shard filter stage, a migration may fail if a
    // broadcast update is operating on orphans from a previous migration in the range being
    // migrated back in. The particular error code is replaced with a more generic one, so this
    // is identified by the failed migration's error message.
    $config.data.isMoveChunkErrorAcceptable = (err) => {
        const codes = [
            ErrorCodes.ShardKeyNotFound,
            ErrorCodes.LockTimeout,
            // The refienCollectionCoordinator interrupt all migrations by setting `allowMigration`
            // to false
            ErrorCodes.Interrupted,
            ErrorCodes.OrphanedRangeCleanUpFailed,
        ];
        return (
            (err.code && codes.includes(err.code)) ||
            (err.message &&
                (err.message.includes("CommandFailed") ||
                    err.message.includes("Documents in target range may still be in use") ||
                    // This error will occur as a result of trying to move a chunk with a
                    // pre-refine collection epoch.
                    err.message.includes("collection may have been dropped") ||
                    // This error will occur if a moveChunk command has been sent with chunk
                    // boundaries that represent the pre-refine chunks, but the collection has
                    // already been changed to possess the post-refine chunk boundaries.
                    (err.message.includes("shard key bounds") &&
                        err.message.includes("are not valid for shard key pattern")) ||
                    (err.message.includes("bound") && err.message.includes("is not valid for shard key pattern"))))
        );
    };

    $config.states.refineCollectionShardKey = function refineCollectionShardKey(db, collName, connCache) {
        const latchCollName = this.getCurrentLatchCollName(collName);

        try {
            assert.commandWorked(
                db.adminCommand({
                    refineCollectionShardKey: db.getCollection(latchCollName).getFullName(),
                    key: this.newShardKey,
                }),
            );
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

    $config.states.moveChunk = function moveChunk(db, collName, connCache) {
        $super.states.moveChunk.apply(this, [db, this.getCurrentOrPreviousLatchCollName(collName), connCache]);
    };

    $config.states.init = function init(db, collName, connCache) {
        for (let i = this.latchCount; i >= 0; --i) {
            const latchCollName = collName + "_" + i;
            $super.states.init.apply(this, [db, latchCollName, connCache]);
        }
    };

    // Occasionally flush the router's cached metadata to verify the metadata for the refined
    // collections can be successfully loaded.
    $config.states.flushRouterConfig = function flushRouterConfig(db, collName, connCache) {
        assert.commandWorked(db.adminCommand({flushRouterConfig: db.getName()}));
    };

    $config.transitions = {
        init: {moveChunk: 0.4, refineCollectionShardKey: 0.4, flushRouterConfig: 0.2},
        moveChunk: {moveChunk: 0.4, refineCollectionShardKey: 0.4, flushRouterConfig: 0.2},
        refineCollectionShardKey: {moveChunk: 0.4, refineCollectionShardKey: 0.4, flushRouterConfig: 0.2},
        flushRouterConfig: {moveChunk: 0.5, refineCollectionShardKey: 0.5},
    };

    $config.setup = function setup(db, collName, cluster) {
        // Proactively create and shard all possible collections suffixed with this.latch.getCount()
        // that could receive CRUD operations over the course of the FSM workload. This prevents the
        // race that could occur between sharding a collection and creating an index on the new
        // shard key (if this step were done after every refineCollectionShardKey).
        for (let i = this.latchCount; i >= 0; --i) {
            const latchCollName = collName + "_" + i;
            let coll = db.getCollection(latchCollName);
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: this.defaultShardKey}));
            assert.commandWorked(coll.createIndex(this.newShardKey));
            $super.setup.apply(this, [db, latchCollName, cluster]);
        }
    };

    return $config;
});
