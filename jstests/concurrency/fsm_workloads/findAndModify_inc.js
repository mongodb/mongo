'use strict';

/**
 * findAndModify_inc.js
 *
 * Inserts a single document into a collection. Each thread performs a
 * findAndModify command to select the document and increment a particular
 * field. Asserts that the field has the correct value based on the number
 * of increments performed.
 *
 * This workload was designed to reproduce SERVER-15892.
 */

// For isMongod and supportsDocumentLevelConcurrency.
load('jstests/concurrency/fsm_workload_helpers/server_types.js');

var $config = (function() {

    var states = {

        init: function init(db, collName) {
            this.fieldName = 't' + this.tid;
            this.count = 0;
        },

        update: function update(db, collName) {
            var updateDoc = {$inc: {}};
            updateDoc.$inc[this.fieldName] = 1;

            var res = db.runCommand(
                {findAndModify: collName, query: {_id: 'findAndModify_inc'}, update: updateDoc});
            assertAlways.commandWorked(res);

            // If the document was invalidated during a yield, then we wouldn't have modified it.
            // The "findAndModify" command returns a null value in this case. See SERVER-22002 for
            // more details.
            if (isMongod(db) && supportsDocumentLevelConcurrency(db)) {
                // For storage engines that support document-level concurrency, if the document is
                // modified by another thread during a yield, then the operation is retried
                // internally. We never expect to see a null value returned by the "findAndModify"
                // command when it is known that a matching document exists in the collection.
                assertWhenOwnColl(res.value !== null, 'query spec should have matched a document');
            }

            if (res.value !== null) {
                ++this.count;
            }
        },

        find: function find(db, collName) {
            var docs = db[collName].find().toArray();
            assertWhenOwnColl.eq(1, docs.length);
            assertWhenOwnColl(() => {
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
        db[collName].insert({_id: 'findAndModify_inc'});
    }

    return {
        threadCount: 20,
        iterations: 20,
        states: states,
        transitions: transitions,
        setup: setup
    };

})();
