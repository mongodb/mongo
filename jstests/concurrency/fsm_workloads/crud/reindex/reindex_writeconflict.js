/**
 * reindex_writeconflict.js
 *
 * Ensures reIndex successfully handles WriteConflictExceptions.
 *
 * The reIndex command is only available on standalone mode.
 * @tags: [
 *   requires_getmore,
 *   requires_standalone,
 *   incompatible_with_concurrency_simultaneous,
 * ]
 */
export const $config = (function() {
    var states = {
        reIndex: function reIndex(db, collName) {
            var res = db[collName].reIndex();
            assert.commandWorked(res);
        },
    };

    var transitions = {reIndex: {reIndex: 1}};

    function setup(db, collName, cluster) {
        for (var i = 0; i < 1000; ++i) {
            db[collName].insert({_id: i});
        }
        // Log traces for each WriteConflictException encountered in case they are not handled
        // properly.

        /*
          So long as there are no BFs, leave WCE tracing disabled.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, traceWriteConflictExceptions: true}));
        */

        // Set up failpoint to trigger WriteConflictException during write operations.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: 'WTWriteConflictException', mode: {activationProbability: 0.5}}));
    }

    function teardown(db, collName, cluster) {
        assert.commandWorked(
            db.adminCommand({configureFailPoint: 'WTWriteConflictException', mode: "off"}));
        assert.commandWorked(
            db.adminCommand({setParameter: 1, traceWriteConflictExceptions: false}));
    }

    return {
        threadCount: 2,
        iterations: 5,
        startState: 'reIndex',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };
})();
