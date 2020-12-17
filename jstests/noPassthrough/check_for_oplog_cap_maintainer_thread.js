/**
 * Checks that the oplog cap maintainer thread is started and the oplog stones calculation is
 * performed under normal startup circumstances. Both of these operations should not be done when
 * starting up with any of the following modes:
 *     - readonly
 *     - repair
 *     - recoverFromOplogAsStandalone
 *
 * @tags: [requires_replication, requires_persistence]
 */
(function() {
"use strict";

// Verify that the oplog cap maintainer thread is running under normal circumstances.
jsTestLog("Testing single node replica set mode");
const rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: {logLevel: 1}}});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

checkLog.contains(primary, "OplogTruncaterThread started");
checkLog.contains(primary, "WiredTiger record store oplog processing took");

rst.stopSet(/*signal=*/null, /*forRestart=*/true);

// A subset of startup options prevent the oplog cap maintainer thread from being started. These
// startup options are currently limited to readOnly, recoverFromOplogAsStandalone and repair.
function verifyOplogCapMaintainerThreadNotStarted(log) {
    const threadRegex = new RegExp("OplogTruncaterThread started");
    const oplogStonesRegex = new RegExp("WiredTiger record store oplog processing took");

    assert(!threadRegex.test(log));
    assert(!oplogStonesRegex.test(log));
}

jsTestLog("Testing recoverFromOplogAsStandalone mode");
clearRawMongoProgramOutput();
let conn = MongoRunner.runMongod({
    dbpath: primary.dbpath,
    noCleanData: true,
    setParameter: {recoverFromOplogAsStandalone: true, logLevel: 1},
});
assert(conn);
MongoRunner.stopMongod(conn);
verifyOplogCapMaintainerThreadNotStarted(rawMongoProgramOutput());

jsTestLog("Testing repair mode");
clearRawMongoProgramOutput();
conn = MongoRunner.runMongod({
    dbpath: primary.dbpath,
    noCleanData: true,
    repair: "",
    setParameter: {logLevel: 1},
});
assert(!conn);
verifyOplogCapMaintainerThreadNotStarted(rawMongoProgramOutput());
}());
