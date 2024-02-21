/**
 * Tests metrics in serverStatus related to replication.
 *
 * The test for metrics.repl.network.oplogGetMoresProcessed requires a storage engine that supports
 * document-level locking because it uses the planExecutorHangBeforeShouldWaitForInserts failpoint
 * to block oplog fetching getMores while trying to do oplog writes.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    checkWriteConcernTimedOut,
    restartServerReplication,
    stopServerReplication
} from "jstests/libs/write_concern_util.js";

/**
 * Test replication metrics
 */
function _testSecondaryMetricsHelper(secondary, opCount, baseOpsApplied, baseOpsReceived) {
    var ss = secondary.getDB("test").serverStatus();
    jsTestLog(`Secondary ${secondary.host} metrics: ${tojson(ss.metrics)}`);

    assert(ss.metrics.repl.network.readersCreated > 0, "no (oplog) readers created");
    assert(ss.metrics.repl.network.getmores.num > 0, "no getmores");
    assert(ss.metrics.repl.network.getmores.totalMillis > 0, "no getmores time");
    // The first oplog entry may or may not make it into network.ops now that we have two
    // n ops (initiate and new primary) before steady replication starts.
    // Sometimes, we disconnect from our sync source and since our find is a gte query, we may
    // double count an oplog entry, so we need some wiggle room for that.
    assert.lte(
        ss.metrics.repl.network.ops, opCount + baseOpsApplied + 5, "wrong number of ops retrieved");
    assert.gte(
        ss.metrics.repl.network.ops, opCount + baseOpsApplied, "wrong number of ops retrieved");
    assert(ss.metrics.repl.network.bytes > 0, "zero or missing network bytes");

    assert.gt(
        ss.metrics.repl.network.replSetUpdatePosition.num, 0, "no update position commands sent");

    // Under a two-node replica set setting, the secondary should not have received or processed any
    // oplog getMore requests from the primary.
    assert.eq(
        ss.metrics.repl.network.oplogGetMoresProcessed.num, 0, "non-zero oplog getMore processed");
    assert.eq(ss.metrics.repl.network.oplogGetMoresProcessed.totalMillis,
              0,
              "non-zero oplog getMore process time");

    assert.gt(ss.metrics.repl.syncSource.numSelections, 0, "num selections not incremented");
    assert.gt(ss.metrics.repl.syncSource.numTimesChoseDifferent, 0, "no new sync source chosen");

    assert(ss.metrics.repl.buffer.count >= 0, "buffer count missing");
    assert(ss.metrics.repl.buffer.sizeBytes >= 0, "size (bytes)] missing");
    assert(ss.metrics.repl.buffer.maxSizeBytes >= 0, "maxSize (bytes) missing");

    assert.eq(ss.metrics.repl.apply.batchSize,
              opCount + baseOpsReceived,
              "apply ops batch size mismatch");
    assert(ss.metrics.repl.apply.batches.num > 0, "no batches");
    assert(ss.metrics.repl.apply.batches.totalMillis >= 0, "missing batch time");
    assert.eq(ss.metrics.repl.apply.ops, opCount + baseOpsApplied, "wrong number of applied ops");
}

// Metrics are racy, e.g. repl.buffer.count could over- or under-reported briefly. Retry on error.
function testSecondaryMetrics(secondary, opCount, baseOpsApplied, baseOpsReceived) {
    assert.soon(() => {
        try {
            _testSecondaryMetricsHelper(secondary, opCount, baseOpsApplied, baseOpsReceived);
            return true;
        } catch (exc) {
            jsTestLog(`Caught ${exc}, retrying`);
            return false;
        }
    });
}

var rt = new ReplSetTest({
    name: "server_status_metrics",
    nodes: 2,
    oplogSize: 100,
    // Set a smaller periodicNoopIntervalSecs to aid sync source selection later in the test. Only
    // enable periodic noop writes when we actually need it to avoid races in other metrics tests.
    nodeOptions: {setParameter: {writePeriodicNoops: false, periodicNoopIntervalSecs: 2}}
});
rt.startSet();
// Initiate the replica set with high election timeout to avoid accidental elections.
rt.initiateWithHighElectionTimeout();

rt.awaitSecondaryNodes();

var secondary = rt.getSecondary();
var primary = rt.getPrimary();
var testDB = primary.getDB("test");

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Record the base oplogGetMoresProcessed on primary and the base oplog getmores on secondary.
const primaryBaseOplogGetMoresProcessedNum =
    primary.getDB("test").serverStatus().metrics.repl.network.oplogGetMoresProcessed.num;
const secondaryBaseGetMoresNum =
    secondary.getDB("test").serverStatus().metrics.repl.network.getmores.num;

assert.commandWorked(testDB.createCollection('a'));
assert.commandWorked(testDB.b.insert({}, {writeConcern: {w: 2}}));

