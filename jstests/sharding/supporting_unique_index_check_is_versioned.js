// Tests that $merge's enforcement of a unique index on mongos includes a shard and/or database
// version.
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {profilerHasAtLeastOneMatchingEntryOrThrow} from "jstests/libs/profiler.js";

function prepareProfilerOnShards(st, dbName) {
    st._rs.forEach(rs => {
        const shardDB = rs.test.getPrimary().getDB(dbName);
        shardDB.system.profile.drop();
        assert.commandWorked(shardDB.setProfilingLevel(2));
    });
}

function verifyProfilerListIndexesEntry(
    {profileDB, collName, expectShardVersion, expectDbVersion}) {
    profilerHasAtLeastOneMatchingEntryOrThrow({
        profileDB: profileDB,
        filter: {
            "command.listIndexes": collName,
            "command.shardVersion": {$exists: expectShardVersion},
            "command.databaseVersion": {$exists: expectDbVersion}
        }
    });
}

// Creates the source collection and target collection as unsharded collections on shard0.
function setUpUnshardedSourceAndTargetCollections(st, dbName, sourceCollName, targetCollName) {
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(st.s.getDB(dbName).createCollection(sourceCollName));
    assert.commandWorked(st.s.getDB(dbName).createCollection(targetCollName));

    assert.commandWorked(st.s.getDB(dbName)[sourceCollName].insert({a: 10, b: 11}));
    assert.commandWorked(st.s.getDB(dbName)[targetCollName].insert({a: 10, b: 12}));
}

// Creates the source collection as an unsharded collection on shard0 and the target collection as a
// sharded collection with one chunk on shard0.
function setUpUnshardedSourceShardedTargetCollections(st, dbName, sourceCollName, targetCollName) {
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    assert.commandWorked(st.s.getDB(dbName)[sourceCollName].insert({a: 10, b: 11}));

    const targetColl = st.s.getDB(dbName)[targetCollName];
    assert.commandWorked(
        st.s.adminCommand({shardCollection: targetColl.getFullName(), key: {a: 1}}));
    assert.commandWorked(targetColl.insert({a: 10, b: 12}));
}

function expectMergeToSucceed(dbName, sourceCollName, targetCollName, onFields) {
    assert.commandWorked(st.s.getDB(dbName).runCommand({
        aggregate: sourceCollName,
        pipeline: [{
            $merge: {
                into: {db: dbName, coll: targetCollName},
                whenMatched: "replace",
                whenNotMatched: "insert",
                on: Object.keys(onFields)
            }
        }],
        cursor: {}
    }));
}

function expectMergeToFailBecauseOfMissingIndex(dbName, sourceCollName, targetCollName, onFields) {
    assert.commandFailedWithCode(st.s.getDB(dbName).runCommand({
        aggregate: sourceCollName,
        pipeline: [{
            $merge: {
                into: {db: dbName, coll: targetCollName},
                whenMatched: "replace",
                whenNotMatched: "insert",
                on: Object.keys(onFields)
            }
        }],
        cursor: {}
    }),
                                 51190);
}

const st = new ShardingTest({shards: 2, rs: {nodes: 1}, mongos: 2});
const sourceCollName = "sourceFoo";
const targetCollName = "targetFoo";

const isTrackUnshardedEnabled = FeatureFlagUtil.isPresentAndEnabled(
    st.shard0.getDB('admin'), "TrackUnshardedCollectionsUponCreation");

//
// Verify database versions are used to detect when the primary shard changes for an unsharded
// target collection.
//

