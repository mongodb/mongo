/**
 * Reproducer for WT-7912. WT-7912 describes a scenario where the prefix_search mechanism added in
 * WT-7264 could result in duplicate values in a collection backed by a unique index.
 *
 * @tags: [
 *     # This test performs a lot of inserts. Using transactions slows this workload down greatly.
 *     # Transactions are not necessary for this reproducer.
 *     does_not_support_transactions
 * ]
 */

"use strict";

var $config = (function() {
    const initData = {
        getCollectionName: function(collName) {
            return "insert_duplicates_unique_index_" + collName;
        },

        getCollection: function(db, collName) {
            return db.getCollection(this.getCollectionName(collName));
        },
    };

    const states = {
        init: function init(db, collName) {
            const coll = this.getCollection(db, collName);
            for (let i = 0; i < 100; i++) {
                const res = coll.insert({t: this.tid, i: i});
                assertAlways.commandWorked(res);
                assertAlways.eq(1, res.nInserted, tojson(res));
            }
        },

        /**
         * Attempts to insert a duplicate document.
         */
        insertDup: function insertDup(db, collName) {
            const coll = this.getCollection(db, collName);
            for (let i = 0; i < 100; i++) {
                const res = coll.insert({t: this.tid, i: i});
                assertAlways.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
                assertAlways.eq(0, res.nInserted, tojson(res));
            }
        }
    };

    function setup(db, collName) {
        collName = this.getCollectionName(collName);
        assertAlways.commandWorked(db.createCollection(collName));
        assertAlways.commandWorked(
            db.getCollection(collName).createIndex({t: 1, i: 1}, {unique: true}));
    }

    const transitions = {
        init: {insertDup: 1.0},
        insertDup: {insertDup: 1.0},
    };

    return {
        threadCount: 10,
        iterations: 100,
        startState: 'init',
        states: states,
        data: initData,
        transitions: transitions,
        setup: setup
    };
})();
