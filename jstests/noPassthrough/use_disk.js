// @tags: [does_not_support_stepdowns, requires_profiling]

// Confirms that profiled aggregation execution contains expected values for usedDisk.

(function() {
"use strict";

// For getLatestProfilerEntry and getProfilerProtocolStringForCommand
load("jstests/libs/profiler.js");
const conn = MongoRunner.runMongod({setParameter: "maxBSONDepth=8"});
const testDB = conn.getDB("profile_agg");
const coll = testDB.getCollection("test");

testDB.setProfilingLevel(2);

function resetCollection() {
    coll.drop();
    for (var i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }
}
function resetForeignCollection() {
    testDB.foreign.drop();
    const forColl = testDB.getCollection("foreign");
    for (var i = 4; i < 18; i += 2)
        assert.writeOK(forColl.insert({b: i}));
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
    testDB.adminCommand({setParameter: 1, internalQueryExecMaxBlockingSortBytes: 10}));
assert.eq(
    8, coll.aggregate([{$match: {a: {$gte: 2}}}, {$sort: {a: 1}}], {allowDiskUse: true}).itcount());
profileObj = getLatestProfilerEntry(testDB);

assert.eq(profileObj.usedDisk, true, tojson(profileObj));
assert.eq(profileObj.hasSortStage, true, tojson(profileObj));

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
    {setParameter: 1, internalQueryExecMaxBlockingSortBytes: 100 * 1024 * 1024}));
resetCollection();
resetForeignCollection();
coll.aggregate(
    [{$lookup: {let : {var1: "$a"}, pipeline: [{$sort: {a: 1}}], from: "otherTest", as: "same"}}],
    {allowDiskUse: true});
profileObj = getLatestProfilerEntry(testDB);
assert(!profileObj.hasOwnProperty("usedDisk"), tojson(profileObj));
MongoRunner.stopMongod(conn);
})();
