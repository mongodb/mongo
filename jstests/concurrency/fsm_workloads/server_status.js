/**
 * server_status.js
 *
 * Simply checks that the serverStatus command works
 */
export const $config = (function() {
    var states = {
        status: function status(db, collName) {
            var res = db.serverStatus();
            assert.commandWorked(res);
            assert(res.hasOwnProperty('version'));
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
