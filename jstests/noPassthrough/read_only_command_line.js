// Test that setting readOnly mode on the command line causes readOnly to be properly set in both
// isMaster and serverStatus output.
//
// This test requires mmapv1.
// @tags: [requires_mmapv1]
(function() {
    "use strict";

    // TODO: use configured storageEngine from testData once wiredTiger supports readOnly mode.
    var bongod = BongoRunner.runBongod({storageEngine: "mmapv1"});
    var dbpath = bongod.dbpath;

    // ensure dbpath gets set up.
    assert.writeOK(bongod.getDB("foo").x.insert({x: 1}));

    assert(!bongod.getDB("admin").isMaster().readOnly);
    assert(!bongod.getDB("admin").serverStatus().storageEngine.readOnly);
    BongoRunner.stopBongod(bongod);

    bongod = BongoRunner.runBongod(
        {storageEngine: "mmapv1", queryableBackupMode: "", dbpath: dbpath, noCleanData: true});
    assert(bongod.getDB("admin").isMaster().readOnly);
    assert(bongod.getDB("admin").serverStatus().storageEngine.readOnly);
    BongoRunner.stopBongod(bongod);
}());
