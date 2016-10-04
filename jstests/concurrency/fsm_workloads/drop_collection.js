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

    return {threadCount: 10, iterations: 10, data: data, states: states, transitions: transitions};

})();
