/**
 * update_inc.js
 *
 * Inserts a single document into a collection. Each thread performs an
 * update operation to select the document and increment a particular
 * field. Asserts that the field has the correct value based on the number
 * of increments performed.
 */

import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    let data = {
        // uses the workload name as _id on the document.
        // assumes this name will be unique.
        id: "update_inc",
        getUpdateArgument: function (fieldName) {
            let updateDoc = {$inc: {}};
            updateDoc.$inc[fieldName] = 1;
            return updateDoc;
        },
    };

    let states = {
        init: function init(db, collName) {
            this.fieldName = "t" + this.tid;
            this.count = 0;
        },

        update: function update(db, collName) {
            let updateDoc = this.getUpdateArgument(this.fieldName);

            let res = db[collName].update({_id: this.id}, updateDoc);
            assert.commandWorked(res);
            assert.eq(0, res.nUpserted, tojson(res));

            if (isMongod(db)) {
                // Storage engines will automatically retry any operations when there are conflicts,
                // so we should always see a matching document.
                assert.eq(res.nMatched, 1, tojson(res));
                assert.eq(res.nModified, 1, tojson(res));
            } else {
                // On storage engines that do not support document-level concurrency, it is possible
                // that the query will not find the document. This can happen if another thread
                // updated the target document during a yield, triggering an invalidation.
                assert.contains(res.nMatched, [0, 1], tojson(res));
                assert.contains(res.nModified, [0, 1], tojson(res));
                assert.eq(res.nModified, res.nMatched, tojson(res));
            }

            // The $inc operator always modifies the matched document, so if we matched something,
            // then we must have updated it.
            this.count += res.nMatched >= 1;
        },

        find: function find(db, collName) {
            let docs = db[collName].find().toArray();
            if (!this.skipAssertions) {
                assert.eq(1, docs.length);
                // If the document hasn't been updated at all, then the field won't exist.
                let doc = docs[0];
                if (doc.hasOwnProperty(this.fieldName)) {
                    assert.eq(this.count, doc[this.fieldName]);
                } else {
                    assert.eq(this.count, 0);
                }
            }
        },
    };

    let transitions = {init: {update: 1}, update: {find: 1}, find: {update: 1}};

    function setup(db, collName, cluster) {
        let doc = {_id: this.id};

        // Pre-populate the fields we need to avoid size change for capped collections.
        for (let i = 0; i < this.threadCount; ++i) {
            doc["t" + i] = 0;
        }
        db[collName].insert(doc);
    }

    return {
        threadCount: 10,
        iterations: 20,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup,
    };
})();
