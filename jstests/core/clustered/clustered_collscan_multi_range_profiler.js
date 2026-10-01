/**
 * Tests that the profiler correctly reports stats for multi-range clustered collection scans.
 *
 * @tags: [
 *   featureFlagClusteredCollScanMultiRange,
 *   requires_profiling,
 *   queries_system_profile_collection,
 *   assumes_against_mongod_not_mongos,
 *   assumes_unsharded_collection,
 *   # Cannot enable the profiler in stepdown overrides
 *   does_not_support_stepdowns,
 * ]
 */

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.clustered_collscan_multi_range_profiler;

coll.drop();
assert.commandWorked(
    testDB.createCollection(coll.getName(), {clusteredIndex: {unique: true, key: {_id: 1}}}),
);

assert.commandWorked(coll.insertMany([{_id: 0}, {_id: 1}, {_id: 2}]));

// Increase the profiler lock deadline to prevent flakiness in FCV upgrade/downgrade
// and sanitizer variants where the system may be under heavy load.
assert.commandWorked(
    testDB
        .getSiblingDB("admin")
        .runCommand({setParameter: 1, internalQueryGlobalProfilingLockDeadlineMs: 1000}),
);

// Enable profiling.
assert.commandWorked(testDB.setProfilingLevel(2, {slowms: -1}));

try {
    // Run a query that produces a multi-range clustered collscan.
    const filter = {_id: {$in: [0, 2]}};
    assert.eq(coll.find(filter).toArray().length, 2);
} finally {
    // Disable profiling before reading the profile collection.
    assert.commandWorked(testDB.setProfilingLevel(0));
}

const profileEntry = testDB.system.profile.findOne({"command.find": coll.getName()});
assert(profileEntry, "No profile entry found for the query");

const isSbe = profileEntry.queryFramework === "sbe";

// Root-level stats.
assert.eq(profileEntry.keysExamined, 0, {profileEntry});
// Classic examines an extra document due to the cursor 'next' beyond the last range;
// SBE does not.
assert.eq(profileEntry.docsExamined, isSbe ? 2 : 3, {profileEntry});
assert.eq(profileEntry.nreturned, 2, {profileEntry});
assert.eq(profileEntry.planSummary, "CLUSTERED_IXSCAN", {profileEntry});

// execStats.
const execStats = profileEntry.execStats;
assert(execStats, "Expected execStats in profile entry", {profileEntry});
assert.eq(execStats.nReturned, 2, {execStats});
assert.eq(execStats.isEOF, 1, {execStats});

if (isSbe) {
    // SBE reports a "scan" stage with different fields.
    assert.eq(execStats.stage, "scan", {execStats});
    assert.eq(execStats.numReads, 2, {execStats});
} else {
    // Classic reports a "CLUSTERED_IXSCAN" stage.
    assert.eq(execStats.stage, "CLUSTERED_IXSCAN", {execStats});
    assert.eq(execStats.advanced, 2, {execStats});
    assert.eq(execStats.nss, testDB.getName() + "." + coll.getName(), {execStats});
    assert.eq(execStats.direction, "forward", {execStats});
    assert.eq(execStats.minRecord, 0, {execStats});
    assert.eq(execStats.maxRecord, 2, {execStats});
    assert.eq(execStats.docsExamined, 3, {execStats});
    assert.eq(execStats.seeks, 2, {execStats});
    assert.eq(
        execStats.recordIdRanges,
        [
            {min: 0, minInclusive: true, max: 0, maxInclusive: true},
            {min: 2, minInclusive: true, max: 2, maxInclusive: true},
        ],
        {execStats},
    );
}
