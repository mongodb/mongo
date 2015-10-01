'use strict';

/**
 * update_inc.js
 *
 * Inserts a single document into a collection. Each thread performs an
 * update operation to select the document and increment a particular
 * field. Asserts that the field has the correct value based on the number
 * of increments performed.
 */
load('jstests/concurrency/fsm_workload_helpers/server_types.js'); // for isMongod and isMMAPv1

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
            var updateDoc = { $inc: {} };
            updateDoc.$inc[this.fieldName] = 1;

            var res = db[collName].update({ _id: this.id }, updateDoc);
            assertAlways.eq(0, res.nUpserted, tojson(res));

            if (isMongod(db) && !isMMAPv1(db)) {
                // For non-mmap storage engines we can have a strong assertion that exactly one doc
                // will be modified.
                assertWhenOwnColl.eq(res.nMatched, 1, tojson(res));
                if (db.getMongo().writeMode() === 'commands') {
                    assertWhenOwnColl.eq(res.nModified, 1, tojson(res));
                }
            }
            else {
                // Zero matches are possible for MMAP v1 because the update will skip a document
                // that was invalidated during a yield.
                assertWhenOwnColl.contains(res.nMatched, [0, 1], tojson(res));
                if (db.getMongo().writeMode() === 'commands') {
                    assertWhenOwnColl.contains(res.nModified, [0, 1], tojson(res));
                    assertAlways.eq(res.nModified, res.nMatched, tojson(res));
                }
            }

            ++this.count;
        },

        find: function find(db, collName) {
            var docs = db[collName].find().toArray();
            assertWhenOwnColl.eq(1, docs.length);
            assertWhenOwnColl((function() {
                var doc = docs[0];
                assertWhenOwnColl.eq(this.count, doc[this.fieldName]);
            }).bind(this));
        }
    };

    var transitions = {
        init: { update: 1 },
        update: { find: 1 },
        find: { update: 1 }
    };

    var threadCount = 5;

    function setup(db, collName, cluster) {
        var doc = { _id: this.id };

        // Pre-populate the fields we need to avoid size change for capped collections.
        for (var i = 0; i < threadCount; ++i) {
            doc['t' + i] = 0;
        }
        db[collName].insert(doc);
    }

    return {
        threadCount: threadCount,
        iterations: 10,
        data: data,
        states: states,
        transitions: transitions,
        setup: setup
    };

})();
