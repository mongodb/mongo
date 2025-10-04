/**
 * update_ordered_bulk_inc.js
 *
 * Inserts multiple documents into a collection. Each thread performs a
 * bulk update operation to select the document and increment a particular
 * field. Asserts that the field has the correct value based on the number
 * of increments performed.
 *
 * Uses an ordered, bulk operation to perform the updates.
 * @tags: [
 *   # Runs a multi-update which is non-retryable.
 *   requires_non_retryable_writes
 * ]
 */

import {isMongod} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    let states = {
        init: function init(db, collName) {
            this.fieldName = "t" + this.tid;
        },

        update: function update(db, collName) {
            let updateDoc = {$inc: {}};
            updateDoc.$inc[this.fieldName] = 1;

            let bulk = db[collName].initializeOrderedBulkOp();
            for (let i = 0; i < this.docCount; ++i) {
                bulk.find({_id: i}).update(updateDoc);
            }
            let result = bulk.execute();
            // TODO: this actually does assume that there are no unique indexes.
            //       but except for weird cases like that, it is valid even when other
            //       threads are modifying the same collection
            assert.eq(0, result.getWriteErrorCount());

            ++this.count;
        },

        find: function find(db, collName) {
            let docs = db[collName].find().toArray();

            // We aren't updating any fields in any indexes, so we should always see all
            // matching documents, since they would not be able to move ahead or behind
            // our collection scan or index scan.
            assert.eq(this.docCount, docs.length);

            if (isMongod(db)) {
                // Storage engines will automatically retry any operations when there are conflicts,
                // so we should have updated all matching documents.
                docs.forEach(function (doc) {
                    assert.eq(this.count, doc[this.fieldName]);
                }, this);
            }

            docs.forEach(function (doc) {
                // If the document hasn't been updated at all, then the field won't exist.
                if (doc.hasOwnProperty(this.fieldName)) {
                    assert.lte(doc[this.fieldName], this.count);
                }
                assert.lt(doc._id, this.docCount);
            }, this);
        },
    };

    let transitions = {init: {update: 1}, update: {find: 1}, find: {update: 1}};

    function setup(db, collName, cluster) {
        this.count = 0;
        for (let i = 0; i < this.docCount; ++i) {
            db[collName].insert({_id: i});
        }
    }

    return {
        threadCount: 10,
        iterations: 20,
        states: states,
        transitions: transitions,
        setup: setup,
        data: {docCount: 15},
    };
})();