var ss = secondary.getDB("test").serverStatus();
// The number of ops received  and the number of ops applied are not guaranteed to be the same
// during initial sync oplog application as we apply received operations only if the operation's
// optime is greater than OplogApplier::Options::beginApplyingOpTime.
var secondaryBaseOplogOpsApplied = ss.metrics.repl.apply.ops;
var secondaryBaseOplogOpsReceived = ss.metrics.repl.apply.batchSize;

// Disable batching of inserts so each one creates an oplog entry.
assert.commandWorked(testDB.adminCommand({setParameter: 1, internalInsertMaxBatchSize: 1}));

// add test docs
var bulk = testDB.a.initializeUnorderedBulkOp();
for (let x = 0; x < 1000; x++) {
    bulk.insert({});
}
assert.commandWorked(bulk.execute({w: 2}));

testSecondaryMetrics(secondary, 1000, secondaryBaseOplogOpsApplied, secondaryBaseOplogOpsReceived);

var options = {writeConcern: {w: 2}, multi: true, upsert: true};
assert.commandWorked(testDB.a.update({}, {$set: {d: new Date()}}, options));

testSecondaryMetrics(secondary, 2000, secondaryBaseOplogOpsApplied, secondaryBaseOplogOpsReceived);

// Test that the number of oplog getMore requested by the secondary and processed by the primary has
// increased since the start of the test.
const primaryOplogGetMoresProcessedNum =
    primary.getDB("test").serverStatus().metrics.repl.network.oplogGetMoresProcessed.num;
const secondaryGetMoresNum =
    secondary.getDB("test").serverStatus().metrics.repl.network.getmores.num;
assert.gt(primaryOplogGetMoresProcessedNum,
          primaryBaseOplogGetMoresProcessedNum,
          `primary getMores processed have not increased; final: ${
              primaryOplogGetMoresProcessedNum}, base: ${primaryBaseOplogGetMoresProcessedNum}`);
assert.gt(secondaryGetMoresNum,
          secondaryBaseGetMoresNum,
          `secondary getMores received have not increased; final: ${secondaryGetMoresNum}, base: ${
              secondaryBaseGetMoresNum}`);

// Test getLastError.wtime and that it only records stats for w > 1, see SERVER-9005
var startMillis = testDB.serverStatus().metrics.getLastError.wtime.totalMillis;
var startNum = testDB.serverStatus().metrics.getLastError.wtime.num;

jsTestLog(
    `Primary ${primary.host} metrics #1: ${tojson(primary.getDB("test").serverStatus().metrics)}`);

assert.commandWorked(testDB.a.insert({x: 1}, {writeConcern: {w: 1, wtimeout: 5000}}));
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.totalMillis, startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum);

assert.commandFailedWithCode(testDB.a.insert({x: 1}, {writeConcern: {w: -11, wtimeout: 5000}}),
                             ErrorCodes.FailedToParse);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.totalMillis, startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum);

assert.commandWorked(testDB.a.insert({x: 1}, {writeConcern: {w: 2, wtimeout: 5000}}));
assert(testDB.serverStatus().metrics.getLastError.wtime.totalMillis >= startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum + 1);

// Write will fail because there are only 2 nodes
assert.writeError(testDB.a.insert({x: 1}, {writeConcern: {w: 3, wtimeout: 50}}));
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum + 2);

// Test metrics related to writeConcern timeouts and default writeConcern.
var startGLEMetrics = testDB.serverStatus().metrics.getLastError;

// Set the default WC to timeout.
assert.commandWorked(testDB.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 2, wtimeout: 1000}, writeConcern: {w: 1}}));
var stopReplProducer = configureFailPoint(secondary, 'stopReplProducer');
stopReplProducer.wait();

// Explicit timeout - increments wtimeouts.
var res = testDB.a.insert({x: 1}, {writeConcern: {w: 2, wtimeout: 1000}});
assert.commandWorkedIgnoringWriteConcernErrors(res);
checkWriteConcernTimedOut({writeConcernError: res.getWriteConcernError()});
assert.eq(res.getWriteConcernError().errInfo.writeConcern.provenance, "clientSupplied");

// Default timeout - increments wtimeouts and default.wtimeouts.
var res = testDB.a.insert({x: 1});
assert.commandWorkedIgnoringWriteConcernErrors(res);
checkWriteConcernTimedOut({writeConcernError: res.getWriteConcernError()});
assert.eq(res.getWriteConcernError().errInfo.writeConcern.provenance, "customDefault");

// Set the default WC to unsatisfiable.
stopReplProducer.off();
assert.commandWorked(testDB.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 3}, writeConcern: {w: 1}}));

// Explicit unsatisfiable - no counters incremented.
var res = testDB.a.insert({x: 1}, {writeConcern: {w: 3}});
assert.commandFailedWithCode(res, ErrorCodes.UnsatisfiableWriteConcern);
assert.eq(res.getWriteConcernError().errInfo.writeConcern.provenance, "clientSupplied");

