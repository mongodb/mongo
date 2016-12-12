/**
 * Test replication metrics
 */
function testSecondaryMetrics(secondary, opCount, offset) {
    var ss = secondary.getDB("test").serverStatus();
    printjson(ss.metrics);

    assert(ss.metrics.repl.network.readersCreated > 0, "no (oplog) readers created");
    assert(ss.metrics.repl.network.getmores.num > 0, "no getmores");
    assert(ss.metrics.repl.network.getmores.totalMillis > 0, "no getmores time");
    // The first oplog entry may or may not make it into network.ops now that we have two
    // n ops (initiate and new primary) before steady replication starts.
    // Sometimes, we disconnect from our sync source and since our find is a gte query, we may
    // double count an oplog entry, so we need some wiggle room for that.
    assert.lte(ss.metrics.repl.network.ops, opCount + offset + 5, "wrong number of ops retrieved");
    assert.gte(ss.metrics.repl.network.ops, opCount + offset, "wrong number of ops retrieved");
    assert(ss.metrics.repl.network.bytes > 0, "zero or missing network bytes");

    assert(ss.metrics.repl.buffer.count >= 0, "buffer count missing");
    assert(ss.metrics.repl.buffer.sizeBytes >= 0, "size (bytes)] missing");
    assert(ss.metrics.repl.buffer.maxSizeBytes >= 0, "maxSize (bytes) missing");

    assert(ss.metrics.repl.preload.docs.num >= 0, "preload.docs num  missing");
    assert(ss.metrics.repl.preload.docs.totalMillis >= 0, "preload.docs time missing");
    assert(ss.metrics.repl.preload.docs.num >= 0, "preload.indexes num missing");
    assert(ss.metrics.repl.preload.indexes.totalMillis >= 0, "preload.indexes time missing");

    assert(ss.metrics.repl.apply.batches.num > 0, "no batches");
    assert(ss.metrics.repl.apply.batches.totalMillis >= 0, "missing batch time");
    assert.eq(ss.metrics.repl.apply.ops, opCount + offset, "wrong number of applied ops");
}

var rt = new ReplSetTest({name: "server_status_metrics", nodes: 2, oplogSize: 100});
rt.startSet();
rt.initiate();

rt.awaitSecondaryNodes();

var secondary = rt.getSecondary();
var primary = rt.getPrimary();
var testDB = primary.getDB("test");

assert.commandWorked(testDB.createCollection('a'));
assert.writeOK(testDB.b.insert({}, {writeConcern: {w: 2}}));

var ss = secondary.getDB("test").serverStatus();
var secondaryBaseOplogInserts = ss.metrics.repl.apply.ops;

// add test docs
var bulk = testDB.a.initializeUnorderedBulkOp();
for (x = 0; x < 1000; x++) {
    bulk.insert({});
}
assert.writeOK(bulk.execute({w: 2}));

testSecondaryMetrics(secondary, 1000, secondaryBaseOplogInserts);

var options = {writeConcern: {w: 2}, multi: true, upsert: true};
assert.writeOK(testDB.a.update({}, {$set: {d: new Date()}}, options));

testSecondaryMetrics(secondary, 2000, secondaryBaseOplogInserts);

// Test getLastError.wtime and that it only records stats for w > 1, see SERVER-9005
var startMillis = testDB.serverStatus().metrics.getLastError.wtime.totalMillis;
var startNum = testDB.serverStatus().metrics.getLastError.wtime.num;

printjson(primary.getDB("test").serverStatus().metrics);

assert.writeOK(testDB.a.insert({x: 1}, {writeConcern: {w: 1, wtimeout: 5000}}));
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.totalMillis, startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum);

assert.writeOK(testDB.a.insert({x: 1}, {writeConcern: {w: -11, wtimeout: 5000}}));
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.totalMillis, startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum);

assert.writeOK(testDB.a.insert({x: 1}, {writeConcern: {w: 2, wtimeout: 5000}}));
assert(testDB.serverStatus().metrics.getLastError.wtime.totalMillis >= startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum + 1);

// Write will fail because there are only 2 nodes
assert.writeError(testDB.a.insert({x: 1}, {writeConcern: {w: 3, wtimeout: 50}}));
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum + 2);

printjson(primary.getDB("test").serverStatus().metrics);

rt.stopSet();
