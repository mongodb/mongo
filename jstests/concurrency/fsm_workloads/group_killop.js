'use strict';

/**
 * Run group() continuously, occasionally killing them midway.
 */
load("jstests/libs/fixture_helpers.js");                  // For isMongos.
load('jstests/concurrency/fsm_libs/extend_workload.js');  // For extendWorkload.
load('jstests/concurrency/fsm_workloads/group.js');       // For $config.

var $config = extendWorkload($config, function($config, $super) {
    var states = (function() {

        function init(db, collName) {
        }

        function group(db, collName) {
            const res = db.runCommand(this.generateGroupCmdObj(collName));

            // The only time this should fail is due to interrupt from the 'killOp'.
            if (res.ok) {
                assertAlways.commandWorked(res);
            } else {
                // TODO We really only expect Interrupted here, but until SERVER-32565 is resolved
                // there are times when we might get InternalError.
                assertAlways.contains(res.code, [ErrorCodes.Interrupted, ErrorCodes.InternalError]);
                return;
            }

            // lte because the documents are generated randomly, and so not all buckets are
            // guaranteed to exist.
            assertWhenOwnColl.lte(res.count, this.numDocs);
            assertWhenOwnColl.lte(res.keys, 10);
        }

        function chooseRandomlyFrom(arr) {
            if (!Array.isArray(arr)) {
                throw new Error('Expected array for first argument, but got: ' + tojson(arr));
            }
            return arr[Random.randInt(arr.length)];
        }

        function killOp(db, collName) {
            // Find a group command to kill.
            const countOps = db.currentOp({"command.group.ns": collName}).inprog;
            if (countOps.length > 0) {
                const op = chooseRandomlyFrom(countOps);
                const res = db.adminCommand({killOp: 1, op: op.opid});
                assertAlways.commandWorked(res);
            }
        }

        return {init: init, group: group, killOp: killOp};

    })();

    $config.states = states;
    $config.transitions = {init: {group: 0.7, killOp: 0.3}, group: {init: 1}, killOp: {init: 1}};
    $config.threadCount = 40;
    $config.iterations = 40;

    return $config;
});
