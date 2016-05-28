'use strict';

/**
 * indexed_insert_base.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan. The indexed
 * value is the thread's id.
 */
var $config = (function() {

    function makeSortSpecFromIndexSpec(ixSpec) {
        var sort = {};

        for (var field in ixSpec) {
            if (!ixSpec.hasOwnProperty(field)) {
                continue;
            }

            var order = ixSpec[field];
            if (order !== 1 && order !== -1) {  // e.g. '2d' or '2dsphere'
                order = 1;
            }

            sort[field] = order;
        }

        return sort;
    }

    var states = {
        init: function init(db, collName) {
            this.nInserted = 0;
            this.indexedValue = this.tid;
        },

        insert: function insert(db, collName) {
            var res = db[collName].insert(this.getDoc());
            assertAlways.writeOK(res);
            assertAlways.eq(1, res.nInserted, tojson(res));
            this.nInserted += this.docsPerInsert;
        },

        find: function find(db, collName) {
            // collection scan
            var count = db[collName].find(this.getDoc()).sort({$natural: 1}).itcount();
            assertWhenOwnColl.eq(count, this.nInserted);

            // Use hint() to force an index scan, but only when an appropriate index exists.
            // We can only use hint() when the index exists and we know that the collection
            // is not being potentially modified by other workloads.
            var ownColl = false;
            assertWhenOwnColl(function() {
                ownColl = true;
            });
            if (this.indexExists && ownColl) {
                count = db[collName].find(this.getDoc()).hint(this.getIndexSpec()).itcount();
                assertWhenOwnColl.eq(count, this.nInserted);
            }

            // Otherwise, impose a sort ordering over the collection scan
            else {
                // For single and compound-key indexes, the index specification is a
                // valid sort spec; however, for geospatial and text indexes it is not
                var sort = makeSortSpecFromIndexSpec(this.getIndexSpec());
                count = db[collName].find(this.getDoc()).sort(sort).itcount();
                assertWhenOwnColl.eq(count, this.nInserted);
            }
        }
    };

    var transitions = {init: {insert: 1}, insert: {find: 1}, find: {insert: 1}};

    function setup(db, collName, cluster) {
        var res = db[collName].ensureIndex(this.getIndexSpec());
        assertAlways.commandWorked(res);
        this.indexExists = true;
    }

    return {
        threadCount: 20,
        iterations: 50,
        states: states,
        transitions: transitions,
        data: {
            getIndexSpec: function getIndexSpec() {
                var ixSpec = {};
                ixSpec[this.indexedField] = 1;
                return ixSpec;
            },
            getDoc: function getDoc() {
                var doc = {};
                doc[this.indexedField] = this.indexedValue;
                return doc;
            },
            indexedField: 'x',
            shardKey: {x: 1},
            docsPerInsert: 1
        },
        setup: setup
    };

})();
