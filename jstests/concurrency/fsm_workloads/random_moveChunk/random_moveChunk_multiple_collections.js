/**
 * Perform continuous moveChunk on multiple collections/databases.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  does_not_support_add_remove_shards,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {fsm} from "jstests/concurrency/fsm_libs/fsm.js";
import {
    runWithManualRetries
} from "jstests/concurrency/fsm_workload_helpers/stepdown_suite_helpers.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/random_moveChunk/random_moveChunk_base.js";

const dbNames = ['db0', 'db1', 'db2'];
const collNames = ['collA', 'collB', 'collC'];

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.threadCount = dbNames.length * collNames.length;
    $config.iterations = 64;

    // Number of documents per partition. (One chunk per partition and one partition per thread).
    $config.data.partitionSize = 100;

    $config.states.moveChunk = function moveChunk(db, collName, connCache) {
        const dbName = dbNames[Random.randInt(dbNames.length)];
        db = db.getSiblingDB(dbName);
        collName = collNames[Random.randInt(collNames.length)];
        $super.states.moveChunk.apply(this, [db, collName, connCache]);
    };

    $config.states.init = function init(db, collName, connCache) {
        for (var i = 0; i < dbNames.length; i++) {
            const dbName = dbNames[i];
            db = db.getSiblingDB(dbName);
            for (var j = 0; j < collNames.length; j++) {
                collName = collNames[j];
                if (TestData.runningWithShardStepdowns) {
                    fsm.forceRunningOutsideTransaction(this);
                    runWithManualRetries(() => {
                        $super.states.init.apply(this, [db, collName, connCache]);
                    });
                } else {
                    $super.states.init.apply(this, [db, collName, connCache]);
                }
            }
        }
    };

    $config.transitions = {
        init: {moveChunk: 1.0},
        moveChunk: {moveChunk: 1.0},
    };

    $config.setup = function setup(db, collName, cluster) {
        const shards = Object.keys(cluster.getSerializedCluster().shards);
        const numShards = shards.length;
        // Initialize `dbNames.length` databases
        for (var i = 0; i < dbNames.length; i++) {
            const dbName = dbNames[i];
            db = db.getSiblingDB(dbName);
            db.adminCommand({enablesharding: dbName, primaryShard: shards[i % numShards]});
            // Initialize `collNames.length` sharded collections per db
            for (var j = 0; j < collNames.length; j++) {
                collName = collNames[j];
                const ns = dbName + '.' + collName;
                assert.commandWorked(db.adminCommand({shardCollection: ns, key: this.shardKey}));
                $super.setup.apply(this, [db, collName, cluster]);
            }
        }
    };

    return $config;
});
