'use strict';

/**
 * Runs updateOne, deleteOne, and findAndModify without shard key against a sharded cluster while
 * the collection reshards concurrently.
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
load("jstests/libs/feature_flag_util.js");

var $config = extendWorkload($config, function($config, $super) {
    $config.startState = "init";

    // reshardingMinimumOperationDurationMillis is set to 30 seconds when there are stepdowns.
    // So in order to limit the overall time for the test, we limit the number of resharding
    // operations to maxReshardingExecutions.
    const maxReshardingExecutions = TestData.runningWithShardStepdowns ? 4 : $config.iterations;
    const customShardKeyFieldName = "customShardKey";

    $config.data.shardKeys = [];
    $config.data.currentShardKeyIndex = -1;
    $config.data.reshardingCount = 0;

    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);
        this.shardKeys.push({[this.defaultShardKeyField]: 1});
        this.shardKeys.push({[customShardKeyFieldName]: 1});
        this.currentShardKeyIndex = 0;
    };

    $config.data.generateRandomDocument = function generateRandomDocument(tid, partition) {
        const doc = $super.data.generateRandomDocument.apply(this, arguments);
        assert.neq(partition, null);
        doc[customShardKeyFieldName] = this.generateRandomInt(partition.lower, partition.upper - 1);
        return doc;
    };

    /**
     * Returns a random boolean.
     */
    $config.data.generateRandomBool = function generateRandomBool() {
        return Math.random() > 0.5;
    };

    $config.data.shouldSkipWriteResponseValidation = function shouldSkipWriteResponseValidation(
        res) {
        let shouldSkip = $super.data.shouldSkipWriteResponseValidation.apply(this, arguments);

        // This workload does in-place resharding so a retry that is sent
        // reshardingMinimumOperationDurationMillis after resharding completes is expected to fail
        // with IncompleteTransactionHistory.
        if (!shouldSkip && (res.code == ErrorCodes.IncompleteTransactionHistory)) {
            return res.errmsg.includes("Incomplete history detected for transaction");
        }

        return shouldSkip;
    };

    $config.states.reshardCollection = function reshardCollection(db, collName, connCache) {
        const collection = db.getCollection(collName);
        const ns = collection.getFullName();
        jsTestLog("Running reshardCollection state on: " + tojson(ns));

        if (this.tid === 0 && (this.reshardingCount <= maxReshardingExecutions)) {
            const newShardKeyIndex = (this.currentShardKeyIndex + 1) % this.shardKeys.length;
            const newShardKey = this.shardKeys[newShardKeyIndex];
            const reshardCollectionCmdObj = {
                reshardCollection: ns,
                key: newShardKey,
            };

            print(`Started resharding collection ${ns}: ${tojson({newShardKey})}`);
            if (TestData.runningWithShardStepdowns) {
                assert.soon(function() {
                    var res = db.adminCommand(reshardCollectionCmdObj);
                    if (res.ok) {
                        return true;
                    }
                    assert(res.hasOwnProperty("code"));

                    if (!FeatureFlagUtil.isEnabled(db, "PointInTimeCatalogLookups")) {
                        // Expected error prior to the PointInTimeCatalogLookups project.
                        if (res.code === ErrorCodes.SnapshotUnavailable) {
                            return true;
                        }
                    }

                    // Race to retry.
                    if (res.code === ErrorCodes.ReshardCollectionInProgress) {
                        return false;
                    }
                    // Unexpected error.
                    doassert(`Failed with unexpected ${tojson(res)}`);
                }, "Reshard command failed", 10 * 1000);
            } else {
                assert.commandWorked(db.adminCommand(reshardCollectionCmdObj));
            }
            print(`Finished resharding collection ${ns}: ${tojson({newShardKey})}`);

            // If resharding fails with SnapshotUnavailable, then this will be incorrect. But
            // its fine since reshardCollection will succeed if the new shard key matches the
            // existing one.
            this.currentShardKeyIndex = newShardKeyIndex;
            this.reshardingCount += 1;

            db.printShardingStatus();

            connCache.mongos.forEach(mongos => {
                if (this.generateRandomBool()) {
                    // Without explicitly refreshing mongoses, retries of retryable write statements
                    // would always be routed to the donor shards. Non-deterministically refreshing
                    // enables us to have test coverage for retrying against both the donor and
                    // recipient shards.
                    assert.commandWorked(mongos.adminCommand({flushRouterConfig: 1}));
                }
            });
        }
    };

    $config.transitions = {
        init: {reshardCollection: 0.3, updateOne: 0.3, deleteOne: 0.2, findAndModify: 0.2},
        updateOne: {reshardCollection: 0.3, updateOne: 0.3, deleteOne: 0.2, findAndModify: 0.2},
        deleteOne: {reshardCollection: 0.3, updateOne: 0.3, deleteOne: 0.2, findAndModify: 0.2},
        findAndModify: {reshardCollection: 0.3, updateOne: 0.3, deleteOne: 0.2, findAndModify: 0.2},
        reshardCollection:
            {reshardCollection: 0.3, updateOne: 0.3, deleteOne: 0.2, findAndModify: 0.2}
    };

    return $config;
});
