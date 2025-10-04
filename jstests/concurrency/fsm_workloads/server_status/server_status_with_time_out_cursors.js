/**
 * Run serverStatus() while running a large number of queries which are expected to reach maxTimeMS
 * and time out.
 *
 * @tags: [
 *     catches_command_failures,
 *     # This test leaks cursors causing range deletions to hang waiting for ongoing queries
 *     assumes_balancer_off,
 *     # Uses $where operator
 *     requires_scripting,
 *     requires_getmore,
 * ]
 */
export const $config = (function () {
    const states = {
        /**
         * This is a no-op, used only as a transition state.
         */
        init: function init(db, collName) {},

        /**
         * Runs a query on the collection with a small enough batchSize to leave the cursor open.
         * If the command was successful, stores the resulting cursor in 'this.cursor'.
         */
        query: function query(db, collName) {
            try {
                // Set a low maxTimeMs and small batch size so that it's likely the cursor will
                // time out over its lifetime.
                let curs = db[collName]
                    .find({
                        $where: function () {
                            sleep(1);
                            return true;
                        },
                    })
                    .batchSize(2)
                    .maxTimeMS(10);

                const c = curs.itcount();
            } catch (e) {
                assert.commandFailedWithCode(e, [
                    ErrorCodes.MaxTimeMSExpired,
                    ErrorCodes.NetworkInterfaceExceededTimeLimit,
                ]);
            }
        },

        serverStatus: function serverStatus(db, collName) {
            assert.commandWorked(db.adminCommand({serverStatus: 1}));
        },
    };

    const transitions = {
        init: {
            query: 0.8,
            serverStatus: 0.2,
        },

        query: {query: 0.8, serverStatus: 0.2},
        serverStatus: {query: 0.5, serverStatus: 0.5},
    };

    function setup(db, collName, cluster) {
        // Write some data.
        assert.commandWorked(db[collName].insert(Array.from({length: 100}, (_) => ({a: 1}))));
    }

    return {
        threadCount: 10,
        iterations: 100,
        states: states,
        startState: "init",
        transitions: transitions,
        setup: setup,
    };
})();