// Default unsatisfiable - increments default.unsatisfiable.
var res = testDB.a.insert({x: 1});
assert.commandFailedWithCode(res, ErrorCodes.UnsatisfiableWriteConcern);
assert.eq(res.getWriteConcernError().errInfo.writeConcern.provenance, "customDefault");

// Set the default WC back to {w: 1, wtimeout: 0}.
assert.commandWorked(testDB.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1, wtimeout: 0}, writeConcern: {w: 1}}));

// Validate counters.
var endGLEMetrics = testDB.serverStatus().metrics.getLastError;
assert.eq(endGLEMetrics.wtimeouts.floatApprox, startGLEMetrics.wtimeouts + 2);
assert.eq(endGLEMetrics.default.wtimeouts.floatApprox, startGLEMetrics.default.wtimeouts + 1);
assert.eq(endGLEMetrics.default.unsatisfiable.floatApprox,
          startGLEMetrics.default.unsatisfiable + 1);

jsTestLog(
    `Primary ${primary.host} metrics #2: ${tojson(primary.getDB("test").serverStatus().metrics)}`);

let ssOld = secondary.getDB("test").serverStatus().metrics.repl.syncSource;
jsTestLog(`Secondary ${secondary.host} metrics before restarting replication: ${tojson(ssOld)}`);

// Enable periodic noops to aid sync source selection.
assert.commandWorked(primary.adminCommand({setParameter: 1, writePeriodicNoops: true}));

// Enable the setSmallOplogGetMoreMaxTimeMS failpoint on secondary so that it will start using
// a small awaitData timeout for oplog fetching after re-choosing the sync source. This is needed to
// make sync source return empty batches more frequently in order to test the metric
// numEmptyBatches.
configureFailPoint(secondary, 'setSmallOplogGetMoreMaxTimeMS');

// Wait for the secondary to sync from the primary before asserting that the secondary increments
// numTimesChoseSame. Otherwise, the secondary may go into the loop with an empty sync source, which
// will lead to the loop never exiting as the secondary always treats choosing the primary as a
// new sync source.
rt.awaitSyncSource(secondary, primary, 5 * 1000 /* timeout */);

// Repeatedly restart replication and wait for the sync source to be rechosen. If the sync source
// gets set to empty between stopping and restarting replication, then the secondary won't
// increment numTimesChoseSame, so we do this in a loop.
let ssNew;
assert.soon(
    function() {
        stopServerReplication(secondary);
        restartServerReplication(secondary);

        // Do a dummy write to choose a new sync source and replicate the write to block on that.
        assert.commandWorked(
            primary.getDB("test").bar.insert({"dummy_write": 3}, {writeConcern: {w: 2}}));
        ssNew = secondary.getDB("test").serverStatus().metrics.repl.syncSource;
        jsTestLog(
            `Secondary ${secondary.host} metrics after restarting replication: ${tojson(ssNew)}`);
        return ssNew.numTimesChoseSame > ssOld.numTimesChoseSame;
    },
    "timed out waiting to re-choose same sync source",
    null,
    5 * 1000 /* 5sec interval to wait for noop */);

assert.gt(ssNew.numSelections, ssOld.numSelections, "num selections not incremented");
assert.gt(ssNew.numTimesChoseSame, ssOld.numTimesChoseSame, "same sync source not chosen");

// Get the base number of empty batches after the secondary is up to date. Assert that the secondary
// eventually gets an empty batch due to awaitData timeout.
rt.awaitLastOpCommitted();
const targetNumEmptyBatches =
    secondary.getDB("test").serverStatus().metrics.repl.network.getmores.numEmptyBatches + 1;
assert.soon(
    () => secondary.getDB("test").serverStatus().metrics.repl.network.getmores.numEmptyBatches >=
        targetNumEmptyBatches,
    `Timed out waiting for numEmptyBatches reach ${targetNumEmptyBatches}, current ${
        secondary.getDB("test").serverStatus().metrics.repl.network.getmores.numEmptyBatches}`);

// Stop the primary so the secondary cannot choose a sync source.
ssOld = ssNew;
rt.stop(primary);
stopServerReplication(secondary);
restartServerReplication(secondary);
assert.soon(function() {
    ssNew = secondary.getDB("test").serverStatus().metrics.repl.syncSource;
    jsTestLog(`Secondary ${secondary.host} metrics after stopping primary: ${tojson(ssNew)}`);
    return ssNew.numTimesCouldNotFind > ssOld.numTimesCouldNotFind;
});

assert.gt(ssNew.numSelections, ssOld.numSelections, "num selections not incremented");
assert.gt(ssNew.numTimesCouldNotFind, ssOld.numTimesCouldNotFind, "found new sync source");

rt.stopSet();
