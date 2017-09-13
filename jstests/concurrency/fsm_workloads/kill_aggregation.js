'use strict';

/**
 * kill_aggregation.js
 *
 * Tests that the aggregation system correctly halts its planning to determine whether the query
 * system can provide a non-blocking sort or can provide a covered projection when a catalog
 * operation occurs.
 *
 * This workload was designed to reproduce SERVER-25039.
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');      // for extendWorkload
load('jstests/concurrency/fsm_workloads/kill_rooted_or.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    // Use the workload name as the collection name, since the workload name is assumed to be
    // unique.
    $config.data.collName = 'kill_aggregation';

    $config.states.query = function query(db, collName) {
        var res = db.runCommand({
            aggregate: this.collName,
            // We use a rooted $or query to cause plan selection to use the subplanner and thus
            // yield.
            pipeline: [{$match: {$or: [{a: 0}, {b: 0}]}}],
            cursor: {}
        });

        if (!res.ok) {
            return;
        }

        var cursor = new DBCommandCursor(db.getMongo(), res);
        try {
            // No documents are ever inserted into the collection.
            assertAlways.eq(0, cursor.itcount());
        } catch (e) {
            // Ignore errors due to the plan executor being killed.
        }
    };

    return $config;
});
