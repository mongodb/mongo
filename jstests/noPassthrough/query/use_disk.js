// @tags: [
//   does_not_support_stepdowns,
//   requires_persistence,
//   requires_profiling,
//   requires_sharding,
// ]

// Confirms that profiled aggregation execution contains expected values for usedDisk.

import {
    getLatestProfilerEntry,
    profilerHasSingleMatchingEntryOrThrow,
    profilerHasZeroMatchingEntriesOrThrow,
} from "jstests/libs/profiler.js";
import {
    getAggPlanStages,
    getPlanStage,
    getWinningPlanFromExplain
} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const conn = MongoRunner.runMongod();
const testDB = conn.getDB("profile_agg");
const collName = jsTestName();
const coll = testDB.getCollection(collName);

testDB.setProfilingLevel(2);

function resetCollection() {
    coll.drop();
    for (var i = 0; i < 10; ++i) {
        assert.commandWorked(coll.insert({a: i}));
    }
}
function resetForeignCollection() {
    testDB.foreign.drop();
    const forColl = testDB.getCollection("foreign");
    for (var i = 4; i < 18; i += 2)
        assert.commandWorked(forColl.insert({b: i}));
}
//
// Confirm hasSortStage with in-memory sort.
//
resetCollection();
//
// Confirm 'usedDisk' is not set if 'allowDiskUse' is set but no stages need to use disk.
//
coll.aggregate([{$match: {a: {$gte: 2}}}], {allowDiskUse: true});
var profileObj = getLatestProfilerEntry(testDB);
assert(!profileObj.hasOwnProperty("usedDisk"), tojson(profileObj));

resetCollection();
coll.aggregate([{$match: {a: {$gte: 2}}}, {$sort: {a: 1}}], {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert(!profileObj.hasOwnProperty("usedDisk"), tojson(profileObj));
assert.eq(profileObj.hasSortStage, true, tojson(profileObj));

assert.commandWorked(
    testDB.adminCommand({setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 10}));
assert.eq(
    8, coll.aggregate([{$match: {a: {$gte: 2}}}, {$sort: {a: 1}}], {allowDiskUse: true}).itcount());
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.usedDisk, true, tojson(profileObj));
assert.eq(profileObj.hasSortStage, true, tojson(profileObj));

//
// Confirm that disk use is correctly detected for the $sort stage and the sorter spilling metrics
// are outputted.
//
resetCollection();

coll.aggregate([{$sort: {a: 1}}], {comment: "sort_spill"});
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.command.comment, "sort_spill", tojson(profileObj));
assert.eq(profileObj.usedDisk, true, tojson(profileObj));
assert.gt(profileObj.sortSpills, 0, tojson(profileObj));
assert.gt(profileObj.sortSpilledBytes, 0, tojson(profileObj));
assert.gt(profileObj.sortSpilledRecords, 0, tojson(profileObj));
assert.gt(profileObj.sortSpilledDataStorageSize, 0, tojson(profileObj));
assert.gt(profileObj.sortTotalDataSizeBytes, 0, tojson(profileObj));

//
// Confirm that disk use is correctly detected for the $facet stage.
//
resetCollection();
coll.aggregate([{$facet: {"aSort": [{$sortByCount: "$a"}]}}], {allowDiskUse: true});

profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.usedDisk, true, tojson(profileObj));

//
// Confirm that usedDisk is correctly detected for the $group stage.
//
resetCollection();

