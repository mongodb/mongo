// Test that setting readOnly mode on the command line causes readOnly to be properly set in both
// isMaster and serverStatus output.
//
// This test requires mmapv1.
// @tags: [requires_mmapv1]
(function() {
    "use strict";

    // TODO: use configured storageEngine from testData once wiredTiger supports readOnly mode.
    var mongod = MongoRunner.runMongod({storageEngine: "mmapv1"});
    var dbpath = mongod.dbpath;

    // ensure dbpath gets set up.
    assert.writeOK(mongod.getDB("foo").x.insert({x: 1}));

    assert(!mongod.getDB("admin").isMaster().readOnly);
    assert(!mongod.getDB("admin").serverStatus().storageEngine.readOnly);
    MongoRunner.stopMongod(mongod);

    mongod = MongoRunner.runMongod(
        {storageEngine: "mmapv1", queryableBackupMode: "", dbpath: dbpath, noCleanData: true});
    assert(mongod.getDB("admin").isMaster().readOnly);
    assert(mongod.getDB("admin").serverStatus().storageEngine.readOnly);
    MongoRunner.stopMongod(mongod);
}());
