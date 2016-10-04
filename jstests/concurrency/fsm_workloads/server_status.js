'use strict';

/**
 * server_status.js
 *
 * Simply checks that the serverStatus command works
 */
var $config = (function() {

    var states = {
        status: function status(db, collName) {
            var opts =
                {opcounterRepl: 1, oplog: 1, rangeDeleter: 1, repl: 1, security: 1, tcmalloc: 1};
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
