/**
 * update_multifield.js
 *
 * Does updates that affect multiple fields on a single document.
 * The collection has an index for each field, and a compound index for all fields.
 */

import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    function makeQuery(options) {
        let query = {};
        if (!options.multi) {
            query._id = Random.randInt(options.numDocs);
        }

        return query;
    }

    // returns an update doc
    function makeRandomUpdateDoc() {
        let x = Random.randInt(5);
        let y = Random.randInt(5);
        // ensure z is never 0, so the $inc is never 0, so we can assert nModified === nMatched
        let z = Random.randInt(5) + 1;
        let set = Random.rand() > 0.5;
        let push = Random.rand() > 0.2;

        let updateDoc = {};
        updateDoc[set ? "$set" : "$unset"] = {x: x};
        updateDoc[push ? "$push" : "$pull"] = {y: y};
        updateDoc.$inc = {z: z};

        return updateDoc;
    }

    let states = {
        update: function update(db, collName) {
            // choose an update to apply
            let updateDoc = makeRandomUpdateDoc();

            // apply this update
            let query = makeQuery({multi: this.multi, numDocs: this.numDocs});
            let res = db[collName].update(query, updateDoc, {multi: this.multi});
            this.assertResult(res, db, collName, query);
        },
    };

    let transitions = {update: {update: 1}};

    function setup(db, collName, cluster) {
        assert.commandWorked(db[collName].createIndex({x: 1}));
        assert.commandWorked(db[collName].createIndex({y: 1}));
        assert.commandWorked(db[collName].createIndex({z: 1}));
        assert.commandWorked(db[collName].createIndex({x: 1, y: 1, z: 1}));

        // numDocs should be much less than threadCount, to make more threads use the same docs.
        this.numDocs = Math.floor(this.threadCount / 3);
        assert.gt(this.numDocs, 0, "numDocs should be a positive number");

        for (let i = 0; i < this.numDocs; ++i) {
            let res = db[collName].insert({_id: i});
            assert.commandWorked(res);
            assert.eq(1, res.nInserted);
        }
    }

    return {
        threadCount: 10,
        iterations: 10,
        startState: "update",
        states: states,
        transitions: transitions,
        data: {
            assertResult: function (res, db, collName, query) {
                assert.eq(0, res.nUpserted, tojson(res));

                if (isMongod(db)) {
                    // Storage engines will automatically retry any operations when there are
                    // conflicts, so we should always see a matching document.
                    assert.eq(res.nMatched, 1, tojson(res));
                    assert.eq(res.nModified, 1, tojson(res));
                } else {
                    // On storage engines that do not support document-level concurrency, it is
                    // possible that the query will not find the document. This can happen if
                    // another thread updated the target document during a yield, triggering an
                    // invalidation.
                    assert.contains(res.nMatched, [0, 1], tojson(res));
                    assert.contains(res.nModified, [0, 1], tojson(res));
                    assert.eq(res.nModified, res.nMatched, tojson(res));
                }
            },
            multi: false,
        },
        setup: setup,
    };
})();
