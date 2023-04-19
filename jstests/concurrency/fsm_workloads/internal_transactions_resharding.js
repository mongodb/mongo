'use strict';

/**
 * Runs insert, update, delete and findAndModify commands against a sharded collection inside
 * single-shard and cross-shard internal transactions using all client session configurations, and
 * occasionally reshards the collection. Only runs on sharded clusters.
 *
 * @tags: [
 *  requires_fcv_60,
 *  requires_sharding,
 *  uses_transactions,
 *  antithesis_incompatible,
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/internal_transactions_sharded.js');
load('jstests/libs/fail_point_util.js');
load("jstests/libs/feature_flag_util.js");

var $config = extendWorkload($config, function($config, $super) {
    // reshardingMinimumOperationDurationMillis is set to 30 seconds when there are stepdowns.
    // So in order to limit the overall time for the test, we limit the number of resharding
    // operations to maxReshardingExecutions.
    const maxReshardingExecutions = TestData.runningWithShardStepdowns ? 4 : $config.iterations;

    const customShardKeyFieldName = "customShardKey";
    $config.data.shardKeys = [];
    $config.data.currentShardKeyIndex = -1;
    $config.data.reshardingCount = 0;

    $config.data.getQueryForDocument = function getQueryForDocument(doc) {
        // The query for a write command against a sharded collection must contain the shard key.
        const query = $super.data.getQueryForDocument.apply(this, arguments);
        query[customShardKeyFieldName] = doc[customShardKeyFieldName];
        return query;
    };

    $config.data.generateRandomDocument = function generateRandomDocument(tid, {partition} = {}) {
        const doc = $super.data.generateRandomDocument.apply(this, arguments);
        if (partition === undefined) {
            partition = this.partition;
        }
        assert.neq(partition, null);
        doc[customShardKeyFieldName] = this.generateRandomInt(partition.lower, partition.upper - 1);
        return doc;
    };

    $config.data.isAcceptableAggregateCmdError = function isAcceptableAggregateCmdError(res) {
        // The aggregate command is expected to involve running getMore commands which are not
        // retryable after a collection rename (done by resharding).
        return res && (res.code == ErrorCodes.QueryPlanKilled);
    };

    $config.data.isAcceptableRetryError = function isAcceptableRetryError(res) {
        // This workload does in-place resharding so a retry that is sent
        // reshardingMinimumOperationDurationMillis after resharding completes is expected to fail
        // with IncompleteTransactionHistory.
        return (res.code == ErrorCodes.IncompleteTransactionHistory) &&
            res.errmsg.includes("Incomplete history detected for transaction");
    };

    $config.states.init = function init(db, collName, connCache) {
        $super.states.init.apply(this, arguments);
        this.shardKeys.push({[this.defaultShardKeyField]: 1});
        this.shardKeys.push({[customShardKeyFieldName]: 1});
        this.currentShardKeyIndex = 0;
    };

    $config.states.reshardCollection = function reshardCollection(db, collName, connCache) {
        const collection = db.getCollection(collName);
        const ns = collection.getFullName();

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
        init: {
            internalTransactionForInsert: 0.25,
            internalTransactionForUpdate: 0.25,
            internalTransactionForDelete: 0.25,
            internalTransactionForFindAndModify: 0.25,
        },
        reshardCollection: {
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
            verifyDocuments: 0.2
        },
        internalTransactionForInsert: {
            reshardCollection: 0.2,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2
        },
        internalTransactionForUpdate: {
            reshardCollection: 0.2,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2
        },
        internalTransactionForDelete: {
            reshardCollection: 0.2,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2
        },
        internalTransactionForFindAndModify: {
            reshardCollection: 0.2,
            internalTransactionForInsert: 0.15,
            internalTransactionForUpdate: 0.15,
            internalTransactionForDelete: 0.15,
            internalTransactionForFindAndModify: 0.15,
            verifyDocuments: 0.2
        },
        verifyDocuments: {
            reshardCollection: 0.2,
            internalTransactionForInsert: 0.2,
            internalTransactionForUpdate: 0.2,
            internalTransactionForDelete: 0.2,
            internalTransactionForFindAndModify: 0.2,
        }
    };

    return $config;
});
