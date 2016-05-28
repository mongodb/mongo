'use strict';

/**
 * update_ordered_bulk_inc.js
 *
 * Inserts multiple documents into a collection. Each thread performs a
 * bulk update operation to select the document and increment a particular
 * field. Asserts that the field has the correct value based on the number
 * of increments performed.
 *
 * Uses an ordered, bulk operation to perform the updates.
 */

// For isMongod, recordIdCanChangeOnUpdate, and supportsDocumentLevelConcurrency.
load('jstests/concurrency/fsm_workload_helpers/server_types.js');

var $config = (function() {

    var states = {
        init: function init(db, collName) {
            this.fieldName = 't' + this.tid;
        },

        update: function update(db, collName) {
            var updateDoc = {$inc: {}};
            updateDoc.$inc[this.fieldName] = 1;

            var bulk = db[collName].initializeOrderedBulkOp();
            for (var i = 0; i < this.docCount; ++i) {
                bulk.find({_id: i}).update(updateDoc);
            }
            var result = bulk.execute();
            // TODO: this actually does assume that there are no unique indexes.
            //       but except for weird cases like that, it is valid even when other
            //       threads are modifying the same collection
            assertAlways.eq(0, result.getWriteErrorCount());

            ++this.count;
        },

        find: function find(db, collName) {
            var docs = db[collName].find().toArray();

            if (isMongod(db) && !recordIdCanChangeOnUpdate(db)) {
                // If the RecordId cannot change and we aren't updating any fields in any indexes,
                // we should always see all matching documents, since they would not be able to move
                // ahead or behind our collection scan or index scan.
                assertWhenOwnColl.eq(this.docCount, docs.length);
            } else {
                // On MMAPv1, we may see more than 'this.docCount' documents during our find. This
                // can happen if an update causes the document to grow such that it is moved in
                // front of an index or collection scan which has already returned it.
                assertWhenOwnColl.gte(docs.length, this.docCount);
            }

            if (isMongod(db) && supportsDocumentLevelConcurrency(db)) {
                // Storage engines which support document-level concurrency will automatically retry
                // any operations when there are conflicts, so we should have updated all matching
                // documents.
                docs.forEach(function(doc) {
                    assertWhenOwnColl.eq(this.count, doc[this.fieldName]);
                }, this);
            }

            docs.forEach(function(doc) {
                // If the document hasn't been updated at all, then the field won't exist.
                if (doc.hasOwnProperty(this.fieldName)) {
                    assertWhenOwnColl.lte(doc[this.fieldName], this.count);
                }
                assertWhenOwnColl.lt(doc._id, this.docCount);
            }, this);
        }
    };

    var transitions = {init: {update: 1}, update: {find: 1}, find: {update: 1}};

    function setup(db, collName, cluster) {
        this.count = 0;
        for (var i = 0; i < this.docCount; ++i) {
            db[collName].insert({_id: i});
        }
    }

    return {
        threadCount: 10,
        iterations: 20,
        states: states,
        transitions: transitions,
        setup: setup,
        data: {docCount: 15}
    };

})();
