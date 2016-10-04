'use strict';

/**
 * distinct_noindex.js
 *
 * Runs distinct on a non-indexed field and verifies the result.
 * The field contains non-unique values.
 * Each thread operates on the same collection.
 */
var $config = (function() {

    var data = {
        randRange: function randRange(low, high) {
            assertAlways.gt(high, low);
            return low + Random.randInt(high - low + 1);
        },
        numDocs: 1000
    };

    var states = (function() {

        function init(db, collName) {
            this.modulus = this.randRange(5, 15);

            var bulk = db[collName].initializeUnorderedBulkOp();
            for (var i = 0; i < this.numDocs; ++i) {
                bulk.insert({i: i % this.modulus, tid: this.tid});
            }
            var res = bulk.execute();
            assertAlways.writeOK(res);
            assertAlways.eq(this.numDocs, res.nInserted);
        }

        function distinct(db, collName) {
            assertWhenOwnColl.eq(this.modulus, db[collName].distinct('i', {tid: this.tid}).length);
        }

        return {init: init, distinct: distinct};

    })();

    var transitions = {init: {distinct: 1}, distinct: {distinct: 1}};

    return {data: data, threadCount: 10, iterations: 20, states: states, transitions: transitions};

})();
