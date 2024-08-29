// Checks that histogram counters for collections are updated as we expect.
//
// This test attempts to perform write operations and get latency statistics using the $collStats
// stage. The former operation must be routed to the primary in a replica set, whereas the latter
// may be routed to a secondary. This is incompatible with embedded right now since the command
// compact does not exist on such storage engines.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: compact, dataSize,reIndex,
//   # whatsmyuri.
//   not_allowed_with_signed_security_token,
//   assumes_read_preference_unchanged,
//   does_not_support_repeated_reads,
//   requires_collstats,
//   # Tenant migrations passthrough suites automatically retry operations on TenantMigrationAborted
//   # errors.
//   tenant_migration_incompatible,
//   # Some passthroughs which implicitly create indexes (e.g. the column store index passthrough)
//   # will override the 'getIndexes()' helper to hide the implicitly created index. This override
//   # messes with the latency stats tracking and counts the operation as an aggregate instead of a
//   # command. It's an implementation detail that leaks and invalidates the test.
//   assumes_no_implicit_index_creation,
//   uses_compact,
//   # Does not support multiplanning, because it stashes documents beyond batch size.
//   does_not_support_multiplanning_single_solutions,
// ]
//

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {assertHistogramDiffEq, getHistogramStats} from "jstests/libs/stats.js";

const dbName = "operationalLatencyHistogramTest";
// Skipping the collection from dbcheck during the test.
const collName = dbName + "_coll_temp";
const afterTestCollName = dbName + "_coll";

var testDB = db.getSiblingDB(dbName);
var testColl = testDB[collName];

testColl.drop();

// Running a $collStats aggregation on a non-existent database will error on mongos but return
// bassic information on monogod.
if (FixtureHelpers.isMongos(db) || TestData.testingReplicaSetEndpoint) {
    assert.commandWorked(testDB.createCollection(collName));
}

// Test aggregation command output format.
var commandResult = testDB.runCommand(
    {aggregate: testColl.getName(), pipeline: [{$collStats: {latencyStats: {}}}], cursor: {}});
assert.commandWorked(commandResult);
assert(commandResult.cursor.firstBatch.length == 1);

var stats = commandResult.cursor.firstBatch[0];
var histogramTypes = ["reads", "writes", "commands"];

assert(stats.hasOwnProperty("localTime"));
assert(stats.hasOwnProperty("latencyStats"));

histogramTypes.forEach(function(key) {
    assert(stats.latencyStats.hasOwnProperty(key));
    assert(stats.latencyStats[key].hasOwnProperty("ops"));
    assert(stats.latencyStats[key].hasOwnProperty("latency"));
});

var lastHistogram = getHistogramStats(testColl);

// Insert
var numRecords = 100;
for (var i = 0; i < numRecords; i++) {
    assert.commandWorked(testColl.insert({_id: i}));
}
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, numRecords, 0);

// Update
for (var i = 0; i < numRecords; i++) {
    assert.commandWorked(testColl.update({_id: i}, {x: i}));
}
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, numRecords, 0);

// Find
var cursors = [];
for (var i = 0; i < numRecords; i++) {
    cursors[i] = testColl.find({x: {$gte: i}}).batchSize(2);
    assert.eq(cursors[i].next()._id, i);
}
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, numRecords, 0, 0);

// GetMore
for (var i = 0; i < numRecords / 2; i++) {
    // Trigger two getmore commands.
    assert.eq(cursors[i].next()._id, i + 1);
    assert.eq(cursors[i].next()._id, i + 2);
    assert.eq(cursors[i].next()._id, i + 3);
    assert.eq(cursors[i].next()._id, i + 4);
}
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, numRecords, 0, 0);

// KillCursors
// The last cursor has no additional results, hence does not need to be closed.
for (var i = 0; i < numRecords - 1; i++) {
    cursors[i].close();
}
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, numRecords - 1);

// Remove
for (var i = 0; i < numRecords; i++) {
    assert.commandWorked(testColl.remove({_id: i}));
}
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, numRecords, 0);

// Upsert
for (var i = 0; i < numRecords; i++) {
    assert.commandWorked(testColl.update({_id: i}, {x: i}, {upsert: 1}));
}
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, numRecords, 0);

// Aggregate
for (var i = 0; i < numRecords; i++) {
    testColl.aggregate([{$match: {x: i}}, {$group: {_id: "$x"}}]);
}
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, numRecords, 0, 0);

// Count
for (var i = 0; i < numRecords; i++) {
    testColl.count({x: i});
}
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, numRecords, 0, 0);

// FindAndModify
testColl.findAndModify({query: {}, update: {pt: {type: "Point", coordinates: [0, 0]}}});
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 1, 0);

// CreateIndex
assert.commandWorked(testColl.createIndex({pt: "2dsphere"}));
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 1);

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
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 1, 0, 0);

// GetIndexes
testColl.getIndexes();
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 1);

// Reindex (Only standalone mode supports the reIndex command.)
if (FixtureHelpers.isStandalone(db)) {
    assert.commandWorked(testColl.reIndex());
    lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 1);
}

// DropIndex
assert.commandWorked(testColl.dropIndex({pt: "2dsphere"}));
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 1);

// Explain
testColl.explain().find().next();
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 1);

// CollStats
assert.commandWorked(testDB.runCommand({collStats: testColl.getName()}));
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 1);

// CollMod
assert.commandWorked(testDB.runCommand({collStats: testColl.getName(), validationLevel: "off"}));
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 1);

// Compact
// Use force:true in case we're in replset.
var commandResult = testDB.runCommand({compact: testColl.getName(), force: true});
// The storage engine may not support compact or if it does, it can be interrupted because of cache
// pressure or concurrent calls to compact.
if (!commandResult.ok) {
    assert.commandFailedWithCode(commandResult,
                                 [ErrorCodes.CommandNotSupported, ErrorCodes.Interrupted],
                                 tojson(commandResult));
}
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 1);

// DataSize
testColl.dataSize();
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 1);

// PlanCache
testColl.getPlanCache().clear();
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 1);

// Commands which occur on the database only should not effect the collection stats.
assert.commandWorked(testDB.serverStatus());
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 0);

assert.commandWorked(testColl.runCommand("whatsmyuri"));
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 0);

// Test non-command.
assert.commandFailed(testColl.runCommand("IHopeNobodyEverMakesThisACommand"));
lastHistogram = assertHistogramDiffEq(testDB, testColl, lastHistogram, 0, 0, 0);

// Rename the collection to enable it for dbcheck after the test.
assert.commandWorked(testColl.renameCollection(afterTestCollName, true /* dropTarget */));
