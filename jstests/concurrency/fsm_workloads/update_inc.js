'use strict';

/**
 * update_inc.js
 *
 * Inserts a single document into a collection. Each thread performs an
 * update operation to select the document and increment a particular
 * field. Asserts that the field has the correct value based on the number
 * of increments performed.
 */

// For isMongod and supportsDocumentLevelConcurrency.
load('jstests/concurrency/fsm_workload_helpers/server_types.js');

var $config = (function() {

    var data = {
        // uses the workload name as _id on the document.
        // assumes this name will be unique.
        id: 'update_inc'
    };

    var states = {
        init: function init(db, collName) {
            this.fieldName = 't' + this.tid;
            this.count = 0;
        },

        update: function update(db, collName) {
            var updateDoc = {$inc: {}};
            updateDoc.$inc[this.fieldName] = 1;

            var res = db[collName].update({_id: this.id}, updateDoc);
            assertAlways.eq(0, res.nUpserted, tojson(res));

            if (isMongod(db) && supportsDocumentLevelConcurrency(db)) {
                // Storage engines which support document-level concurrency will automatically retry
                // any operations when there are conflicts, so we should always see a matching
                // document.
                assertWhenOwnColl.eq(res.nMatched, 1, tojson(res));
                if (db.getMongo().writeMode() === 'commands') {
                    assertWhenOwnColl.eq(res.nModified, 1, tojson(res));
                }
            } else {
                // On storage engines that do not support document-level concurrency, it is possible
                // that the query will not find the document. This can happen if another thread
                // updated the target document during a yield, triggering an invalidation.
                assertWhenOwnColl.contains(res.nMatched, [0, 1], tojson(res));
                if (db.getMongo().writeMode() === 'commands') {
                    assertWhenOwnColl.contains(res.nModified, [0, 1], tojson(res));
                    assertAlways.eq(res.nModified, res.nMatched, tojson(res));
                }
            }

            // The $inc operator always modifies the matched document, so if we matched something,
            // then we must have updated it.
            this.count += (res.nMatched >= 1);
        },

        find: function find(db, collName) {
            var docs = db[collName].find().toArray();
            assertWhenOwnColl.eq(1, docs.length);
            assertWhenOwnColl(() => {
                // If the document hasn't been updated at all, then the field won't exist.
                var doc = docs[0];
                if (doc.hasOwnProperty(this.fieldName)) {
                    assertWhenOwnColl.eq(this.count, doc[this.fieldName]);
                } else {
                    assertWhenOwnColl.eq(this.count, 0);
                }
            });
        }
    };

    var transitions = {init: {update: 1}, update: {find: 1}, find: {update: 1}};

    function setup(db, collName, cluster) {
        var doc = {_id: this.id};

        // Pre-populate the fields we need to avoid size change for capped collections.
        for (var i = 0; i < this.threadCount; ++i) {
            doc['t' + i] = 0;
        }
        db[collName].insert(doc);
    }

    return {
        threadCount: 5,
        iterations: 10,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup
    };

})();
