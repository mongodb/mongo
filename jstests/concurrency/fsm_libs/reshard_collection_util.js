/**
 * Util functions around resharding in fsm workloads.
 */

/**
 * Run reshardCollection until reaching the expected times of resharding.
 * @param {Object} $config FSM workload config.
 * @param {*} db DB connection.
 * @param {String} collName Collection name to run resharding on.
 * @param {*} connCache connections to mongo.
 * @param {Boolean} sameKeyResharding Whether or not to do a same-key resharding.
 */
export function executeReshardCollection($config, db, collName, connCache, sameKeyResharding) {
    // reshardingMinimumOperationDurationMillis is set to 30 seconds when there are stepdowns.
    // So in order to limit the overall time for the test, we limit the number of resharding
    // operations to maxReshardingExecutions.
    const maxReshardingExecutions = TestData.runningWithShardStepdowns ? 4 : $config.iterations;
    const collection = db.getCollection(collName);
    const ns = collection.getFullName();
    jsTestLog("Running reshardCollection state on: " + tojson(ns));

    if ($config.tid === 0 && ($config.reshardingCount <= maxReshardingExecutions)) {
        const newShardKeyIndex = sameKeyResharding
            ? $config.currentShardKeyIndex
            : ($config.currentShardKeyIndex + 1) % $config.shardKeys.length;
        const newShardKey = $config.shardKeys[newShardKeyIndex];
        let reshardCollectionCmdObj = {
            reshardCollection: ns,
            key: newShardKey,
            numInitialChunks: 1
        };
        if (sameKeyResharding) {
            reshardCollectionCmdObj.forceRedistribution = true;
        }

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

        // If resharding fails with SnapshotUnavailable, then $config will be incorrect. But
        // its fine since reshardCollection will succeed if the new shard key matches the
        // existing one.
        $config.currentShardKeyIndex = newShardKeyIndex;
        $config.reshardingCount += 1;

        db.printShardingStatus();

        connCache.mongos.forEach(mongos => {
            if ($config.generateRandomBool()) {
                // Without explicitly refreshing mongoses, retries of retryable write statements
                // would always be routed to the donor shards. Non-deterministically refreshing
                // enables us to have test coverage for retrying against both the donor and
                // recipient shards.
                assert.commandWorked(mongos.adminCommand({flushRouterConfig: 1}));
            }
        });
    }
}
