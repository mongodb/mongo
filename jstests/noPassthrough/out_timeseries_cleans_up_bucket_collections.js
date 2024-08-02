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

const kDbName = jsTestName();
const kOutCollName = 'out';
const kSourceCollName = 'foo';
const kLookUpCollName = "lookup";
const kOutBucketsCollName = 'system.buckets.out';
const testDB = st.s.getDB(kDbName);
const kSourceDocument = {
    x: 1,
    t: ISODate()
};
const kOutPipeline = [{$out: {db: kDbName, coll: kOutCollName, timeseries: {timeField: 't'}}}];
const kPrimary = st.shard0.shardName;
const kOther = st.shard1.shardName;
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName, primaryShard: kPrimary}));

// TODO SERVER-93149: Remove 'reshardingForTimeseriesFeatureFlagEnabled' checks.
const reshardingForTimeseriesFeatureFlagEnabled =
    FeatureFlagUtil.isPresentAndEnabled(testDB, "FeatureFlagReshardingForTimeseries");

function listCollections(collName) {
    return testDB.getCollectionNames().filter(coll => coll === collName);
}

function dropCollections() {
    if (testDB[kSourceCollName]) {
        assert(testDB[kSourceCollName].drop());
    }
    if (testDB[kOutCollName]) {
        assert(testDB[kOutCollName].drop());
    }
}

