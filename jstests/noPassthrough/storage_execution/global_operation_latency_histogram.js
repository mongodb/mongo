// Checks that global histogram counters for collections are updated as we expect.
// @tags: [
//   requires_replication,
// ]

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Get the current opLatencies histograms from testDB.
function getLatencyHistogramStats(testDB) {
    return testDB.serverStatus({opLatencies: {histograms: 1}}).opLatencies;
}

// Get the current opWorkingTime histograms from testDB.
function getWorkingTimeHistogramStats(testDB) {
    return testDB.serverStatus({opWorkingTime: {histograms: 1}}).opWorkingTime;
}

// Checks that the difference in the histogram is what we expect, and also
// accounts for the serverStatus command itself.
function checkHistogramDiff(getHistogramStats, lastHistogram, testDB, reads, writes, commands) {
    const thisHistogram = getHistogramStats(testDB);
    assert.eq(thisHistogram.reads.ops - lastHistogram.reads.ops, reads);
    assert.eq(thisHistogram.writes.ops - lastHistogram.writes.ops, writes);
    // Running the server status itself will increment command stats by one.
    assert.eq(thisHistogram.commands.ops - lastHistogram.commands.ops, commands + 1);
    return thisHistogram;
}

// Run the set of tests on a given DB and collection. This is called for both mongod and mongos.
// isMongos is used to skip commands that do not exist when run on mongos.
function runTests(getHistogramStats, testDB, testColl, isMongos) {
    const numRecords = 100;
    let lastHistogram = getHistogramStats(testDB);

    // Insert
    for (var i = 0; i < numRecords; i++) {
        assert.commandWorked(testColl.insert({_id: i}));
    }
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, numRecords, 0);

    // Update
    for (var i = 0; i < numRecords; i++) {
        assert.commandWorked(testColl.update({_id: i}, {x: i}));
    }
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, numRecords, 0);

    // Find
    let cursors = [];
    for (var i = 0; i < numRecords; i++) {
        cursors[i] = testColl.find({x: {$gte: i}}).batchSize(2);
        assert.eq(cursors[i].next()._id, i);
    }
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, numRecords, 0, 0);

    // GetMore
    for (var i = 0; i < numRecords / 2; i++) {
        // Trigger two getmore commands.
        assert.eq(cursors[i].next()._id, i + 1);
        assert.eq(cursors[i].next()._id, i + 2);
        assert.eq(cursors[i].next()._id, i + 3);
        assert.eq(cursors[i].next()._id, i + 4);
    }
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, numRecords, 0, 0);

    // KillCursors
    // The last cursor has no additional results, hence does not need to be closed.
    for (var i = 0; i < numRecords - 1; i++) {
        cursors[i].close();
    }
    lastHistogram =
        checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, numRecords - 1);

    // Remove
    for (var i = 0; i < numRecords; i++) {
        assert.commandWorked(testColl.remove({_id: i}));
    }
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, numRecords, 0);

    // Upsert
    for (var i = 0; i < numRecords; i++) {
        assert.commandWorked(testColl.update({_id: i}, {x: i}, {upsert: 1}));
    }
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, numRecords, 0);

    // Aggregate
    for (var i = 0; i < numRecords; i++) {
        testColl.aggregate([{$match: {x: i}}, {$group: {_id: "$x"}}]);
    }
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, numRecords, 0, 0);

    // Count
    for (var i = 0; i < numRecords; i++) {
        testColl.count({x: i});
    }
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, numRecords, 0, 0);

    // FindAndModify
    testColl.findAndModify({query: {}, update: {pt: {type: "Point", coordinates: [0, 0]}}});
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 1, 0);

    // CreateIndex
    assert.commandWorked(testColl.createIndex({pt: "2dsphere"}));
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);

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
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 1, 0, 0);

    // GetIndexes
    testColl.getIndexes();
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);

    if (!isMongos) {
        // Reindex is deprecated on mongod and does not exist on mongos.
        assert.commandWorked(testColl.reIndex());
        lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);
    }

    // DropIndex
    assert.commandWorked(testColl.dropIndex({pt: "2dsphere"}));
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);

    // Explain
    testColl.explain().find().next();
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);

    // CollStats
    assert.commandWorked(testDB.runCommand({collStats: testColl.getName()}));
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);

    // CollMod
    assert.commandWorked(
        testDB.runCommand({collStats: testColl.getName(), validationLevel: "off"}));
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);

    // Compact
    const commandResult = testDB.runCommand({compact: testColl.getName()});
    // The storage engine may not support compact or if it does, it can be interrupted because of
    // cache pressure or concurrent calls to compact.
    if (!commandResult.ok) {
        assert.commandFailedWithCode(commandResult,
                                     [ErrorCodes.CommandNotSupported, ErrorCodes.Interrupted],
                                     tojson(commandResult));
    }
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);

    // DataSize
    testColl.dataSize();
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);

    // PlanCache
    testColl.getPlanCache().list();
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 1, 0, 0);

    // ServerStatus
    assert.commandWorked(testDB.serverStatus());
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);

    // WhatsMyURI
    assert.commandWorked(testColl.runCommand("whatsmyuri"));
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);

    // Test non-command.
    assert.commandFailed(testColl.runCommand("IHopeNobodyEverMakesThisACommand"));
    lastHistogram = checkHistogramDiff(getHistogramStats, lastHistogram, testDB, 0, 0, 1);
}

