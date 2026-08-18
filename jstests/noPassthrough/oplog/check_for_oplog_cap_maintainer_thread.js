/**
 * Checks that the oplog cap maintainer thread is started and the oplog truncate markers calculation
 * is performed under normal startup circumstances. Both of these operations should not be done when
 * starting up with any of the following modes:
 *     - skipOplogSampling
 *     - readonly
 *     - repair
 *     - recoverFromOplogAsStandalone
 *
 * @tags: [requires_replication, requires_persistence]
 */
import {verifyOplogCapMaintainerThreadNotStarted} from "jstests/libs/oplog_truncation_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {skipTestIfSizeBasedOplogTruncationDisabled} from "jstests/libs/oplog_truncation_util.js";

// Verify that the oplog cap maintainer thread is running under normal circumstances.
jsTestLog("Testing single node replica set mode");
const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {logComponentVerbosity: tojson({storage: 1})}},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

// This test relies on marker-based oplog truncation, which may be disabled in disagg.
// TODO(SERVER-125068) remove this once this feature flag is deleted
const isDsc = skipTestIfSizeBasedOplogTruncationDisabled(rst.getPrimary(), () => rst.stopSet());

checkLog.containsJson(primary, 5295000); // OplogCapMaintainerThread started.
checkLog.containsJson(primary, 22382); // Oplog truncate markers calculated.

rst.stopSet(/*signal=*/ null, /*forRestart=*/ true);

if (isDsc) {
    // None of the other startup options are supported in DSC
    jsTest.log.info("Skipping other modes because they are unsupported in this architecture");
    quit();
}

// A subset of startup options prevent the oplog cap maintainer thread from being started. These
// startup options are currently limited to skipOplogSampling, readOnly,
// recoverFromOplogAsStandalone and repair.

jsTestLog("Testing with skipOplogSampling mode");
clearRawMongoProgramOutput();
let conn = MongoRunner.runMongod({
    dbpath: primary.dbpath,
    noCleanData: true,
    setParameter: {skipOplogSampling: true, logComponentVerbosity: tojson({storage: 1})},
});
assert(conn);
MongoRunner.stopMongod(conn);
verifyOplogCapMaintainerThreadNotStarted(rawMongoProgramOutput(".*"));

jsTestLog("Testing readOnly mode");
clearRawMongoProgramOutput();
conn = MongoRunner.runMongod({
    dbpath: primary.dbpath,
    noCleanData: true,
    queryableBackupMode: "", // readOnly
    setParameter: {logComponentVerbosity: tojson({storage: 1})},
});
assert(conn);
MongoRunner.stopMongod(conn);
verifyOplogCapMaintainerThreadNotStarted(rawMongoProgramOutput(".*"));

jsTestLog("Testing recoverFromOplogAsStandalone mode");
clearRawMongoProgramOutput();
conn = MongoRunner.runMongod({
    dbpath: primary.dbpath,
    noCleanData: true,
    setParameter: {recoverFromOplogAsStandalone: true, logComponentVerbosity: tojson({storage: 1})},
});
assert(conn);
MongoRunner.stopMongod(conn);
verifyOplogCapMaintainerThreadNotStarted(rawMongoProgramOutput(".*"));

jsTestLog("Testing repair mode");
clearRawMongoProgramOutput();
conn = MongoRunner.runMongod({
    dbpath: primary.dbpath,
    noCleanData: true,
    repair: "",
    setParameter: {logComponentVerbosity: tojson({storage: 1})},
});
assert(!conn);
verifyOplogCapMaintainerThreadNotStarted(rawMongoProgramOutput(".*"));