(() => {
    // Database versioning tests only make sense when all collections are not tracked.
    if (isTrackUnshardedEnabled) {
        return;
    }
    const dbName = "testMovedPrimarySuccess";
    jsTestLog("Running test on database: " + dbName);

    setUpUnshardedSourceAndTargetCollections(st, dbName, sourceCollName, targetCollName);

    // Move the primary from shard0 to shard1 and create an index required for the merge only on the
    // new primary. Because of database versioning (or shard versioning, if the collection is
    // tracked), the stale router should discover the primary has changed when checking indexes for
    // the merge and load them from the new primary.
    const otherRouter = st.s1;
    assert.commandWorked(otherRouter.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

    assert.commandWorked(
        otherRouter.getDB(dbName)[targetCollName].createIndex({a: 1, b: 1}, {unique: true}));

    // Run $merge and expect it to succeed because the stale router refreshes and is able to find
    // the correct indexes. Enable the profiler to verify shard/db versions later.
    prepareProfilerOnShards(st, dbName);
    expectMergeToSucceed(dbName, sourceCollName, targetCollName, {a: 1, b: 1});

    // Verify the aggregation succeeded on the expected shard and included the expected shard/db
    // versions.
    verifyProfilerListIndexesEntry({
        profileDB: st.rs1.getPrimary().getDB(dbName),
        collName: targetCollName,
        expectShardVersion: true,
        expectDbVersion: true
    });
})();

(() => {
    // Database versioning tests only make sense when all collections are not tracked.
    if (isTrackUnshardedEnabled) {
        return;
    }
    const dbName = "testMovedPrimaryFailure";
    jsTestLog("Running test on database: " + dbName);

    setUpUnshardedSourceAndTargetCollections(st, dbName, sourceCollName, targetCollName);

    // Create the index necessary for the merge below.
    assert.commandWorked(
        st.s.getDB(dbName)[targetCollName].createIndex({a: 1, b: 1}, {unique: true}));

    // Move the primary from shard0 to shard1 and drop the index required for the merge only on the
    // new primary. Note that the collection will be dropped from the old primary when the
    // movePrimary completes, so this case would pass without versioning, but is included for
    // completeness.
    const otherRouter = st.s1;
    assert.commandWorked(otherRouter.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

    const targetColl = otherRouter.getDB(dbName)[targetCollName];
    assert.commandWorked(targetColl.dropIndex("a_1_b_1"));

    // Run $merge and expect it to fail because the router refreshes and discovers the required
    // index no longer exists. Enable the profiler to verify shard/db versions later.
    prepareProfilerOnShards(st, dbName);
    expectMergeToFailBecauseOfMissingIndex(dbName, sourceCollName, targetCollName, {a: 1, b: 1});

    // Verify the aggregation succeeded on the expected shard and included the expected shard/db
    // versions.
    verifyProfilerListIndexesEntry({
        profileDB: st.rs1.getPrimary().getDB(dbName),
        collName: targetCollName,
        expectShardVersion: true,
        expectDbVersion: true
    });
})();

//
// Verify shard versions are used to detect when the shards that own chunks for a sharded target
// collection changes.
//

(() => {
    const dbName = "testMovedChunkSuccess";
    jsTestLog("Running test on database: " + dbName);

    setUpUnshardedSourceShardedTargetCollections(st, dbName, sourceCollName, targetCollName);

    // Move the only chunk for the test collection from shard0 to shard1 and create an index
    // required for the merge. Indexes are only created on shards that own chunks, so the index
    // will only exist on shard1.
    const otherRouter = st.s1;
    const targetColl = otherRouter.getDB(dbName)[targetCollName];
    assert.commandWorked(otherRouter.adminCommand(
        {moveChunk: targetColl.getFullName(), find: {a: 0}, to: st.shard1.shardName}));
    assert.commandWorked(targetColl.createIndex({a: 1, b: 1}, {unique: true}));

    // Run $merge and expect it to succeed because the stale router refreshes and is able to find
    // the correct indexes. Enable the profiler to verify shard/db versions later.
    prepareProfilerOnShards(st, dbName);
    expectMergeToSucceed(dbName, sourceCollName, targetCollName, {a: 1, b: 1});

    // Verify the aggregation succeeded on the expected shard and included the expected shard/db
    // versions.
    verifyProfilerListIndexesEntry({
        profileDB: st.rs1.getPrimary().getDB(dbName),
        collName: targetCollName,
        expectShardVersion: true,
        expectDbVersion: false
    });
})();

(() => {
    const dbName = "testMovedChunkFailure";
    jsTestLog("Running test on database: " + dbName);

    setUpUnshardedSourceShardedTargetCollections(st, dbName, sourceCollName, targetCollName);

    // Create the index necessary for the merge below.
    assert.commandWorked(
        st.s.getDB(dbName)[targetCollName].createIndex({a: 1, b: 1}, {unique: true}));

    // Move the only chunk for the test collection from shard0 to shard1 and drop the index required
    // for the merge. dropIndexes will only target shards that own chunks, so the index will still
    // exist on shard0.
    const otherRouter = st.s1;
    const targetColl = otherRouter.getDB(dbName)[targetCollName];
    assert.commandWorked(otherRouter.adminCommand(
        {moveChunk: targetColl.getFullName(), find: {a: 0}, to: st.shard1.shardName}));
    assert.commandWorked(targetColl.dropIndex("a_1_b_1"));

    // Run $merge and expect it to fail because the router refreshes and discovers the required
    // index no longer exists. Enable the profiler to verify shard/db versions later.
    prepareProfilerOnShards(st, dbName);
    expectMergeToFailBecauseOfMissingIndex(dbName, sourceCollName, targetCollName, {a: 1, b: 1});

    // Verify the aggregation succeeded on the expected shard and included the expected shard/db
    // versions.
    verifyProfilerListIndexesEntry({
        profileDB: st.rs1.getPrimary().getDB(dbName),
        collName: targetCollName,
        expectShardVersion: true,
        expectDbVersion: false
    });
})();

//
// Verify shard versions are used to detect when an unsharded collection becomes sharded or vice
// versa.
//

(() => {
    const dbName = "testBecomeShardedSuccess";
    jsTestLog("Running test on database: " + dbName);

    setUpUnshardedSourceAndTargetCollections(st, dbName, sourceCollName, targetCollName);

    // Shard the target collection through a different router, move its only chunk to shard1, and
    // create a new index required for the merge on only the shard with the chunk.
    const otherRouter = st.s1;
    assert.commandWorked(otherRouter.adminCommand({enableSharding: dbName}));

    const targetColl = otherRouter.getDB(dbName)[targetCollName];
    assert.commandWorked(targetColl.createIndex({a: 1}));
    assert.commandWorked(
        st.s1.adminCommand({shardCollection: targetColl.getFullName(), key: {a: 1}}));

    assert.commandWorked(otherRouter.adminCommand(
        {moveChunk: targetColl.getFullName(), find: {a: 0}, to: st.shard1.shardName}));
    assert.commandWorked(targetColl.createIndex({a: 1, b: 1}, {unique: true}));

    // Run $merge and expect it to succeed because the stale router refreshes and is able to find
    // the correct indexes. Enable the profiler to verify shard/db versions later.
    prepareProfilerOnShards(st, dbName);
    expectMergeToSucceed(dbName, sourceCollName, targetCollName, {a: 1, b: 1});

    // Verify the aggregation succeeded on the expected shard and included the expected shard/db
    // versions.
    verifyProfilerListIndexesEntry({
        profileDB: st.rs1.getPrimary().getDB(dbName),
        collName: targetCollName,
        expectShardVersion: true,
        expectDbVersion: false
    });
})();

(() => {
    // Database versioning tests only make sense when all collections are not tracked.
    if (isTrackUnshardedEnabled) {
        return;
    }
    const dbName = "testBecomeUnshardedFailure";
    jsTestLog("Running test on database: " + dbName);

    setUpUnshardedSourceAndTargetCollections(st, dbName, sourceCollName, targetCollName);

    // Create the index necessary for the merge below.
    assert.commandWorked(
        st.s.getDB(dbName)[targetCollName].createIndex({a: 1, b: 1}, {unique: true}));

    // Drop and recreate the sharded target collection as an unsharded collection with its primary
    // on shard1. Dropping the collection will also drop the index required for the merge.
    const otherRouter = st.s1;
    const targetColl = otherRouter.getDB(dbName)[targetCollName];
    assert(targetColl.drop());

    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard1.name}));
    assert.commandWorked(targetColl.insert({a: 10, b: 12}));

    // Run $merge and expect it to fail because the router refreshes and discovers the required
    // index no longer exists. Enable the profiler to verify shard/db versions later.
    prepareProfilerOnShards(st, dbName);
    expectMergeToFailBecauseOfMissingIndex(dbName, sourceCollName, targetCollName, {a: 1, b: 1});

    // Verify the aggregation succeeded on the expected shard and included the expected shard/db
    // versions.
    verifyProfilerListIndexesEntry({
        profileDB: st.rs1.getPrimary().getDB(dbName),
        collName: targetCollName,
        expectShardVersion: true,
        expectDbVersion: true
    });
})();

st.stop();