function opLatencies(node) {
    return node.serverStatus().opLatencies;
}

function opWorkingTime(node) {
    return node.serverStatus().opWorkingTime;
}

// Do a sharding latency test that runs insert and read operations on the newly created collection
// and verifies the latency measured on mongos >= the latency measured on mongod. On a sharded
// cluster there are background operations on the mongod, so this only compares the latencies if
// no extra operations of the corresponding type have occurred on the mongod.
function runLatencyComparisonTest(getOpTimes, st, testDB, testColl) {
    const kObjId = 99999;
    const shard = st.shard0.getDB("test");

    // Write test
    let opTimesMongoSOld = getOpTimes(testDB);
    let opTimesMongoDOld = getOpTimes(shard);
    assert.commandWorked(testColl.insert({_id: kObjId}));
    let opTimesMongoSNew = getOpTimes(testDB);
    let opTimesMongoDNew = getOpTimes(shard);
    let opsMongoD = opTimesMongoDNew.writes.ops - opTimesMongoDOld.writes.ops;
    if (opsMongoD == 1) {
        assert.gte((opTimesMongoSNew.writes.latency - opTimesMongoSOld.writes.latency),
                   (opTimesMongoDNew.writes.latency - opTimesMongoDOld.writes.latency),
                   {
                       "mongoSOld": opTimesMongoSOld,
                       "mongoSNew": opTimesMongoSNew,
                       "mongoDOld": opTimesMongoDOld,
                       "mongoDNew": opTimesMongoDNew
                   });
    }

    // Read test
    opTimesMongoSOld = opTimesMongoSNew;
    opTimesMongoDOld = opTimesMongoDNew;
    assert.eq(kObjId, testColl.findOne()._id);
    opTimesMongoSNew = getOpTimes(testDB);
    opTimesMongoDNew = getOpTimes(shard);
    opsMongoD = opTimesMongoDNew.reads.ops - opTimesMongoDOld.reads.ops;
    if (opsMongoD == 1) {
        assert.gte((opTimesMongoSNew.reads.latency - opTimesMongoSOld.reads.latency),
                   (opTimesMongoDNew.reads.latency - opTimesMongoDOld.reads.latency),
                   {
                       "mongoSOld": opTimesMongoSOld,
                       "mongoSNew": opTimesMongoSNew,
                       "mongoDOld": opTimesMongoDOld,
                       "mongoDNew": opTimesMongoDNew
                   });
    }
}

const latencyName = "opLatencyHistogramTest";
const workingTimeName = "opWorkingTimeHistogramTest";

// Run tests against mongod.
const mongod = MongoRunner.runMongod();
let testDB = mongod.getDB("test");
let testColl = testDB[latencyName + "coll"];
testColl.drop();
runTests(getLatencyHistogramStats, testDB, testColl, false);

testColl = testDB[workingTimeName + "coll"];
testColl.drop();
runTests(getWorkingTimeHistogramStats, testDB, testColl, false);

MongoRunner.stopMongod(mongod);

// Run tests against mongos.
const st = new ShardingTest({config: 1, shards: 1});
testDB = st.s.getDB("test");
testColl = testDB[latencyName + "coll"];
testColl.drop();
runLatencyComparisonTest(opLatencies, st, testDB, testColl);
runTests(getLatencyHistogramStats, testDB, testColl, true);

testColl = testDB[workingTimeName + "coll"];
testColl.drop();
runLatencyComparisonTest(opWorkingTime, st, testDB, testColl);
runTests(getWorkingTimeHistogramStats, testDB, testColl, true);

st.stop();
