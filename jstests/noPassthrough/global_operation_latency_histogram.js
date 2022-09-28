// Checks that global histogram counters for collections are updated as we expect.
// @tags: [
//   requires_replication,
// ]

(function() {
"use strict";
const name = "operationalLatencyHistogramTest";

// Get the current opLatencies histograms from testDB.
function getHistogramStats(testDB) {
    return testDB.serverStatus({opLatencies: {histograms: 1}}).opLatencies;
}

// Checks that the difference in the histogram is what we expect, and also
// accounts for the serverStatus command itself.
function checkHistogramDiff(lastHistogram, testDB, reads, writes, commands) {
    const thisHistogram = getHistogramStats(testDB);
    assert.eq(thisHistogram.reads.ops - lastHistogram.reads.ops, reads);
    assert.eq(thisHistogram.writes.ops - lastHistogram.writes.ops, writes);
    // Running the server status itself will increment command stats by one.
    assert.eq(thisHistogram.commands.ops - lastHistogram.commands.ops, commands + 1);
    return thisHistogram;
}

// Run the set of tests on a given DB and collection. This is called for both mongod and mongos.
// isMongos is used to skip commands that do not exist when run on mongos.
function runTests(testDB, isMongos) {
    const numRecords = 100;
    const testColl = testDB[name + "coll"];
    let lastHistogram = getHistogramStats(testDB);

    // Insert
    for (var i = 0; i < numRecords; i++) {
        assert.commandWorked(testColl.insert({_id: i}));
    }
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, numRecords, 0);

    // Update
    for (var i = 0; i < numRecords; i++) {
        assert.commandWorked(testColl.update({_id: i}, {x: i}));
    }
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, numRecords, 0);

    // Find
    let cursors = [];
    for (var i = 0; i < numRecords; i++) {
        cursors[i] = testColl.find({x: {$gte: i}}).batchSize(2);
        assert.eq(cursors[i].next()._id, i);
    }
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, numRecords, 0, 0);

    // GetMore
    for (var i = 0; i < numRecords / 2; i++) {
        // Trigger two getmore commands.
        assert.eq(cursors[i].next()._id, i + 1);
        assert.eq(cursors[i].next()._id, i + 2);
        assert.eq(cursors[i].next()._id, i + 3);
        assert.eq(cursors[i].next()._id, i + 4);
    }
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, numRecords, 0, 0);

    // KillCursors
    // The last cursor has no additional results, hence does not need to be closed.
    for (var i = 0; i < numRecords - 1; i++) {
        cursors[i].close();
    }
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, numRecords - 1);

    // Remove
    for (var i = 0; i < numRecords; i++) {
        assert.commandWorked(testColl.remove({_id: i}));
    }
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, numRecords, 0);

    // Upsert
    for (var i = 0; i < numRecords; i++) {
        assert.commandWorked(testColl.update({_id: i}, {x: i}, {upsert: 1}));
    }
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, numRecords, 0);

    // Aggregate
    for (var i = 0; i < numRecords; i++) {
        testColl.aggregate([{$match: {x: i}}, {$group: {_id: "$x"}}]);
    }
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, numRecords, 0, 0);

    // Count
    for (var i = 0; i < numRecords; i++) {
        testColl.count({x: i});
    }
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, numRecords, 0, 0);

    // FindAndModify
    testColl.findAndModify({query: {}, update: {pt: {type: "Point", coordinates: [0, 0]}}});
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 1, 0);

    // CreateIndex
    assert.commandWorked(testColl.createIndex({pt: "2dsphere"}));
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);

    // $geoNear aggregation stage
    assert.commandWorked(testDB.runCommand({
        aggregate: testColl.getName(),
        pipeline: [{
            $geoNear: {
                near: {type: "Point", coordinates: [0, 0]},
                spherical: true,
                distanceField: "dist",
            }
        }],
        cursor: {},
    }));
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 1, 0, 0);

    // GetIndexes
    testColl.getIndexes();
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);

    if (!isMongos) {
        // Reindex is deprecated on mongod and does not exist on mongos.
        assert.commandWorked(testColl.reIndex());
        lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);
    }

    // DropIndex
    assert.commandWorked(testColl.dropIndex({pt: "2dsphere"}));
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);

    // Explain
    testColl.explain().find().next();
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);

    // CollStats
    assert.commandWorked(testDB.runCommand({collStats: testColl.getName()}));
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);

    // CollMod
    assert.commandWorked(
        testDB.runCommand({collStats: testColl.getName(), validationLevel: "off"}));
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);

    // Compact
    const commandResult = testDB.runCommand({compact: testColl.getName()});
    // If storage engine supports compact, it should count as a command.
    if (!commandResult.ok) {
        assert.commandFailedWithCode(commandResult, ErrorCodes.CommandNotSupported);
    }
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);

    // DataSize
    testColl.dataSize();
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);

    // PlanCache
    testColl.getPlanCache().list();
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 1, 0, 0);

    // ServerStatus
    assert.commandWorked(testDB.serverStatus());
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);

    // WhatsMyURI
    assert.commandWorked(testColl.runCommand("whatsmyuri"));
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);

    // Test non-command.
    assert.commandFailed(testColl.runCommand("IHopeNobodyEverMakesThisACommand"));
    lastHistogram = checkHistogramDiff(lastHistogram, testDB, 0, 0, 1);
}

