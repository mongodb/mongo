// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
    load('jstests/hooks/validate_collections.js');  // For validateCollections

    var serverList = [];
    serverList.push(db.getMongo());

    var addSecondaryNodes = function() {
        var cmdLineOpts = db.adminCommand('getCmdLineOpts');
        assert.commandWorked(cmdLineOpts);

        if (cmdLineOpts.parsed.hasOwnProperty('replication') &&
            cmdLineOpts.parsed.replication.hasOwnProperty('replSet')) {
            var rst = new ReplSetTest(db.getMongo().host);
            // Call getPrimary to populate rst with information about the nodes.
            var primary = rst.getPrimary();
            assert(primary, 'calling getPrimary() failed');
            serverList.push(...rst.getSecondaries());
        }
    };

    addSecondaryNodes();

    for (var server of serverList) {
        print('Running validate() on ' + server.host);
        var dbNames = server.getDBNames();
        for (var dbName of dbNames) {
            if (!validateCollections(db.getSiblingDB(dbName), {full: true})) {
                throw new Error('Collection validation failed');
            }
        }
    }
})();
