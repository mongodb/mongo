/**
 * Tests the updates into sharded time-series collection during a chunk migration.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 *  requires_fcv_51,
 * ]
 */
import {assertAlways} from "jstests/concurrency/fsm_libs/assert.js";
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/random_moveChunk_timeseries_inserts.js';
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

const numValues = 10;
const logCollection = "log_collection";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        // Drop and create a collection to log the number of arbitrary updates we perform.
        db[logCollection].drop();
        assert.commandWorked(db.createCollection(logCollection));
    };

    $config.data.generateMetaFieldValueForInitialInserts = () => {
        let meta = {};
        // Insert a document with a field for every thread to test concurrent updates on the
        // same document.
        for (let i = 0; i < $config.threadCount; i++) {
            meta["tid" + i] = Random.randInt(numValues);
        }
        return meta;
    };

    $config.data.generateMetaFieldValueForInsertStage = (tid) => {
        return {["tid" + tid]: Random.randInt(numValues)};
    };

    // Perform bucket level updates by updating the meta field of measurements.
    $config.states.bucketLevelUpdate = function(db, collName, connCache) {
        const shardedColl = db[collName];
        const updateField = this.metaField + ".tid" + this.tid;
        const oldValue = Random.randInt(numValues);

        jsTestLog("Executing bucket level update on: " + collName + " on field '" + updateField +
                  "'");
        assertAlways.commandWorked(shardedColl.update(
            {[updateField]: {$gte: oldValue}}, {$inc: {[updateField]: 1}}, {multi: true}));
    };

    // Perform arbitrary updates on metric fields of measurements.
    $config.states.arbitraryUpdate = function(db, collName, connCache) {
        if (TimeseriesTest.arbitraryUpdatesEnabled(db)) {
            const shardedColl = db[collName];

            // Updates measurements by adding/incrementing a field.
            jsTestLog("Executing arbitrary update on: " + collName);
            const res = assert.commandWorked(
                shardedColl.update({}, {$inc: {"updateCount": 1}}, {multi: true}));

            // Log the number of updates performed.
            assert.commandWorked(db[logCollection].insert({updateCount: res.nModified}));
        }
    };

    $config.data.validateCollection = function validate(db, collName) {
        // Since we can't use a 'snapshot' read concern for timeseries updates, updates on the
        // sharded collection may not see the exact same records as the non-sharded, so the
        // validation needs to be more lenient.
        const pipeline = [{$project: {_id: "$_id"}}, {$sort: {_id: 1}}];
        const diff = DataConsistencyChecker.getDiff(db[collName].aggregate(pipeline),
                                                    db[this.nonShardCollName].aggregate(pipeline));
        assertAlways.eq(
            diff, {docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []});

        if (TimeseriesTest.arbitraryUpdatesEnabled(db)) {
            // Check the sum of 'updateCount' against the logged values.
            const updateCountAggregation = {$group: {_id: null, count: {$sum: "$updateCount"}}};
            const totalUpdateCount =
                db[collName].aggregate(updateCountAggregation).toArray()[0].count;
            const loggedUpdateCount =
                db[logCollection].aggregate(updateCountAggregation).toArray()[0].count;

            assert.eq(totalUpdateCount, loggedUpdateCount, `Mismatch in update counts.`);
        }
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

    // Reduced iteration and document counts to avoid timeouts.
    $config.iterations = 20;

    // Five minutes.
    $config.data.increment = 1000 * 60 * 5;

    // This should generate documents for a span of one month.
    $config.data.numInitialDocs = 12 * 24 * 30;

    return $config;
});
