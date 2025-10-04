/**
 * list_indexes.js
 *
 * Checks that the listIndexes command can tolerate concurrent modifications to the
 * index catalog.
 *
 * This workload implicitly assumes that its tid ranges are [0, $config.threadCount). This
 * isn't guaranteed to be true when they are run in parallel with other workloads. Therefore
 * it can't be run in concurrency simultaneous suites.
 * @tags: [
 *   requires_getmore,
 *   incompatible_with_concurrency_simultaneous,
 *   # We run the 'listIndexes' command, which cannot be run within a transaction, and perform
 *   # subsequent getMores. Therefore the getMore will run outside a transaction.
 *   uses_getmore_outside_of_transaction,
 * ]
 */
export const $config = (function () {
    let states = (function () {
        // Picks a random index to drop and recreate.
        function modifyIndices(db, collName) {
            let spec = {};
            spec["foo" + this.tid] = 1;

            assert.commandWorked(db[collName].dropIndex(spec));
            sleep(100);
            assert.commandWorked(db[collName].createIndex(spec));
        }

        // List indexes, using a batchSize of 2 to ensure getmores happen.
        function listIndices(db, collName) {
            let cursor = new DBCommandCursor(db, db.runCommand({listIndexes: collName, cursor: {batchSize: 2}}));
            assert.gte(cursor.itcount(), 0);
        }

        return {modifyIndices: modifyIndices, listIndices: listIndices};
    })();

    let transitions = {
        modifyIndices: {listIndices: 0.75, modifyIndices: 0.25},
        listIndices: {listIndices: 0.25, modifyIndices: 0.75},
    };

    function setup(db, collName) {
        // Create indices {fooi: 1}.
        for (let i = 0; i < this.threadCount; ++i) {
            let spec = {};
            spec["foo" + i] = 1;
            assert.commandWorked(db[collName].createIndex(spec));
        }
    }

    return {
        threadCount: 10,
        iterations: 20,
        states: states,
        startState: "modifyIndices",
        transitions: transitions,
        setup: setup,
    };
})();
