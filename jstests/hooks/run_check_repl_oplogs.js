// Runner for checkOplogs() that compares the oplog on all replica set nodes
// to ensure all nodes have the same data.
'use strict';

(function() {
    var startTime = Date.now();
    assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a mongod?');

    var primaryInfo = db.isMaster();

    assert(primaryInfo.ismaster,
           'shell is not connected to the primary or master node: ' + tojson(primaryInfo));

    var cmdLineOpts = db.adminCommand('getCmdLineOpts');
    assert.commandWorked(cmdLineOpts);
    var testFixture = new ReplSetTest(db.getMongo().host);
    testFixture.checkOplogs();

    var totalTime = Date.now() - startTime;
    print('Finished consistency oplog checks of cluster in ' + totalTime + ' ms.');
})();
