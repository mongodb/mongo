'use strict';

/**
 * map_reduce_interrupt.js
 *
 * Extends the map_reduce_inline.js workload with a state that randomly kills a running map-reduce
 * operation. This workload is intended to test that there are no deadlocks or unhandled exceptions
 * when tearing down a map-reduce command following an interrupt.
 *
 * @tags: [
 *   # mapReduce does not support afterClusterTime.
 *   does_not_support_causal_consistency,
 *   uses_curop_agg_stage,
 * ]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');                      // for extendWorkload
load('jstests/concurrency/fsm_workloads/map_reduce_replace_nonexistent.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.prefix = 'map_reduce_interrupt';

    $config.states.killOp = function killOp(db, collName) {
        const mrOps = db.getSiblingDB('admin')
                          .aggregate([
                              {$currentOp: {}},
                              {$match: {'command.mapreduce': collName}},
                              {$project: {opid: '$opid'}}
                          ])
                          .toArray();

        if (mrOps.length > 0) {
            const randomOpIndex = Math.floor(mrOps.length * Math.random());
            const randomOpId = mrOps[randomOpIndex].opid;
            jsTestLog('Randomly chose to kill Map-Reduce with opid: ' + tojson(randomOpId));

            // Note: Even if the killOp reaches the server after the map-reduce command is already
            // done, the server still returns an "ok" response, so this assertion is safe.
            assertAlways.commandWorked(
                db.getSiblingDB('admin').runCommand({killOp: 1, op: randomOpId}));
        } else {
            // No map-reduce operations to kill at the moment.
        }
    };

    $config.states.mapReduce = function mapReduce(db, collName) {
        try {
            $super.states.mapReduce.apply(this, arguments);
        } catch (err) {
            // The nature of this test means that we expect the map-reduce command to sometimes fail
            // due to interruption. No other failures are expected, though. Note that interruptions
            // can cause some unrelated error codes, including InternalError (during JavaScript
            // execution) and some non-specific errors (SERVER-39281, SERVER-39282). Checking for
            // "interrupted" in the error message is a reasonable way to spot all the miscellaneous
            // errors that can occur because of an interruption.
            if (err.code != ErrorCodes.Interrupted && err.code != ErrorCodes.InternalError &&
                !/interrupted/i.test(err.message)) {
                throw err;
            }
        }
    };

    $config.teardown = function teardown(db, collname, cluster) {
        // Interrupted map-reduce operations should still be able to clean up the temp collections
        // that they create within the database of the output collection and within the "local"
        // database.
        //
        // Cleanup occurs as part of its own operations, which can also be interrupted, but the
        // 'killOp' state of this test only targets map-reduce operations.

        const dbTempCollectionsResult =
            db.runCommand({listCollections: 1, filter: {'options.temp': true}});
        assertAlways.commandWorked(dbTempCollectionsResult);
        assertAlways.eq(
            dbTempCollectionsResult.cursor.firstBatch.length, 0, dbTempCollectionsResult);

        if (!cluster.isSharded()) {
            // Note that we can't do this check on sharded clusters, which do not have a "local"
            // database.
            const localTempCollectionsResult = db.getSiblingDB('local').runCommand(
                {listCollections: 1, filter: {'options.temp': true}});
            assertAlways.commandWorked(localTempCollectionsResult);
            assertAlways.eq(
                localTempCollectionsResult.cursor.firstBatch.length, 0, localTempCollectionsResult);
        }
    };

    $config.transitions = {
        init: {mapReduce: 0.8, killOp: 0.2},
        mapReduce: {mapReduce: 0.8, killOp: 0.2},
        killOp: {mapReduce: 0.8, killOp: 0.2}
    };

    return $config;
});
