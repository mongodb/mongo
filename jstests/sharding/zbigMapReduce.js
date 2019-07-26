/**
 * This test is labeled resource intensive because its total io_write is 625MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [resource_intensive]
 */
(function() {
'use strict';

let s = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        rs: true,
        numReplicas: 2,
        chunkSize: 1,
        rsOptions: {oplogSize: 50},
        enableAutoSplit: true,
    }
});

assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
s.ensurePrimaryShard('test', s.shard0.shardName);
assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {"_id": 1}}));

let testDb = s.getDB("test");

jsTest.log("Inserting a lot of documents into test.foo");

// Make each document data to be 5K so that the total size is ~250MB
const str = "#".repeat(5 * 1024);

var idInc = 0;
var valInc = 0;

var bulk = testDb.foo.initializeUnorderedBulkOp();
for (var j = 0; j < 100; j++) {
    for (var i = 0; i < 512; i++) {
        bulk.insert({i: idInc++, val: valInc++, y: str});
    }
}
assert.writeOK(bulk.execute({w: 2, wtimeout: 10 * 60 * 1000}));

jsTest.log("Documents inserted, doing double-checks of insert...");

// Collect some useful stats to figure out what happened
if (testDb.foo.find().itcount() != 51200) {
    s.printShardingStatus(true);

    print("Shard 0: " + s.shard0.getCollection(testDb.foo + "").find().itcount());
    print("Shard 1: " + s.shard1.getCollection(testDb.foo + "").find().itcount());

    for (var i = 0; i < 51200; i++) {
        if (!testDb.foo.findOne({i: i}, {i: 1})) {
            print("Could not find: " + i);
        }

        if (i % 100 == 0)
            print("Checked " + i);
    }

    assert(false, 'Incorect number of chunks found!');
}

s.printChunks(testDb.foo.getFullName());
s.printChangeLog();

function map() {
    emit('count', 1);
}
function reduce(key, values) {
    return Array.sum(values);
}

// Let chunks move around while map reduce is running
s.startBalancer();

jsTest.log("Test basic mapreduce...");

// Test basic mapReduce
for (var iter = 0; iter < 5; iter++) {
    print("Test #" + iter);
    testDb.foo.mapReduce(map, reduce, "big_out");
}

print("Testing output to different db...");

// Test output to a different DB - do it multiple times so that the merging shard changes
for (var iter = 0; iter < 5; iter++) {
    print("Test #" + iter);

    assert.eq(51200, testDb.foo.find().itcount(), "Not all data was found!");

    let outCollStr = "mr_replace_col_" + iter;
    let outDbStr = "mr_db_" + iter;

    print("Testing mr replace into DB " + iter);

    var res = testDb.foo.mapReduce(map, reduce, {out: {replace: outCollStr, db: outDbStr}});
    printjson(res);

    var outDb = s.getDB(outDbStr);
    var outColl = outDb[outCollStr];

    var obj = outColl.convertToSingleObject("value");
    assert.eq(51200, obj.count, "Received wrong result " + obj.count);

    print("Checking result field");
    assert.eq(res.result.collection, outCollStr, "Wrong collection " + res.result.collection);
    assert.eq(res.result.db, outDbStr, "Wrong db " + res.result.db);
}

jsTest.log("Verifying nonatomic M/R throws...");

// Check nonAtomic output
assert.throws(function() {
    testDb.foo.mapReduce(map, reduce, {out: {replace: "big_out", nonAtomic: true}});
});

jsTest.log("Adding documents");

// Add docs with dup "i"
valInc = 0;
for (var j = 0; j < 100; j++) {
    print("Inserted document: " + (j * 100));
    var bulk = testDb.foo.initializeUnorderedBulkOp();
    for (i = 0; i < 512; i++) {
        bulk.insert({i: idInc++, val: valInc++, y: str});
    }
    assert.writeOK(bulk.execute({w: 2, wtimeout: 10 * 60 * 1000}));
}

jsTest.log("No errors...");

function map2() {
    emit(this.val, 1);
}
function reduce2(key, values) {
    return Array.sum(values);
}

// Test merge
let outColMerge = 'big_out_merge';

// M/R quarter of the docs
{
    jsTestLog("Test A");
    var out =
        testDb.foo.mapReduce(map2, reduce2, {query: {i: {$lt: 25600}}, out: {merge: outColMerge}});
    printjson(out);
    assert.eq(25600, out.counts.emit, "Received wrong result");
    assert.eq(25600, out.counts.output, "Received wrong result");
}

// M/R further docs
{
    jsTestLog("Test B");
    var out = testDb.foo.mapReduce(
        map2, reduce2, {query: {i: {$gte: 25600, $lt: 51200}}, out: {merge: outColMerge}});
    printjson(out);
    assert.eq(25600, out.counts.emit, "Received wrong result");
    assert.eq(51200, out.counts.output, "Received wrong result");
}

// M/R do 2nd half of docs
{
    jsTestLog("Test C");
    var out = testDb.foo.mapReduce(
        map2, reduce2, {query: {i: {$gte: 51200}}, out: {merge: outColMerge, nonAtomic: true}});
    printjson(out);
    assert.eq(51200, out.counts.emit, "Received wrong result");
    assert.eq(51200, out.counts.output, "Received wrong result");
    assert.eq(1, testDb[outColMerge].findOne().value, "Received wrong result");
}

// Test reduce
let outColReduce = "big_out_reduce";

// M/R quarter of the docs
{
    jsTestLog("Test D");
    var out = testDb.foo.mapReduce(
        map2, reduce2, {query: {i: {$lt: 25600}}, out: {reduce: outColReduce}});
    printjson(out);
    assert.eq(25600, out.counts.emit, "Received wrong result");
    assert.eq(25600, out.counts.output, "Received wrong result");
}

// M/R further docs
{
    jsTestLog("Test E");
    var out = testDb.foo.mapReduce(
        map2, reduce2, {query: {i: {$gte: 25600, $lt: 51200}}, out: {reduce: outColReduce}});
    printjson(out);
    assert.eq(25600, out.counts.emit, "Received wrong result");
    assert.eq(51200, out.counts.output, "Received wrong result");
}

// M/R do 2nd half of docs
{
    jsTestLog("Test F");
    var out = testDb.foo.mapReduce(
        map2, reduce2, {query: {i: {$gte: 51200}}, out: {reduce: outColReduce, nonAtomic: true}});
    printjson(out);
    assert.eq(51200, out.counts.emit, "Received wrong result");
    assert.eq(51200, out.counts.output, "Received wrong result");
    assert.eq(2, testDb[outColReduce].findOne().value, "Received wrong result");
}

// Verify that data is also on secondary
{
    jsTestLog("Test G");
    var primary = s.rs0._master;
    var secondaries = s.rs0._slaves;

    // Stop the balancer to prevent new writes from happening and make sure that replication can
    // keep up even on slow machines
    s.stopBalancer();
    s.rs0.awaitReplication();
    assert.eq(51200, primary.getDB("test")[outColReduce].find().itcount(), "Wrong count");

    for (var i = 0; i < secondaries.length; ++i) {
        assert.eq(
            51200, secondaries[i].getDB("test")[outColReduce].find().itcount(), "Wrong count");
    }
}

s.stop();
})();
