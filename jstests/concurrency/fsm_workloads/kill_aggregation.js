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
    // unique. Note that we choose our own collection name instead of using the collection provided
    // by the concurrency framework, because this workload drops its collection.
    $config.data.collName = 'kill_aggregation';

    $config.states.query = function query(db, collNameUnused) {
        const aggResult = db.runCommand({
            aggregate: this.collName,
            // We use a rooted $or query to cause plan selection to use the subplanner and thus
            // yield.
            pipeline: [{$match: {$or: [{a: 0}, {b: 0}]}}],
            cursor: {}
        });

        if (!aggResult.ok) {
            // We expect to see errors caused by the plan executor being killed, because of the
            // collection getting dropped on another thread.
            assertAlways.contains(aggResult.code,
                                  [ErrorCodes.OperationFailed, ErrorCodes.QueryPlanKilled],
                                  aggResult);
            return;
        }

        var cursor = new DBCommandCursor(db, aggResult);
        try {
            cursor.itcount();
        } catch (e) {
            // We expect to see errors caused by the plan executor being killed, because of the
            // collection getting dropped on another thread.
            if (ErrorCodes.QueryPlanKilled != e.code) {
                throw e;
            }
        }
    };

    return $config;
});
