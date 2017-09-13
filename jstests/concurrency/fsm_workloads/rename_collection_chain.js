'use strict';

/**
 * rename_collection_chain.js
 *
 * Creates a collection and then repeatedly executes the renameCollection
 * command against it. The previous "to" namespace is used as the next "from"
 * namespace.
 */
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropCollections

var $config = (function() {

    var data = {
        // Use the workload name as a prefix for the collection name,
        // since the workload name is assumed to be unique.
        prefix: 'rename_collection_chain'
    };

    var states = (function() {

        function uniqueCollectionName(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function init(db, collName) {
            this.fromCollName = uniqueCollectionName(this.prefix, this.tid, 0);
            this.num = 1;
            assertAlways.commandWorked(db.createCollection(this.fromCollName));
        }

        function rename(db, collName) {
            var toCollName = uniqueCollectionName(this.prefix, this.tid, this.num++);
            var res = db[this.fromCollName].renameCollection(toCollName, false /* dropTarget */);
            assertWhenOwnDB.commandWorked(res);
            this.fromCollName = toCollName;
        }

        return {init: init, rename: rename};

    })();

    var transitions = {init: {rename: 1}, rename: {rename: 1}};

    function teardown(db, collName, cluster) {
        var pattern = new RegExp('^' + this.prefix + '\\d+_\\d+$');
        dropCollections(db, pattern);
    }

    return {
        threadCount: 10,
        iterations: 20,
        data: data,
        states: states,
        transitions: transitions,
        teardown: teardown
    };

})();
