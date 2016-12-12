// Runner for checkOplogs() that compares the oplog on all replica set nodes
// to ensure all nodes have the same data.
'use strict';

(function() {
    // Master/Slave does not support oplog test, since the oplog.$main is empty on the Slave.
    var MasterSlaveOplogTest = function() {
        throw new Error('checkOplogs not supported for Master/Slave');
    };

    var startTime = Date.now();
    assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a mongod?');

    var primaryInfo = db.isMaster();

    assert(primaryInfo.ismaster,
           'shell is not connected to the primary or master node: ' + tojson(primaryInfo));

    var cmdLineOpts = db.adminCommand('getCmdLineOpts');
    assert.commandWorked(cmdLineOpts);
    var isMasterSlave = cmdLineOpts.parsed.master === true;
    var testFixture =
        isMasterSlave ? new MasterSlaveOplogTest() : new ReplSetTest(db.getMongo().host);
    testFixture.checkOplogs();

    var totalTime = Date.now() - startTime;
    print('Finished consistency oplog checks of cluster in ' + totalTime + ' ms.');
})();