function killOp(comment) {
    const adminDB = st.s.getDB("admin");
    const curOps = adminDB
                       .aggregate([
                           {$currentOp: {allUsers: true}},
                           {
                               $match: {
                                   "command.comment": comment,
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

function runOut(dbName, sourceCollName, pipeline, comment) {
    const cmdRes = db.getSiblingDB(dbName).runCommand({
        aggregate: sourceCollName,
        pipeline: pipeline,
        cursor: {},
        comment: comment,
    });
    assert.commandFailed(cmdRes);
}

function runOutAndInterrupt(mergeShard, pipeline = kOutPipeline, comment = "testComment") {
    const fp = configureFailPoint(mergeShard.rs.getPrimary(),
                                  'outWaitAfterTempCollectionRenameBeforeView',
                                  {shouldCheckForInterrupt: true});

    let outShell = startParallelShell(
        funWithArgs(runOut, kDbName, kSourceCollName, pipeline, comment), st.s.port);

    fp.wait();

    // Verify that the buckets collection is created.
    let bucketCollections = listCollections(kOutBucketsCollName);
    assert.eq(1, bucketCollections.length, bucketCollections);

    // Provoke failure.
    killOp(comment);
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
    dropCollections();

    // Create source collection and move to the correct shard.
    assert.commandWorked(testDB.runCommand({create: kSourceCollName}));
    assert.commandWorked(testDB[kSourceCollName].insert(kSourceDocument));
    assert.commandWorked(st.s.adminCommand(
        {moveCollection: testDB[kSourceCollName].getFullName(), toShard: sourceShard}));

    let bucketCollections = listCollections(kOutBucketsCollName);
    assert.eq(0, bucketCollections, bucketCollections);

    // The output collection is a time-series collection and needs a view so $out will run on the
    // primary.
    runOutAndInterrupt(st.shard0);

    // There should neither be a buckets collection nor a view.
    bucketCollections = listCollections(kOutBucketsCollName);
    assert.eq(0, bucketCollections.length, bucketCollections);
    let view = listCollections(kOutCollName);
    assert.eq(0, view.length, view);
}

testCreatingNewCollection(kPrimary);
testCreatingNewCollection(kOther);

// Validates $out should not clean up the buckets collection if the command is interrupted when the
// view exists. The source and output collections will be on the same shard.
function testReplacingExistingCollectionOnSameShard(shard) {
    dropCollections();

    // Create source collection and move to the correct shard.
    assert.commandWorked(testDB.runCommand({create: kSourceCollName}));
    assert.commandWorked(testDB[kSourceCollName].insert(kSourceDocument));
    assert.commandWorked(
        st.s.adminCommand({moveCollection: testDB[kSourceCollName].getFullName(), toShard: shard}));

    // Create the time-series collection $out will replace. The buckets collection will be on the
    // same shard, but the view will always exist on the primary shard.
    assert.commandWorked(testDB.runCommand({create: kOutCollName, timeseries: {timeField: "t"}}));
    assert.commandWorked(testDB[kOutCollName].insert({a: 1, t: ISODate()}));
    if (reshardingForTimeseriesFeatureFlagEnabled) {
        assert.commandWorked(st.s.adminCommand(
            {moveCollection: testDB[kOutCollName].getFullName(), toShard: shard}));
    }

    let bucketCollections = listCollections(kOutBucketsCollName);
    assert.eq(1, bucketCollections.length, bucketCollections);

    // The output collection is a time-series collection and needs a view so $out will run on the
    // primary.
    runOutAndInterrupt(st.shard0);

    // There should be a buckets collection and a view.
    bucketCollections = listCollections(kOutBucketsCollName);
    assert.eq(1, bucketCollections.length, bucketCollections);
    let view = listCollections(kOutCollName);
    assert.eq(1, view.length, view);

    // $out should replace the document inside the collection.
    assert.sameMembers(testDB[kOutCollName].find({}, {_id: 0}).toArray(), [kSourceDocument]);
}

testReplacingExistingCollectionOnSameShard(kPrimary);
if (reshardingForTimeseriesFeatureFlagEnabled) {
    testReplacingExistingCollectionOnSameShard(kOther);
}

// Validates $out should not clean up the buckets collection if the command is interrupted when the
// view exists. The source and output collections will be on different shards.
function testReplacingExistingCollectionOnDifferentShard() {
    dropCollections();

    // Create the source and foreign collection on the non-primary shard.
    assert.commandWorked(testDB.runCommand({create: kSourceCollName}));
    assert.commandWorked(testDB[kSourceCollName].insert(kSourceDocument));
    assert.commandWorked(st.s.adminCommand(
        {moveCollection: testDB[kSourceCollName].getFullName(), toShard: kOther}));

    assert.commandWorked(testDB.runCommand({create: kLookUpCollName}));
    assert.commandWorked(testDB[kLookUpCollName].insert([{x: 1, t: ISODate(), lookup: 2}]));
    assert.commandWorked(st.s.adminCommand(
        {moveCollection: testDB[kLookUpCollName].getFullName(), toShard: kOther}));

    // Create the time-series collection $out will replace. Both the buckets collection and the view
    // will be on the primary shard, since all views live on the primary shard. To ensure this
    // remains true in the future, we will move the time-series collection to the primary shard when
    // supported.
    assert.commandWorked(testDB.runCommand({create: kOutCollName, timeseries: {timeField: "t"}}));
    assert.commandWorked(testDB[kOutCollName].insert({a: 1, t: ISODate()}));
    assert.commandWorked(
        st.s.adminCommand({moveCollection: testDB[kOutCollName].getFullName(), toShard: kPrimary}));

    let bucketCollections = listCollections(kOutBucketsCollName);
    assert.eq(1, bucketCollections.length, bucketCollections);

    const lookUpPipeline = [
        {$lookup: {from: kLookUpCollName, localField: "x", foreignField: "x", as: "bb"}},
        {$out: {db: kDbName, coll: kOutCollName, timeseries: {timeField: 't'}}}
    ];

    // Must reset the profiler for all shards before running the aggregation.
    const kOtherShardDB = st.shard1.getDB(kDbName);
    assert.commandWorked(kOtherShardDB.setProfilingLevel(0));
    kOtherShardDB.system.profile.drop();
    assert.commandWorked(kOtherShardDB.setProfilingLevel(2));

    // Run $out inside a $lookup. The $lookup will force the entire pipeline to run on the
    // non-primary shard. $out will run on a shard that doesn't own the output collection.
    const kLookUpOutComment = "replacing_existing_collection_on_different_shard";
    runOutAndInterrupt(st.shard1, lookUpPipeline, kLookUpOutComment);

    // Confirm the aggregation ran on the other shard using the profiler.
    let profileFilter = {
        "op": "command",
        "command.comment": kLookUpOutComment,
        "ns": testDB[kSourceCollName].getFullName(),
        "command.pipeline.0.$lookup": {"$exists": true},
        "command.pipeline.1.$out": {"$exists": true},
    };
    assert.gt(kOtherShardDB.system.profile.find(profileFilter).itcount(), 0);

    // There should be a buckets collection and a view.
    bucketCollections = listCollections(kOutBucketsCollName);
    assert.eq(1, bucketCollections.length, bucketCollections);
    let view = listCollections(kOutCollName);
    assert.eq(1, view.length, view);
}

if (reshardingForTimeseriesFeatureFlagEnabled) {
    testReplacingExistingCollectionOnDifferentShard();
}

st.stop();
