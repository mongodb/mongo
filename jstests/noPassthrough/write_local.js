// SERVER-22011: Deadlock in ticket distribution
(function() {
    'use strict';

    // Limit concurrent WiredTiger transactions to maximize locking issues, harmless for other SEs.
    var options = {verbose: 1};

    // Create a new single node replicaSet
    var replTest =
        new ReplSetTest({name: "write_local", nodes: 1, oplogSize: 1, nodeOptions: options});
    replTest.startSet();
    replTest.initiate();
    var mongod = replTest.getPrimary();
    mongod.adminCommand({setParameter: 1, wiredTigerConcurrentWriteTransactions: 1});

    var local = mongod.getDB('local');

    // Start inserting documents in test.capped and local.capped capped collections.
    var shells = ['test', 'local'].map(function(dbname) {
        var mydb = local.getSiblingDB(dbname);
        mydb.capped.drop();
        mydb.createCollection('capped', {capped: true, size: 20 * 1000});
        return startParallelShell('var mydb=db.getSiblingDB("' + dbname + '"); ' +
                                      '(function() { ' +
                                      '    for(var i=0; i < 10*1000; i++) { ' +
                                      '        mydb.capped.insert({ x: i }); ' +
                                      '    } ' +
                                      '})();',
                                  mongod.port);
    });

    // The following causes inconsistent locking order in the ticket system, depending on
    // timeouts to avoid deadlock.
    var oldObjects = 0;
    for (var i = 0; i < 1000; i++) {
        print(local.stats().objects);
        sleep(1);
    }

    // Wait for parallel shells to terminate and stop our replset.
    shells.forEach((function(f) {
        f();
    }));
    replTest.stopSet();
}());
