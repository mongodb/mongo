/*
 * Tests that $out cleans up the buckets collections if interrupted after the rename, but before the
 * view is created.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_persistence,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const st = new ShardingTest({shards: 2});

const dbName = jsTestName();
const outCollName = 'out';
const sourceCollName = 'foo';
const outBucketsCollName = 'system.buckets.out';
const testDB = st.s.getDB(dbName);
const sourceDocument = {
    x: 1,
    t: ISODate()
};
const primary = st.shard0.shardName;
const other = st.shard1.shardName;
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: primary}));

function listCollections(collName) {
    return testDB.getCollectionNames().filter(coll => coll === collName);
}

function resetCollections() {
    if (testDB[sourceCollName]) {
        assert(testDB[sourceCollName].drop());
    }
    if (testDB[outCollName]) {
        assert(testDB[outCollName].drop());
    }
}

function killOp() {
    const adminDB = st.s.getDB("admin");
    const curOps = adminDB
                       .aggregate([
                           {$currentOp: {allUsers: true}},
                           {
                               $match: {
                                   "command.comment": "testComment",
                                   // The create coordinator issues fire and forget refreshes after
                                   // creating a collection. We filter these out to ensure we are
                                   // killing the correct operation.
                                   "command._flushRoutingTableCacheUpdates": {$exists: false}
                               }
                           }
                       ])
                       .toArray();
    assert.eq(1, curOps.length, curOps);
    assert.commandWorked(adminDB.killOp(curOps[0].opid));
}

function runOut(dbName, sourceCollName, targetCollName) {
    const cmdRes = db.getSiblingDB(dbName).runCommand({
        aggregate: sourceCollName,
        pipeline: [{$out: {db: dbName, coll: targetCollName, timeseries: {timeField: 't'}}}],
        cursor: {},
        comment: "testComment",
    });
    assert.commandFailed(cmdRes);
}

function runOutAndInterrupt(mergeShard) {
    const fp = configureFailPoint(mergeShard.rs.getPrimary(),
                                  'outWaitAfterTempCollectionRenameBeforeView',
                                  {shouldCheckForInterrupt: true});

    let outShell =
        startParallelShell(funWithArgs(runOut, dbName, sourceCollName, outCollName), st.s.port);

    fp.wait();

    // Verify that the buckets collection is created.
    let bucketCollections = listCollections(outBucketsCollName);
    assert.eq(1, bucketCollections.length, bucketCollections);

    // Provoke failure.
    killOp();
    outShell();

    // Assert that the temporary collections has been garbage collected.
    const tempCollections =
        testDB.getCollectionNames().filter(coll => coll.includes('tmp.agg_out'));
    const garbageCollectionEntries = st.s.getDB('config')['agg_temp_collections'].count();
    assert(tempCollections.length === 0 && garbageCollectionEntries === 0);
}

// Validates $out should clean up the buckets collection if the command is interrupted before the
// view is created. We will test if the source collection exists on the primary shard or
// any other shard.
function testCreatingNewCollection(sourceShard) {
    resetCollections();

    // Create source collection and move to the correct shard.
    assert.commandWorked(testDB.runCommand({create: sourceCollName}));
    assert.commandWorked(testDB[sourceCollName].insert(sourceDocument));
    assert.commandWorked(
        st.s.adminCommand({moveCollection: dbName + '.' + sourceCollName, toShard: sourceShard}));

    let bucketCollections = listCollections(outBucketsCollName);
    assert.eq(0, bucketCollections, bucketCollections);

    // The output collection is a time-series collection and needs a view so $out will run on the
    // primary.
    runOutAndInterrupt(st.shard0);

    // There should neither be a buckets collection nor a view.
    bucketCollections = listCollections(outBucketsCollName);
    assert.eq(0, bucketCollections.length, bucketCollections);
    let view = listCollections(outCollName);
    assert.eq(0, view.length, view);
}

testCreatingNewCollection(primary);
testCreatingNewCollection(other);

// Validates $out should not clean up the buckets collection if the command is interrupted when the
// view exists. The source and output collections will be on the same shard.
function testReplacingExistingCollectionOnSameShard(shard) {
    resetCollections();

    // Create source collection and move to the correct shard.
    assert.commandWorked(testDB.runCommand({create: sourceCollName}));
    assert.commandWorked(testDB[sourceCollName].insert(sourceDocument));
    assert.commandWorked(
        st.s.adminCommand({moveCollection: dbName + '.' + sourceCollName, toShard: shard}));

    // Create the time-series collection $out will replace. The buckets collection will be on the
    // same shard, but the view will always exist on the primary shard.
    assert.commandWorked(testDB.runCommand({create: outCollName, timeseries: {timeField: "t"}}));
    assert.commandWorked(testDB[outCollName].insert({a: 1, t: ISODate()}));
    assert.commandWorked(
        st.s.adminCommand({moveCollection: dbName + '.' + outCollName, toShard: shard}));

    let bucketCollections = listCollections(outBucketsCollName);
    assert.eq(1, bucketCollections.length, bucketCollections);

    // The output collection is a time-series collection and needs a view so $out will run on the
    // primary.
    runOutAndInterrupt(st.shard0);

    // There should be a buckets collection and a view.
    bucketCollections = listCollections(outBucketsCollName);
    assert.eq(1, bucketCollections.length, bucketCollections);
    let view = listCollections(outCollName);
    assert.eq(1, view.length, view);

    // $out should replace the document inside the collection.
    assert.sameMembers(testDB[outCollName].find({}, {_id: 0}).toArray(), [sourceDocument]);
}

if (FeatureFlagUtil.isPresentAndEnabled(testDB, 'ReshardingForTimeseries')) {
    testReplacingExistingCollectionOnSameShard(primary);
    testReplacingExistingCollectionOnSameShard(other);
}

// TODO SERVER-90720 Add test for running $out on collections on different shards.

st.stop();
