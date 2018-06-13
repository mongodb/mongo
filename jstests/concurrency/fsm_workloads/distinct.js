'use strict';

/**
 * distinct.js
 *
 * Runs distinct on an indexed field and verifies the result.
 * The indexed field contains unique values.
 * Each thread operates on a separate collection.
 */

var $config = (function() {

    var data = {numDocs: 1000, prefix: 'distinct_fsm', shardKey: {i: 1}};

    var states = (function() {

        function init(db, collName) {
            this.threadCollName = this.prefix + '_' + this.tid;
            var bulk = db[this.threadCollName].initializeUnorderedBulkOp();
            for (var i = 0; i < this.numDocs; ++i) {
                bulk.insert({i: i});
            }
            var res = bulk.execute();
            assertAlways.writeOK(res);
            assertAlways.eq(this.numDocs, res.nInserted);
            assertAlways.commandWorked(db[this.threadCollName].ensureIndex({i: 1}));
        }

        function distinct(db, collName) {
            assertWhenOwnColl.eq(this.numDocs, db[this.threadCollName].distinct('i').length);
        }

        return {init: init, distinct: distinct};

    })();

    var transitions = {init: {distinct: 1}, distinct: {distinct: 1}};

    return {
        data: data,
        threadCount: 10,
        iterations: 20,
        states: states,
        transitions: transitions,
    };

})();