// Do a sharding latency test that runs insert and read operations on the newly created collection
// and verifies the latency measured on mongos >= the latency measured on mongod. On a sharded
// cluster there are background operations on the mongod, so this only compares the latencies if
// no extra operations of the corresponding type have occurred on the mongod.
function runLatencyComparisonTest(st, testDB) {
    const kObjId = 99999;
    const testColl = testDB[name + "coll"];
    const shard = st.shard0.getDB("test");

    // Write test
    let opLatenciesMongoSOld = testDB.serverStatus().opLatencies;
    let opLatenciesMongoDOld = shard.serverStatus().opLatencies;
    assert.commandWorked(testColl.insert({_id: kObjId}));
    let opLatenciesMongoSNew = testDB.serverStatus().opLatencies;
    let opLatenciesMongoDNew = shard.serverStatus().opLatencies;
    let opsMongoD = opLatenciesMongoDNew.writes.ops - opLatenciesMongoDOld.writes.ops;
    if (opsMongoD == 1) {
        assert.gte((opLatenciesMongoSNew.writes.latency - opLatenciesMongoSOld.writes.latency),
                   (opLatenciesMongoDNew.writes.latency - opLatenciesMongoDOld.writes.latency),
                   {
                       "mongoSOld": opLatenciesMongoSOld,
                       "mongoSNew": opLatenciesMongoSNew,
                       "mongoDOld": opLatenciesMongoDOld,
                       "mongoDNew": opLatenciesMongoDNew
                   });
    }

    // Read test
    opLatenciesMongoSOld = opLatenciesMongoSNew;
    opLatenciesMongoDOld = opLatenciesMongoDNew;
    assert.eq(kObjId, testColl.findOne()._id);
    opLatenciesMongoSNew = testDB.serverStatus().opLatencies;
    opLatenciesMongoDNew = shard.serverStatus().opLatencies;
    opsMongoD = opLatenciesMongoDNew.reads.ops - opLatenciesMongoDOld.reads.ops;
    if (opsMongoD == 1) {
        assert.gte((opLatenciesMongoSNew.reads.latency - opLatenciesMongoSOld.reads.latency),
                   (opLatenciesMongoDNew.reads.latency - opLatenciesMongoDOld.reads.latency),
                   {
                       "mongoSOld": opLatenciesMongoSOld,
                       "mongoSNew": opLatenciesMongoSNew,
                       "mongoDOld": opLatenciesMongoDOld,
                       "mongoDNew": opLatenciesMongoDNew
                   });
    }
}

// Run tests against mongod.
const mongod = MongoRunner.runMongod();
let testDB = mongod.getDB("test");
let testColl = testDB[name + "coll"];
testColl.drop();
runTests(testDB, false);
MongoRunner.stopMongod(mongod);

// Run tests against mongos.
const st = new ShardingTest({config: 1, shards: 1});
testDB = st.s.getDB("test");
testColl = testDB[name + "coll"];
testColl.drop();
runLatencyComparisonTest(st, testDB);
runTests(testDB, true);
st.stop();
}());
