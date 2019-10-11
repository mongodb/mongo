
(function() {
"use strict";

load("jstests/libs/check_log.js");
load("jstests/libs/write_concern_util.js");

/**
 * Test replication metrics
 */
function testSecondaryMetrics(secondary, opCount, baseOpsApplied, baseOpsReceived) {
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

var rt = new ReplSetTest({
    name: "server_status_metrics",
    nodes: 2,
    oplogSize: 100,
    // Write periodic noops to aid sync source selection. ReplSetTest.initiate() requires at least a
    // 2 second noop writer interval to converge on a lastApplied optime.
    nodeOptions: {setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 2}}
});
rt.startSet();
rt.initiateWithHighElectionTimeout();

rt.awaitSecondaryNodes();

var secondary = rt.getSecondary();
var primary = rt.getPrimary();
var testDB = primary.getDB("test");

assert.commandWorked(testDB.createCollection('a'));
assert.commandWorked(testDB.b.insert({}, {writeConcern: {w: 2}}));

var ss = secondary.getDB("test").serverStatus();
// The number of ops received  and the number of ops applied are not guaranteed to be the same
// during initial sync oplog application as we apply received operations only if the operation's
// optime is greater than OplogApplier::Options::beginApplyingOpTime.
var secondaryBaseOplogOpsApplied = ss.metrics.repl.apply.ops;
var secondaryBaseOplogOpsReceived = ss.metrics.repl.apply.batchSize;

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

// Test getLastError.wtime and that it only records stats for w > 1, see SERVER-9005
var startMillis = testDB.serverStatus().metrics.getLastError.wtime.totalMillis;
var startNum = testDB.serverStatus().metrics.getLastError.wtime.num;

jsTestLog(
    `Primary ${primary.host} metrics #1: ${tojson(primary.getDB("test").serverStatus().metrics)}`);

assert.commandWorked(testDB.a.insert({x: 1}, {writeConcern: {w: 1, wtimeout: 5000}}));
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.totalMillis, startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum);

assert.commandWorked(testDB.a.insert({x: 1}, {writeConcern: {w: -11, wtimeout: 5000}}));
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.totalMillis, startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum);

assert.commandWorked(testDB.a.insert({x: 1}, {writeConcern: {w: 2, wtimeout: 5000}}));
assert(testDB.serverStatus().metrics.getLastError.wtime.totalMillis >= startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum + 1);

// Write will fail because there are only 2 nodes
assert.writeError(testDB.a.insert({x: 1}, {writeConcern: {w: 3, wtimeout: 50}}));
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum + 2);

jsTestLog(
    `Primary ${primary.host} metrics #2: ${tojson(primary.getDB("test").serverStatus().metrics)}`);

let ssOld = secondary.getDB("test").serverStatus().metrics.repl.syncSource;
jsTestLog(`Secondary ${secondary.host} metrics before restarting replication: ${tojson(ssOld)}`);

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
    3 * 1000 /* 3sec interval to wait for noop */);

assert.gt(ssNew.numSelections, ssOld.numSelections, "num selections not incremented");
assert.gt(ssNew.numTimesChoseSame, ssOld.numTimesChoseSame, "same sync source not chosen");

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
})();
