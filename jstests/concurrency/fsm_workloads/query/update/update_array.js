/**
 * update_array.js
 *
 * Each thread does a $push or $pull on a random doc, pushing or pulling its
 * thread id. After each push or pull, the thread does a .findOne() to verify
 * that its thread id is present or absent (respectively). This is correct even
 * though other threads in the workload may be modifying the array between the
 * update and the find, because thread ids are unique.
 */

import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    let states = (function () {
        // db: explicitly passed to avoid accidentally using the global `db`
        // res: WriteResult
        // nModifiedPossibilities: array of allowed values for res.nModified
        function assertUpdateSuccess(db, res, nModifiedPossibilities) {
            assert.eq(0, res.nUpserted, tojson(res));

            if (isMongod(db)) {
                // The update should have succeeded if a matching document existed.
                assert.contains(1, nModifiedPossibilities, tojson(res));
                assert.contains(res.nModified, nModifiedPossibilities, tojson(res));
            } else {
                // On storage engines that do not support document-level concurrency, it is possible
                // that the update will not update all matching documents. This can happen if
                // another thread updated a target document during a yield, triggering an
                // invalidation.
                assert.contains(res.nMatched, [0, 1], tojson(res));
                assert.contains(res.nModified, [0, 1], tojson(res));
            }
        }

        function doPush(db, collName, docIndex, value) {
            let res = db[collName].update({_id: docIndex}, {$push: {arr: value}});

            // assert the update reported success
            assertUpdateSuccess(db, res, [1]);

            // find the doc and make sure it was updated
            let doc = db[collName].findOne({_id: docIndex});
            assert.neq(null, doc);
            assert(doc.hasOwnProperty("arr"), 'doc should have contained a field named "arr": ' + tojson(doc));

            // If the document was invalidated during a yield, then we may not have updated
            // anything. The $push operator always modifies the matched document, so if we
            // matched something, then we must have updated it.
            if (res.nMatched > 0) {
                assert.contains(
                    value,
                    doc.arr,
                    "doc.arr doesn't contain value (" + value + ") after $push: " + tojson(doc.arr),
                );
            }
        }

        function doPull(db, collName, docIndex, value) {
            let res = db[collName].update({_id: docIndex}, {$pull: {arr: value}});

            // assert the update reported success
            assertUpdateSuccess(db, res, [0, 1]);

            // find the doc and make sure it was updated
            let doc = db[collName].findOne({_id: docIndex});
            assert.neq(null, doc);

            // If the document was invalidated during a yield, then we may not have updated
            // anything. If the update matched a document, then the $pull operator would have
            // removed all occurrences of 'value' from the array (meaning that there should be
            // none left).
            if (res.nMatched > 0) {
                assert.eq(
                    -1,
                    doc.arr.indexOf(value),
                    "doc.arr contains removed value (" + value + ") after $pull: " + tojson(doc.arr),
                );
            }
        }

        return {
            push: function push(db, collName) {
                let docIndex = Random.randInt(this.numDocs);
                let value = this.tid;

                doPush(db, collName, docIndex, value);
            },

            pull: function pull(db, collName) {
                let docIndex = Random.randInt(this.numDocs);
                let value = this.tid;

                doPull(db, collName, docIndex, value);
            },
        };
    })();

    let transitions = {push: {push: 0.8, pull: 0.2}, pull: {push: 0.8, pull: 0.2}};

    function setup(db, collName, cluster) {
        // index on 'arr', the field being updated
        assert.commandWorked(db[collName].createIndex({arr: 1}));
        for (let i = 0; i < this.numDocs; ++i) {
            let res = db[collName].insert({_id: i, arr: []});
            assert.commandWorked(res);
            assert.eq(1, res.nInserted);
        }
    }

    return {
        threadCount: 5,
        iterations: 10,
        startState: "push",
        states: states,
        transitions: transitions,
        data: {numDocs: 10},
        setup: setup,
    };
})();
