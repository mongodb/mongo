/**
 * Test replication metrics
 */
function testSecondaryMetrics(secondary, opCount, offset) {
    var ss = secondary.getDB("test").serverStatus()
    printjson(ss.metrics)

    assert(ss.metrics.repl.network.readersCreated > 0, "no (oplog) readers created")
    assert(ss.metrics.repl.network.getmores.num > 0, "no getmores")
    assert(ss.metrics.repl.network.getmores.totalMillis > 0, "no getmores time")
    assert.eq(ss.metrics.repl.network.ops, opCount + offset, "wrong number of ops retrieved")
    assert(ss.metrics.repl.network.bytes > 0, "zero or missing network bytes")

    assert(ss.metrics.repl.buffer.count >= 0, "buffer count missing")
    assert(ss.metrics.repl.buffer.sizeBytes >= 0, "size (bytes)] missing")
    assert(ss.metrics.repl.buffer.maxSizeBytes >= 0, "maxSize (bytes) missing")

    assert(ss.metrics.repl.preload.docs.num >= 0, "preload.docs num  missing")
    assert(ss.metrics.repl.preload.docs.totalMillis  >= 0, "preload.docs time missing")
    assert(ss.metrics.repl.preload.docs.num >= 0, "preload.indexes num missing")
    assert(ss.metrics.repl.preload.indexes.totalMillis >= 0, "preload.indexes time missing")

    assert(ss.metrics.repl.apply.batches.num > 0, "no batches")
    assert(ss.metrics.repl.apply.batches.totalMillis > 0, "no batch time")
    assert.eq(ss.metrics.repl.apply.ops, opCount + offset, "wrong number of applied ops")
}

function testPrimaryMetrics(primary, opCount, offset) {
    var ss = primary.getDB("test").serverStatus()
    printjson(ss.metrics)

    assert.eq(ss.metrics.repl.oplog.insert.num, opCount + offset, "wrong oplog insert count")
    assert(ss.metrics.repl.oplog.insert.totalMillis >= 0, "no oplog inserts time")
    assert(ss.metrics.repl.oplog.insertBytes > 0, "no oplog inserted bytes")
}

var rt = new ReplSetTest( { name : "server_status_metrics" , nodes: 2, oplogSize: 100 } );
rt.startSet()
rt.initiate()

rt.awaitSecondaryNodes();

var secondary = rt.getSecondary();
var primary = rt.getPrimary();
var testDB = primary.getDB("test");

var ss = primary.getDB("test").serverStatus();
var primaryBaseOplogInserts = ss.metrics.repl.oplog.insert.num;

var ss = secondary.getDB("test").serverStatus();
var secondaryBaseOplogInserts = ss.metrics.repl.oplog.insert.num;

//add test docs
for(x=0;x<10000;x++){ testDB.a.insert({}) }

testPrimaryMetrics(primary, 10000, primaryBaseOplogInserts);
testDB.getLastError(2);

testSecondaryMetrics(secondary, 10000, secondaryBaseOplogInserts -1);

testDB.a.update({}, {$set:{d:new Date()}},true, true)
testDB.getLastError(2);

testSecondaryMetrics(secondary, 20000, secondaryBaseOplogInserts -1);


// Test getLastError.wtime and that it only records stats for w > 1, see SERVER-9005
var startMillis = testDB.serverStatus().metrics.getLastError.wtime.totalMillis
var startNum = testDB.serverStatus().metrics.getLastError.wtime.num

printjson(primary.getDB("test").serverStatus().metrics);

testDB.getLastError(1, 50 );
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.totalMillis, startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum);

testDB.getLastError(-11, 50 );
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.totalMillis, startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum);

testDB.getLastError(2, 50 );
assert(testDB.serverStatus().metrics.getLastError.wtime.totalMillis >= startMillis);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum + 1);

testDB.getLastError(3, 50 );
assert(testDB.serverStatus().metrics.getLastError.wtime.totalMillis >= startMillis + 50);
assert.eq(testDB.serverStatus().metrics.getLastError.wtime.num, startNum + 2);

printjson(primary.getDB("test").serverStatus().metrics);

rt.stopSet();
