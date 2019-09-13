/**
 * Tests $out stage of aggregate command concurrently with killOp. Ensures that all the temporary
 * collections created during aggreate command are deleted.
 *
 * @tags: [uses_curop_agg_stage]
 */
'use strict';
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/agg_base.js');    // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.states.aggregate = function aggregate(db, collName) {
        // $out to the same collection so that concurrent aggregate commands would cause congestion.
        db[collName].runCommand(
            {aggregate: collName, pipeline: [{$out: "interrupt_temp_out"}], cursor: {}});
    };

    $config.states.killOp = function killOp(db, collName) {
        // The aggregate command could be running different commands internally (renameCollection,
        // insertDocument, etc.) depending on which stage of execution it is in. So, get all the
        // operations that are running against the input, output or temp collections.
        const activeCurOpsFilter = {
            op: "command",
            active: true,
            $or: [
                {"ns": db.getName() + ".interrupt_temp_out"},              // For output collection.
                {"ns": db.getName() + "." + collName},                     // For input collection.
                {"ns": {$regex: "^" + db.getName() + "\.tmp\.agg_out.*"}}  // For temp during $out.
            ],
            "command.drop": {
                $exists: false
            }  // Exclude 'drop' command from the filter to make sure that we don't kill the the
               // drop command which is responsible for dropping the temporary collection.
        };

        const currentOpOutput = db.getSiblingDB('admin')
                                    .aggregate([{$currentOp: {}}, {$match: activeCurOpsFilter}])
                                    .toArray();
        for (let op of currentOpOutput) {
            assert(op.hasOwnProperty('opid'));
            assertAlways.commandWorked(db.getSiblingDB('admin').killOp(op.opid));
        }
    };

    $config.teardown = function teardown(db, collName, cluster) {
        // Ensure that no temporary collection is left behind.
        assertAlways.eq(db.getCollectionNames().filter(col => col.includes('tmp.agg_out')).length,
                        0);
    };

    $config.transitions = {
        aggregate: {aggregate: 0.8, killOp: 0.2},
        killOp: {aggregate: 0.8, killOp: 0.2}
    };

    $config.startState = 'aggregate';

    return $config;
});
