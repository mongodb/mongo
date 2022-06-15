/**
 * Test that WT log compressor settings can be changed between clean shutdowns.
 *
 * @tags: [requires_wiredtiger, requires_replication, requires_persistence]
 */

(function() {
'use strict';

const initOpt = {
    wiredTigerEngineConfigString: 'log=(compressor=snappy)'
};

const replSetTest = new ReplSetTest({nodes: 2, nodeOptions: initOpt});
replSetTest.startSet();
replSetTest.initiate();

// Perform a write larger than 128 bytes so to ensure there is a compressed journal entry. Not
// really necessary as the oplog is filled with system entries just by starting up the replset, but
// in case somethings changes in the future. 10kb value used in case this threshold is modified.
const testDB = replSetTest.getPrimary().getDB('test');
testDB.coll.insert({a: 'a'.repeat(10 * 1024)});

const restartNodeWithOpts = function(node, opts) {
    // Mongod clean shutdown by SIGINT.
    replSetTest.stop(node, 2, {allowedExitCode: MongoRunner.EXIT_CLEAN});
    replSetTest.start(node, opts);
};

const optChangeJournal = {
    noCleanData: true,  // Keep dbpath data from previous start.
    wiredTigerEngineConfigString: 'log=(compressor=zstd)'
};

// Restart nodes in a rolling fashion, none should crash due to decompression errors.
for (const node of replSetTest.getSecondaries()) {
    restartNodeWithOpts(node, optChangeJournal);
}
restartNodeWithOpts(replSetTest.getPrimary(), optChangeJournal);

replSetTest.stopSet();
})();
