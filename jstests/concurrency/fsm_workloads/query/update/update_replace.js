/**
 * update_replace.js
 *
 * Does updates that replace an entire document.
 * The collection has indexes on some but not all fields.
 */

import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    // explicitly pass db to avoid accidentally using the global `db`
    function assertResult(db, res) {
        assert.eq(0, res.nUpserted, tojson(res));

        if (isMongod(db)) {
            // Storage engines will automatically retry any operations when there are conflicts, so
            // we should always see a matching document.
            assert.eq(res.nMatched, 1, tojson(res));
        } else {
            // On storage engines that do not support document-level concurrency, it is possible
            // that the query will not find the document. This can happen if another thread
            // updated the target document during a yield, triggering an invalidation.
            assert.contains(res.nMatched, [0, 1], tojson(res));
        }

        // It's possible that we replaced the document with its current contents, making the update
        // a no-op.
        assert.contains(res.nModified, [0, 1], tojson(res));
        assert.lte(res.nModified, res.nMatched, tojson(res));
    }

    // returns an update doc
    function getRandomUpdateDoc() {
        let choices = [{}, {x: 1, y: 1, z: 1}, {a: 1, b: 1, c: 1}];
        return choices[Random.randInt(choices.length)];
    }

    let states = {
        update: function update(db, collName) {
            // choose a doc to update
            let docIndex = Random.randInt(this.numDocs);

            // choose an update to apply
            let updateDoc = getRandomUpdateDoc();

            // apply the update
            let res = db[collName].update({_id: docIndex}, updateDoc);
            assertResult(db, res);
        },
    };

    let transitions = {update: {update: 1}};

    function setup(db, collName, cluster) {
        assert.commandWorked(db[collName].createIndex({a: 1}));
        assert.commandWorked(db[collName].createIndex({b: 1}));
        // no index on c

        assert.commandWorked(db[collName].createIndex({x: 1}));
        assert.commandWorked(db[collName].createIndex({y: 1}));
        // no index on z

        // numDocs should be much less than threadCount, to make more threads use the same docs.
        this.numDocs = Math.floor(this.threadCount / 3);
        assert.gt(this.numDocs, 0, "numDocs should be a positive number");

        for (let i = 0; i < this.numDocs; ++i) {
            let res = db[collName].insert({_id: i});
            assert.commandWorked(res);
            assert.eq(1, res.nInserted);
        }

        assert.eq(this.numDocs, db[collName].find().itcount());
    }

    return {
        threadCount: 10,
        iterations: 10,
        startState: "update",
        states: states,
        transitions: transitions,
        setup: setup,
    };
})();
