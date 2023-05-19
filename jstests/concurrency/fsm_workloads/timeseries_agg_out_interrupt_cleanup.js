'use strict';
/**
 * Tests $out stage of aggregate command with time-series collections concurrently with killOp.
 * Ensures that all the temporary collections created during the aggregate command are deleted and
 * that all buckets collection have a corresponding view. This workloads extends
 * 'agg_out_interrupt_cleanup'.
 *
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_transactions,
 *   does_not_support_stepdowns,
 *   uses_curop_agg_stage,
 *   requires_fcv_71,
 *   featureFlagAggOutTimeseries
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/agg_base.js');    // for $config
load(
    'jstests/concurrency/fsm_workloads/agg_out_interrupt_cleanup.js');  // for killOpsMatchingFilter

var $config = extendWorkload($config, function($config, $super) {
    const timeFieldName = 'time';
    const metaFieldName = 'tag';
    const numDocs = 100;

    $config.states.aggregate = function aggregate(db, collName) {
        // drop the view to ensure that each time a buckets collection is made, the view will also
        // be made or both be destroyed.
        assert(db["interrupt_temp_out"].drop());
        // $out to the same collection so that concurrent aggregate commands would cause congestion.
        db[collName].runCommand({
            aggregate: collName,
            pipeline: [{
                $out: {
                    db: db.getName(),
                    coll: "interrupt_temp_out",
                    timeseries: {timeField: timeFieldName, metaField: metaFieldName}
                }
            }],
            cursor: {}
        });
    };

    $config.states.killOp = function killOp(db, collName) {
        // The aggregate command could be running different commands internally (renameCollection,
        // insertDocument, etc.) depending on which stage of execution it is in. So, get all the
        // operations that are running against the input, output or temp collections.
        $super.data.killOpsMatchingFilter(db, {
            op: "command",
            active: true,
            $or: [
                {"ns": db.getName() + ".interrupt_temp_out"},  // For the view.
                {"ns": db.getName() + "." + collName},         // For input collection.
                // For the tmp collection.
                {"ns": {$regex: "^" + db.getName() + "\.system.buckets\.tmp\.agg_out.*"}}
            ],
            "command.drop": {
                $exists: false
            }  // Exclude 'drop' command from the filter to make sure that we don't kill the the
            // drop command which is responsible for dropping the temporary collection.
        });
    };

    $config.teardown = function teardown(db) {
        const collNames = db.getCollectionNames();
        // Ensure that a temporary collection is not left behind.
        assertAlways.eq(
            collNames.filter(coll => coll.includes('system.buckets.tmp.agg_out')).length, 0);

        // Ensure that for the buckets collection there is a corresponding view.
        assertAlways(!(collNames.includes('system.buckets.interrupt_temp_out') &&
                       !collNames.includes('interrupt_temp_out')));
    };

    /**
     * Create a time-series collection and insert 100 documents.
     */
    $config.setup = function setup(db, collName, cluster) {
        db[collName].drop();
        assert.commandWorked(db.createCollection(
            collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
        const docs = [];
        for (let i = 0; i < numDocs; ++i) {
            docs.push({
                [timeFieldName]: ISODate(),
                [metaFieldName]: (this.tid * numDocs) + i,
            });
        }
        assert.commandWorked(db.runCommand({insert: collName, documents: docs, ordered: false}));
    };

    return $config;
});