coll.aggregate([{$group: {"_id": {$avg: "$a"}}}], {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert(!profileObj.hasOwnProperty("usedDisk"), tojson(profileObj));

assert.commandWorked(
    testDB.adminCommand({setParameter: 1, internalDocumentSourceGroupMaxMemoryBytes: 10}));
resetCollection();
coll.aggregate([{$group: {"_id": {$avg: "$a"}}}], {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.usedDisk, true, tojson(profileObj));
assert.gt(profileObj.groupSpills, 0, tojson(profileObj));
assert.gt(profileObj.groupSpilledBytes, 0, tojson(profileObj));
assert.gt(profileObj.groupSpilledRecords, 0, tojson(profileObj));
assert.gt(profileObj.groupSpilledDataStorageSize, 0, tojson(profileObj));

//
// Confirm that usedDisk is correctly detected for the TEXT_OR stage.
//
coll.drop();
assert.commandWorked(
    coll.insertMany([{a: "green tea", b: 5}, {a: "black tea", b: 6}, {a: "black coffee", b: 7}]));
assert.commandWorked(coll.createIndex({a: "text"}));
assert.commandWorked(testDB.adminCommand({setParameter: 1, internalTextOrStageMaxMemoryBytes: 1}));

coll.aggregate(
    [{$match: {$text: {$search: "black tea"}}}, {$addFields: {score: {$meta: "textScore"}}}],
    {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.usedDisk, true, tojson(profileObj));
assert.gt(profileObj.textOrSpills, 0, tojson(profileObj));
assert.gt(profileObj.textOrSpilledBytes, 0, tojson(profileObj));
assert.gt(profileObj.textOrSpilledRecords, 0, tojson(profileObj));
assert.gt(profileObj.textOrSpilledDataStorageSize, 0, tojson(profileObj));

coll.aggregate([{$match: {$text: {$search: "black tea"}}}, {$sort: {_: {$meta: "textScore"}}}],
               {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.usedDisk, true, tojson(profileObj));
assert.gt(profileObj.textOrSpills, 0, tojson(profileObj));
assert.gt(profileObj.textOrSpilledBytes, 0, tojson(profileObj));
assert.gt(profileObj.textOrSpilledRecords, 0, tojson(profileObj));
assert.gt(profileObj.textOrSpilledDataStorageSize, 0, tojson(profileObj));
assert.gt(profileObj.sortSpills, 0, tojson(profileObj));
assert.gt(profileObj.sortSpilledBytes, 0, tojson(profileObj));
assert.gt(profileObj.sortSpilledRecords, 0, tojson(profileObj));
assert.gt(profileObj.sortSpilledDataStorageSize, 0, tojson(profileObj));

//
// Confirm that usedDisk is correctly detected for the $setWindowFields stage.
//

const setWindowFieldsPipeline =
    [{$setWindowFields: {sortBy: {a: 1}, output: {as: {$addToSet: "$a"}}}}];

function getSetWindowFieldsMemoryLimit() {
    const explain = coll.explain().aggregate(setWindowFieldsPipeline);
    // If $setWindowFields was pushed down to SBE, set a lower limit. We can't set it to 1 byte
    // for Classic because DocumentSourceSetWindowFields will fail if it still doesn't fit into
    // memory limit after spilling.
    if (getPlanStage(getWinningPlanFromExplain(explain), "WINDOW")) {
        return 1;
    } else {
        return 500;
    }
}

assert.commandWorked(testDB.adminCommand({
    setParameter: 1,
    internalDocumentSourceSetWindowFieldsMaxMemoryBytes: getSetWindowFieldsMemoryLimit()
}));
resetCollection();
coll.aggregate(setWindowFieldsPipeline, {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.usedDisk, true, tojson(profileObj));
assert.gt(profileObj.setWindowFieldsSpills, 0, tojson(profileObj));
assert.gt(profileObj.setWindowFieldsSpilledBytes, 0, tojson(profileObj));
assert.gt(profileObj.setWindowFieldsSpilledRecords, 0, tojson(profileObj));
assert.gt(profileObj.setWindowFieldsSpilledDataStorageSize, 0, tojson(profileObj));
assert.gt(profileObj.sortSpills, 0, tojson(profileObj));
assert.gt(profileObj.sortSpilledBytes, 0, tojson(profileObj));
assert.gt(profileObj.sortSpilledRecords, 0, tojson(profileObj));
assert.gt(profileObj.sortSpilledDataStorageSize, 0, tojson(profileObj));

//
// Confirm that usedDisk is correctly detected for the $lookup stage with a subsequent $unwind.
//
resetCollection();
resetForeignCollection();
coll.aggregate(
    [
        {$lookup: {let : {var1: "$a"}, pipeline: [{$sort: {a: 1}}], from: "foreign", as: "same"}},
        {$unwind: "$same"}
    ],
    {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.usedDisk, true, tojson(profileObj));

//
// Confirm that usedDisk is correctly detected for the $lookup stage without a subsequent
// $unwind.
//
resetCollection();
resetForeignCollection();
coll.aggregate(
    [{$lookup: {let : {var1: "$a"}, pipeline: [{$sort: {a: 1}}], from: "foreign", as: "same"}}],
    {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.usedDisk, true, tojson(profileObj));

//
// Confirm that usedDisk is correctly detected when $limit is set after the $lookup stage.
//
resetCollection();
resetForeignCollection();
coll.aggregate(
    [
        {$lookup: {let : {var1: "$a"}, pipeline: [{$sort: {a: 1}}], from: "foreign", as: "same"}},
        {$unwind: "$same"},
        {$limit: 3}
    ],
    {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.usedDisk, true, tojson(profileObj));

//
// Confirm that usedDisk is correctly detected when $limit is set before the $lookup stage.
//
resetCollection();
resetForeignCollection();
coll.aggregate(
    [
        {$limit: 1},
        {$lookup: {let : {var1: "$a"}, pipeline: [{$sort: {a: 1}}], from: "foreign", as: "same"}},
        {$unwind: "$same"}
    ],
    {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.usedDisk, true, tojson(profileObj));

//
// Test that usedDisk is not set for a $lookup with a pipeline that does not use disk.
//
assert.commandWorked(testDB.adminCommand(
    {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 100 * 1024 * 1024}));
resetCollection();
resetForeignCollection();
coll.aggregate(
    [{$lookup: {let : {var1: "$a"}, pipeline: [{$sort: {a: 1}}], from: "otherTest", as: "same"}}],
    {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert(!profileObj.hasOwnProperty("usedDisk"), tojson(profileObj));

assert.commandWorked(testDB.adminCommand({
    setParameter: 1,
    internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: 1
}));
assert.commandWorked(testDB.adminCommand(
    {setParameter: 1, internalQuerySlotBasedExecutionHashAggIncreasedSpilling: "never"}));

function checkHashLookup(pipeline) {
    // HashLookup spills only in SBE
    const explain = coll.explain().aggregate(pipeline);
    if (getAggPlanStages(explain, "EQ_LOOKUP_UNWIND").length > 0 ||
        getAggPlanStages(explain, "EQ_LOOKUP").length > 0) {
        coll.aggregate(pipeline, {allowDiskUse: true});
        const profileObj = getLatestProfilerEntry(testDB);
        assert.eq(profileObj.usedDisk, true, tojson(profileObj));
        assert.gt(profileObj.hashLookupSpills, 0, tojson(profileObj));
        assert.gt(profileObj.hashLookupSpilledBytes, 0, tojson(profileObj));
        assert.gt(profileObj.hashLookupSpilledRecords, 0, tojson(profileObj));
        assert.gt(profileObj.hashLookupSpilledDataStorageSize, 0, tojson(profileObj));
    }
}

const lookupPipeline =
    [{$lookup: {from: "foreign", localField: "a", foreignField: "b", as: "same"}}];
checkHashLookup(lookupPipeline);

const lookupUnwindPipeline = [
    {$lookup: {from: "foreign", localField: "a", foreignField: "b", as: "same"}},
    {$unwind: "$same"},
    {$project: {same: 1}}
];
checkHashLookup(lookupUnwindPipeline);

//
// Test that aggregate command fails when 'allowDiskUse:false' because of insufficient available
// memory to perform group.
//
assert.throws(() => coll.aggregate(
                  [{$unionWith: {coll: "foreign", pipeline: [{$group: {"_id": {$avg: "$b"}}}]}}],
                  {allowDiskUse: false}));

//
// Test that the above command succeeds with 'allowDiskUse:true'. 'usedDisk' is correctly detected
// when a sub-pipeline of $unionWith stage uses disk.
//
resetCollection();
resetForeignCollection();
coll.aggregate([{$unionWith: {coll: "foreign", pipeline: [{$group: {"_id": {$avg: "$b"}}}]}}],
               {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert.eq(profileObj.usedDisk, true, tojson(profileObj));

//
// Test that usedDisk is not set for a $unionWith with a sub-pipeline that does not use disk.
//
coll.aggregate([{$unionWith: {coll: "foreign", pipeline: [{$sort: {b: 1}}]}}],
               {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert(!profileObj.usedDisk, tojson(profileObj));

coll.aggregate([{$unionWith: {coll: "foreign", pipeline: [{$match: {a: 1}}]}}],
               {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert(!profileObj.usedDisk, tojson(profileObj));

MongoRunner.stopMongod(conn);

//
// Tests on a sharded cluster.
//
const st = new ShardingTest({shards: 2});
const shardedDB = st.s.getDB(jsTestName());

assert.commandWorked(
    st.s0.adminCommand({enableSharding: shardedDB.getName(), primaryShard: st.shard0.shardName}));

const shardedSourceColl = shardedDB.coll1;
const shardedForeignColl = shardedDB.coll2;

const shard0DB = st.shard0.getDB(jsTestName());
const shard1DB = st.shard1.getDB(jsTestName());

// Shard 'shardedSourceColl' and 'shardedForeignColl' on {x:1}, split it at {x:0}, and move
// chunk {x:0} to shard1.
st.shardColl(shardedSourceColl, {x: 1}, {x: 0}, {x: 0});
st.shardColl(shardedForeignColl, {x: 1}, {x: 0}, {x: 0});

// Insert few documents on each shard.
for (let i = 0; i < 10; ++i) {
    assert.commandWorked(shardedSourceColl.insert({x: i}));
    assert.commandWorked(shardedSourceColl.insert({x: -i}));
    assert.commandWorked(shardedForeignColl.insert({x: i}));
    assert.commandWorked(shardedForeignColl.insert({x: -i}));
    assert.commandWorked(shardedDB.unshardedColl.insert({x: i}));
}

// Restart profiler.
function restartProfiler() {
    for (let shardDB of [shard0DB, shard1DB]) {
        shardDB.setProfilingLevel(0);
        shardDB.system.profile.drop();

        // Enable profiling and changes the 'slowms' threshold to -1ms. This will log all the
        // commands.
        shardDB.setProfilingLevel(2, -1);
    }
}

assert.commandWorked(
    shard0DB.adminCommand({setParameter: 1, internalDocumentSourceGroupMaxMemoryBytes: 10}));
restartProfiler();
// Test that 'usedDisk' doesn't get populated on the profiler entry of the base pipeline, when the
// $unionWith'd pipeline needs to use disk on a sharded collection.
assert.commandWorked(shardedDB.runCommand({
    aggregate: shardedSourceColl.getName(),
    pipeline: [{
        $unionWith:
            {coll: shardedForeignColl.getName(), pipeline: [{$group: {"_id": {$avg: "$x"}}}]}
    }],
    cursor: {},
    allowDiskUse: true,
}));
// Verify that the $unionWith'd pipeline always has the profiler entry.
profilerHasSingleMatchingEntryOrThrow({
    profileDB: shard0DB,
    filter:
        {'command.getMore': {$exists: true}, usedDisk: true, ns: shardedForeignColl.getFullName()}
});

// If the $mergeCursor is ran on the shard0DB, then the profiler entry should have the 'usedDisk'
// set.
if (shard0DB.system.profile
        .find({
            ns: shardedSourceColl.getFullName(),
            'command.pipeline.$mergeCursors': {$exists: true}
        })
        .itcount() > 0) {
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: shard0DB,
        filter: {
            'command.pipeline.$mergeCursors': {$exists: true},
            usedDisk: true,
            ns: shardedSourceColl.getFullName()
        }
    });
    profilerHasZeroMatchingEntriesOrThrow(
        {profileDB: shard1DB, filter: {usedDisk: true, ns: shardedSourceColl.getFullName()}});
} else {
    // If the $mergeCursors is ran on mongos or shard1DB, then the profiler shouldn't have the
    // 'usedDisk' set.
    profilerHasZeroMatchingEntriesOrThrow(
        {profileDB: shard0DB, filter: {usedDisk: true, ns: shardedSourceColl.getFullName()}});
    profilerHasZeroMatchingEntriesOrThrow(
        {profileDB: shard1DB, filter: {usedDisk: true, ns: shardedSourceColl.getFullName()}});
}

// Verify that the 'usedDisk' is always set correctly on base pipeline.
restartProfiler();
assert.commandWorked(shardedDB.runCommand({
    aggregate: shardedSourceColl.getName(),
    pipeline: [
        {$group: {"_id": {$avg: "$x"}}},
        {$unionWith: {coll: shardedForeignColl.getName(), pipeline: []}}
    ],
    cursor: {},
    allowDiskUse: true,
}));
profilerHasSingleMatchingEntryOrThrow({
    profileDB: shard0DB,
    filter:
        {'command.getMore': {$exists: true}, usedDisk: true, ns: shardedSourceColl.getFullName()}
});
profilerHasZeroMatchingEntriesOrThrow(
    {profileDB: shard0DB, filter: {usedDisk: true, ns: shardedForeignColl.getFullName()}});

// Set the 'internalDocumentSourceGroupMaxMemoryBytes' to a higher value so that st.stop()
// doesn't fail.
assert.commandWorked(shard0DB.adminCommand(
    {setParameter: 1, internalDocumentSourceGroupMaxMemoryBytes: 100 * 1024 * 1024}));

st.stop();
