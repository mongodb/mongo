'use strict';

/**
 * drop_database.js
 *
 * Repeatedly creates and drops a database.
 */
var $config = (function() {

    var states = {
        init: function init(db, collName) {
            this.uniqueDBName = db.getName() + 'drop_database' + this.tid;
        },

        createAndDrop: function createAndDrop(db, collName) {
            // TODO: should we ever do something different?
            //       e.g. create multiple collections on the database and then drop?
            var myDB = db.getSiblingDB(this.uniqueDBName);
            assertAlways.commandWorked(myDB.createCollection(collName));

            var res = myDB.dropDatabase();
            assertAlways.commandWorked(res);
            assertAlways.eq(this.uniqueDBName, res.dropped);
        }
    };

    var transitions = {init: {createAndDrop: 1}, createAndDrop: {createAndDrop: 1}};

    return {
        threadCount: 10,
        // We only run a few iterations to reduce the amount of data cumulatively
        // written to disk by mmapv1. For example, setting 10 threads and 5
        // iterations causes this workload to write at least 32MB (.ns and .0 files)
        // * 10 threads * 5 iterations worth of data to disk, which can be slow on
        // test hosts.
        iterations: 5,
        states: states,
        transitions: transitions
    };

})();
