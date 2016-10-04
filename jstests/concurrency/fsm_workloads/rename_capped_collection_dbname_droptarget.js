'use strict';

/**
 * rename_capped_collection_dbname_droptarget.js
 *
 * Creates a capped collection and then repeatedly executes the renameCollection
 * command against it, specifying a different database name in the namespace.
 * Inserts documents into the "to" namespace and specifies dropTarget=true.
 */
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropDatabases

var $config = (function() {

    var data = {
        // Use the workload name as a prefix for the collection name,
        // since the workload name is assumed to be unique.
        prefix: 'rename_capped_collection_dbname_droptarget'
    };

    var states = (function() {

        var options = {capped: true, size: 4096};

        function uniqueDBName(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function insert(db, collName, numDocs) {
            for (var i = 0; i < numDocs; ++i) {
                var res = db[collName].insert({});
                assertAlways.writeOK(res);
                assertAlways.eq(1, res.nInserted);
            }
        }

        function init(db, collName) {
            var num = 0;
            this.fromDBName = db.getName() + uniqueDBName(this.prefix, this.tid, num++);
            this.toDBName = db.getName() + uniqueDBName(this.prefix, this.tid, num++);

            var fromDB = db.getSiblingDB(this.fromDBName);
            assertAlways.commandWorked(fromDB.createCollection(collName, options));
            assertAlways(fromDB[collName].isCapped());
        }

        function rename(db, collName) {
            var fromDB = db.getSiblingDB(this.fromDBName);
            var toDB = db.getSiblingDB(this.toDBName);

            // Clear out the "from" collection and insert 'fromCollCount' documents
            var fromCollCount = 7;
            assertAlways(fromDB[collName].drop());
            assertAlways.commandWorked(fromDB.createCollection(collName, options));
            assertAlways(fromDB[collName].isCapped());
            insert(fromDB, collName, fromCollCount);

            var toCollCount = 4;
            assertAlways.commandWorked(toDB.createCollection(collName, options));
            insert(toDB, collName, toCollCount);

            // Verify that 'fromCollCount' documents exist in the "to" collection
            // after the rename occurs
            var renameCommand = {
                renameCollection: this.fromDBName + '.' + collName,
                to: this.toDBName + '.' + collName,
                dropTarget: true
            };

            assertAlways.commandWorked(fromDB.adminCommand(renameCommand));
            assertAlways(toDB[collName].isCapped());
            assertAlways.eq(fromCollCount, toDB[collName].find().itcount());
            assertAlways.eq(0, fromDB[collName].find().itcount());

            // Swap "to" and "from" collections for next execution
            var temp = this.fromDBName;
            this.fromDBName = this.toDBName;
            this.toDBName = temp;
        }

        return {init: init, rename: rename};

    })();

    var transitions = {init: {rename: 1}, rename: {rename: 1}};

    function teardown(db, collName, cluster) {
        var pattern = new RegExp('^' + db.getName() + this.prefix + '\\d+_\\d+$');
        dropDatabases(db, pattern);
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
