'use strict';

/**
 * drop_collection.js
 *
 * Repeatedly creates and drops a collection.
 */
var $config = (function() {

    var data = {
        // Use the workload name as a prefix for the collection name,
        // since the workload name is assumed to be unique.
        prefix: 'drop_collection'
    };

    var states = (function() {

        function uniqueCollectionName(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        function createAndDrop(db, collName) {
            // TODO: should we ever do something different?
            var myCollName = uniqueCollectionName(this.prefix, this.tid, this.num++);
            assertAlways.commandWorked(db.createCollection(myCollName));
            assertAlways(db[myCollName].drop());
        }

        return {init: init, createAndDrop: createAndDrop};

    })();

    var transitions = {init: {createAndDrop: 1}, createAndDrop: {createAndDrop: 1}};

    // This test performs dropCollection concurrently from many threads, and dropCollection on a
    // sharded cluster takes a distributed lock. Since a distributed lock is acquired by repeatedly
    // attempting to grab the lock every half second for 20 seconds (a max of 40 attempts), it's
    // possible that some thread will be starved by the other threads and fail to grab the lock
    // after 40 attempts. To reduce the likelihood of this, we choose threadCount and iterations so
    // that threadCount * iterations < 40.
    // The threadCount and iterations can be increased once PM-697 ("Remove all usages of
    // distributed lock") is complete.
    return {threadCount: 5, iterations: 5, data: data, states: states, transitions: transitions};

})();
