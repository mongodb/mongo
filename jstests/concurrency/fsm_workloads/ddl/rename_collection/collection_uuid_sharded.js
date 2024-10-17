/**
 * Tests running sharding operations with 'collectionUUID' parameter while the sharded collection is
 * being renamed concurrently.
 * @tags: [
 *   # balancer may move unsplittable collections and change the uuid
 *   assumes_balancer_off,
 *   # This test just performs rename operations that can't be executed in transactions.
 *   does_not_support_transactions,
 *   requires_non_retryable_writes,
 *   requires_fcv_60,
 *   requires_sharding,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig,
    testCommand
} from "jstests/concurrency/fsm_workloads/ddl/rename_collection/collection_uuid.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    const origStates = Object.keys($config.states);
    $config.states = Object.extend({
        shardingCommands: function shardingCommands(db, collName) {
            const namespace = db.getName() + "." + collName;

            // ShardCollection should fail as the collection is already sharded.
            let shardCollectionCmd = {
                shardCollection: namespace,
                key: {a: 1},
                collectionUUID: this.collUUID
            };
            testCommand(db, namespace, "shardCollection", shardCollectionCmd, this, [
                ErrorCodes.AlreadyInitialized
            ]);

            // Reshard with new shard-key.
            let reshardCollectionCmd = {
                reshardCollection: namespace,
                key: {a: 1},
                collectionUUID: this.collUUID,
                numInitialChunks: 1,
            };
            testCommand(db, namespace, "reshardCollection", reshardCollectionCmd, this);

            // Refine the shard-key.
            const refineCollectionCmd = {
                refineCollectionShardKey: namespace,
                key: {a: 1, b: 1},
                collectionUUID: this.collUUID
            };
            testCommand(db,
                        namespace,
                        "refineCollectionShardKey",
                        refineCollectionCmd,
                        this,
                        // The shard key might be changed to '_id' already by another thread.
                        [ErrorCodes.InvalidOptions]);

            // Reshard back with '_id' shard-key.
            reshardCollectionCmd = {
                reshardCollection: namespace,
                key: {_id: 1},
                collectionUUID: this.collUUID,
                numInitialChunks: 1,
            };
            testCommand(db, namespace, "reshardCollection", reshardCollectionCmd, this);
        }
    },
                                   $super.states);

    let newTransitions = Object.extend({}, $super.transitions);
    let indexCommandsState = {};
    origStates.forEach(function(state) {
        if (state === "indexCommands") {
            indexCommandsState = newTransitions[state];
        }

        if (state !== "init") {
            newTransitions[state]["shardingCommands"] = 0.2;
        }
    });
    newTransitions["shardingCommands"] = indexCommandsState;
    $config.transitions = newTransitions;

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, [db, collName]);

        // reshardCollcetion changes the collectionUUID.
        this.collUUIDFixed = false;
    };

    $config.threadCount = 5;
    $config.iterations = 50;

    return $config;
});
