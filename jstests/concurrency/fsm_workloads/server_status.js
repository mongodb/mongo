/**
 * server_status.js
 *
 * Simply checks that the serverStatus command works
 */
import {assertAlways} from "jstests/concurrency/fsm_libs/assert.js";

export const $config = (function() {
    var states = {
        status: function status(db, collName) {
            var res = db.serverStatus();
            assertAlways.commandWorked(res);
            assertAlways(res.hasOwnProperty('version'));
        }
    };

    var transitions = {status: {status: 1}};

    return {
        threadCount: 10,
        iterations: 20,
        states: states,
        startState: 'status',
        transitions: transitions
    };
})();
