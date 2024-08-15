/**
 * Tests the updates into sharded time-series collection during a chunk migration.
 *
 * @tags: [
 *  requires_sharding,
 *  resource_intensive,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 *  featureFlagTimeseriesUpdatesSupport,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/random_moveChunk_timeseries_meta_updates.js';

const logCollection = "log_collection";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        // Drop and create a collection to log the number of arbitrary updates we perform.
        db[logCollection].drop();
        assert.commandWorked(db.createCollection(logCollection));
    };

    // Perform arbitrary updates on metric fields of measurements.
    $config.states.arbitraryUpdate = function(db, collName, connCache) {
        const shardedColl = db[collName];

        // Updates measurements by adding/incrementing a field.
        jsTestLog("Executing arbitrary update on: " + collName);
        const res =
            assert.commandWorked(shardedColl.update({}, {$inc: {"updateCount": 1}}, {multi: true}));

        // Log the number of updates performed.
        assert.commandWorked(db[logCollection].insert({updateCount: res.nModified}));
    };

    $config.data.validateCollection = function validate(db, collName) {
        $super.data.validateCollection.apply(this, arguments);

        // Check the sum of 'updateCount' against the logged values.
        const updateCountAggregation = {$group: {_id: null, count: {$sum: "$updateCount"}}};
        const totalUpdateCount = db[collName].aggregate(updateCountAggregation).toArray()[0].count;
        const loggedUpdateCount =
            db[logCollection].aggregate(updateCountAggregation).toArray()[0].count;

        assert.eq(totalUpdateCount, loggedUpdateCount, `Mismatch in update counts.`);
    };

    $config.transitions = {
        init: {insert: 1},
        insert: {insert: 0.2, moveChunk: 0.1, bucketLevelUpdate: 0.25, arbitraryUpdate: 0.45},
        bucketLevelUpdate:
            {insert: 0.2, moveChunk: 0.1, bucketLevelUpdate: 0.2, arbitraryUpdate: 0.5},
        arbitraryUpdate:
            {insert: 0.2, moveChunk: 0.1, bucketLevelUpdate: 0.2, arbitraryUpdate: 0.5},
        moveChunk: {insert: 0.2, moveChunk: 0.1, bucketLevelUpdate: 0.25, arbitraryUpdate: 0.45},
    };

    return $config;
});
