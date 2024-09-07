/**
 * drop_index_during_lookup.js
 *
 * Sets up a situation where index join strategy will be chosen for $lookup while while running
 * concurrent dropIndexes against the index chosen for the foreign side.
 */
import {interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";

export const $config = (function() {
    let data = {
        collName: 'localColl',
        foreignCollName: 'foreignColl',
    };

    let states = {
        lookup: function lookup(db, collName) {
            try {
                const coll = db[this.collName];
                const result = coll.aggregate([{$lookup: { from: this.foreignCollName, localField: 'a', foreignField: 'b', as: 'out'}}]).toArray();
                assert.eq(result.length, 1);
                assert.docEq({_id: 0, a: 0, out: [{_id: 0, b: 0}]}, result[0]);
            } catch (e) {
                // We expect any errors of query getting killed due to selected index for join is
                // dropped. We also accept StaleConfig error that may be raised due to the slowness
                // of MoveChunk in certain suites in debug build.
                assert.contains(e.code, interruptedQueryErrors.concat(ErrorCodes.StaleConfig));
            }
        },

        dropIndex: function dropIndex(db, collName) {
            // We don't assert that the command succeeded when dropping an index because it's
            // possible another thread has already dropped this index.
            db[this.foreignCollName].dropIndex({b: 1});

            // Recreate the index that was dropped.
            assert.commandWorkedOrFailedWithCode(db[this.foreignCollName].createIndex({b: 1}), [
                ErrorCodes.IndexBuildAborted,
                ErrorCodes.NoMatchingDocument,
            ]);
        }
    };

    let transitions = {lookup: {lookup: 0.8, dropIndex: 0.2}, dropIndex: {lookup: 1}};

    function setup(db, collName, cluster) {
        assert.commandWorked(db[this.collName].insert({_id: 0, a: 0}));
        assert.commandWorked(db[this.foreignCollName].insert({_id: 0, b: 0}));
        assert.commandWorked(db[this.foreignCollName].createIndex({b: 1}));
    }

    return {
        threadCount: 10,
        iterations: 50,
        data: data,
        states: states,
        startState: 'lookup',
        transitions: transitions,
        setup: setup
    };
})();
