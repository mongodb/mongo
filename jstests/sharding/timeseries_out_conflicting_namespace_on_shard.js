/*
 * Tests that $out to time-series cleanly fails when a conflicting view or collection exists on a
 * different shard than the shard running the aggregation.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # TODO (SERVER-88125): Re-enable this test or add an explanation why it is incompatible.
 *   embedded_router_incompatible,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const kDbName = jsTestName();
const kOutCollName = 'out';
const kSourceCollName = 'foo';
const kLookUpCollName = 'lookUp';
const testDB = st.s.getDB(kDbName);
const kSourceDocument = {
    x: 1,
    t: ISODate()
};
const kPipelineComment = "test_conflicting_namespace_shard_comment";
const kPrimary = st.shard0.shardName;
const kOther = st.shard1.shardName;
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName, primaryShard: kPrimary}));

// Create source and lookup collections and move both collections to the non-primary shard. We will
// reference these collections in each test.
const sourceColl = testDB[kSourceCollName];
assert.commandWorked(sourceColl.insert(kSourceDocument));
assert.commandWorked(
    st.s.adminCommand({moveCollection: sourceColl.getFullName(), toShard: kOther}));

const lookUpColl = testDB[kLookUpCollName];
assert.commandWorked(lookUpColl.insert([{x: 1, t: ISODate(), lookup: 2}]));
assert.commandWorked(
    st.s.adminCommand({moveCollection: lookUpColl.getFullName(), toShard: kOther}));

// Run an $out aggregation pipeline where the $out runs on the non-primary shard and the collection
// that $out is trying to replace is on the primary shard. Because we have the 'timeseries' option,
// which requires a view, $out will run on the primary shard. So to avoid this and force $out to run
// on the non-primary shard, we must input a $lookup stage first.
function validateAggOutFailed(createCommand) {
    assert(testDB[kOutCollName].drop());
    assert.commandWorked(testDB.runCommand(createCommand));
    assert.commandWorked(
        st.s.adminCommand({moveCollection: sourceColl.getFullName(), toShard: kPrimary}));

    // Must reset the profiler for all shards before running the aggregation.
    const kOtherShardDB = st.shard1.getDB(kDbName);
    assert.commandWorked(kOtherShardDB.setProfilingLevel(0));
    kOtherShardDB.system.profile.drop();
    assert.commandWorked(kOtherShardDB.setProfilingLevel(2));

    const kPipeline = [
        {$lookup: {from: kLookUpCollName, localField: "x", foreignField: "x", as: "bb"}},
        {$out: {db: kDbName, coll: kOutCollName, timeseries: {timeField: 't'}}}
    ];

    assert.throwsWithCode(() => sourceColl.aggregate(kPipeline, {comment: kPipelineComment}),
                          7268700);

    // Confirm the aggregation ran on the other shard using the profiler.
    let profileFilter = {
        "op": "command",
        "command.comment": kPipelineComment,
        "ns": sourceColl.getFullName(),
        "command.pipeline.0.$mergeCursors": {"$exists": true},
        "command.pipeline.1.$lookup": {"$exists": true},
        "command.pipeline.2.$out": {"$exists": true},
    };
    assert.gt(kOtherShardDB.system.profile.find(profileFilter).itcount(), 0);

    // Validate the conflicting view or collection remains after $out failed.
    const collections = testDB.getCollectionNames().filter(coll => !coll.startsWith("system."));
    assert.sameMembers(
        [kSourceCollName, kLookUpCollName, kOutCollName], collections, tojson(collections));
}

validateAggOutFailed({create: kOutCollName});
validateAggOutFailed(
    {create: kOutCollName, viewOn: kLookUpCollName, pipeline: [{$project: {val: 1}}]});

st.stop();
