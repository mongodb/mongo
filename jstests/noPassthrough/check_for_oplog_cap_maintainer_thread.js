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
const rst = new ReplSetTest(
    {nodes: 1, nodeOptions: {setParameter: {logComponentVerbosity: tojson({storage: 1})}}});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
checkLog.containsJson(primary, 5295000);  // OplogCapMaintainerThread started.
checkLog.containsJson(primary, 22382);    // Oplog stones calculated.

rst.stopSet(/*signal=*/null, /*forRestart=*/true);

// A subset of startup options prevent the oplog cap maintainer thread from being started. These
// startup options are currently limited to readOnly, recoverFromOplogAsStandalone and repair.
function verifyOplogCapMaintainerThreadNotStarted(log) {
    const threadRegex = new RegExp("\"id\":5295000");
    const oplogStonesRegex = new RegExp("\"id\":22382");

    assert(!threadRegex.test(log));
    assert(!oplogStonesRegex.test(log));
}

jsTestLog("Testing readOnly mode");
clearRawMongoProgramOutput();
let conn = MongoRunner.runMongod({
    dbpath: primary.dbpath,
    noCleanData: true,
    queryableBackupMode: "",  // readOnly
    setParameter: {logComponentVerbosity: tojson({storage: 1})},
});
assert(conn);
MongoRunner.stopMongod(conn);
verifyOplogCapMaintainerThreadNotStarted(rawMongoProgramOutput());

jsTestLog("Testing recoverFromOplogAsStandalone mode");
clearRawMongoProgramOutput();
conn = MongoRunner.runMongod({
    dbpath: primary.dbpath,
    noCleanData: true,
    setParameter: {recoverFromOplogAsStandalone: true, logComponentVerbosity: tojson({storage: 1})},
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
    setParameter: {logComponentVerbosity: tojson({storage: 1})},
});
assert(!conn);
verifyOplogCapMaintainerThreadNotStarted(rawMongoProgramOutput());
}());
