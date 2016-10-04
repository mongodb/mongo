'use strict';

/**
 * remove_multiple_documents.js
 *
 * Each thread first inserts 200 documents, each containing the thread id and
 * a random float. Then on each iteration, each thread repeatedly removes some
 * of the documents it inserted.
 */
var $config = (function() {

    var states = {
        init: function init(db, collName) {
            this.numDocs = 200;
            for (var i = 0; i < this.numDocs; ++i) {
                db[collName].insert({tid: this.tid, rand: Random.rand()});
            }
        },

        remove: function remove(db, collName) {
            // choose a random interval to remove documents from
            var low = Random.rand();
            var high = low + 0.05 * Random.rand();

            var res = db[collName].remove({tid: this.tid, rand: {$gte: low, $lte: high}});
            assertAlways.gte(res.nRemoved, 0);
            assertAlways.lte(res.nRemoved, this.numDocs);
            this.numDocs -= res.nRemoved;
        },

        count: function count(db, collName) {
            var numDocs = db[collName].find({tid: this.tid}).itcount();
            assertWhenOwnColl.eq(this.numDocs, numDocs);
        }
    };

    var transitions = {init: {count: 1}, count: {remove: 1}, remove: {remove: 0.825, count: 0.125}};

    return {threadCount: 10, iterations: 20, states: states, transitions: transitions};

})();
