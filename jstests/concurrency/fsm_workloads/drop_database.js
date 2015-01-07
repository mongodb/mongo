'use strict';

/**
 * drop_database.js
 *
 * Repeatedly creates and drops a database.
 */
var $config = (function() {

    var states = {
        init: function init(db, collName) {
            this.uniqueDBName = 'drop_database' + this.tid;
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

    var transitions = {
        init: { createAndDrop: 1 },
        createAndDrop: { createAndDrop: 1 }
    };

    return {
        threadCount: 10,
        iterations: 10,
        states: states,
        transitions: transitions
    };

})();
