// Tests that sub-queries across shards as part of $unionWith will obey the read preference
// specified by the user.
// @tags: [requires_majority_read_concern]
(function() {
"use strict";

load('jstests/libs/profiler.js');             // For various profiler helpers.
load("jstests/aggregation/extras/utils.js");  // For arrayEq()

// For supportsMajorityReadConcern.
load('jstests/multiVersion/libs/causal_consistency_helpers.js');

const st = new ShardingTest({name: "union_with_read_pref", mongos: 1, shards: 2, rs: {nodes: 2}});

// In this test we perform writes which we expect to read on a secondary, so we need to enable
// causal consistency.
const dbName = jsTestName() + "_db";
st.s0.setCausalConsistency(true);
const mongosDB = st.s0.getDB(dbName);

const mongosColl = mongosDB[jsTestName()];
const unionedColl = mongosDB.union_target;

// Shard the test collection on _id with 2 chunks: [MinKey, 0), [0, MaxKey].
st.shardColl(mongosColl, {_id: 1}, {_id: 0}, {_id: 0});
// Shard the union's target collection on _id with the same chunks, but moving the negative chunk
// off the primary shard so their distributions are flipped.
st.shardColl(unionedColl, {_id: 1}, {_id: 0}, {_id: -1});
st.ensurePrimaryShard(dbName, st.shard1.shardName);

// Turn on the profiler.
for (let rs of [st.rs0, st.rs1]) {
    const primary = rs.getPrimary();
    const secondary = rs.getSecondary();
    assert.commandWorked(primary.getDB(dbName).setProfilingLevel(2, -1));
    assert.commandWorked(
        primary.adminCommand({setParameter: 1, logComponentVerbosity: {query: {verbosity: 3}}}));
    assert.commandWorked(secondary.getDB(dbName).setProfilingLevel(2, -1));
    assert.commandWorked(
        secondary.adminCommand({setParameter: 1, logComponentVerbosity: {query: {verbosity: 3}}}));
}

// Write a document to each chunk.
assert.commandWorked(mongosColl.insert([{_id: -1, docNum: 0}, {_id: 1, docNum: 1}],
                                       {writeConcern: {w: "majority"}}));
assert.commandWorked(unionedColl.insert([{_id: -1, docNum: 2}, {_id: 1, docNum: 3}],
                                        {writeConcern: {w: "majority"}}));

// Test that $unionWith goes to the primary by default.
let unionWithComment = "union against primary";
assert.eq(mongosColl
              .aggregate([{$unionWith: unionedColl.getName()}, {$sort: {docNum: 1}}],
                         {comment: unionWithComment})
              .toArray(),
          [{_id: -1, docNum: 0}, {_id: 1, docNum: 1}, {_id: -1, docNum: 2}, {_id: 1, docNum: 3}]);

// Test that the union's sub-pipelines go to the primary.
for (let rs of [st.rs0, st.rs1]) {
    const primaryDB = rs.getPrimary().getDB(dbName);
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: primaryDB,
        filter: {
            ns: unionedColl.getFullName(),
            op: {$ne: "getmore"},
            "command.comment": unionWithComment,
        }
    });
}

// Test that $unionWith subpipelines go to the secondary when the readPreference is {mode:
// "secondary"}.
unionWithComment = 'union against secondary';
assert.eq(mongosColl
              .aggregate([{$unionWith: unionedColl.getName()}, {$sort: {docNum: 1}}], {
                  comment: unionWithComment,
                  $readPreference: {mode: "secondary"},
                  readConcern: {level: "majority"}
              })
              .toArray(),
          [{_id: -1, docNum: 0}, {_id: 1, docNum: 1}, {_id: -1, docNum: 2}, {_id: 1, docNum: 3}]);

// Test that the union's sub-pipelines go to the secondary.
for (let rs of [st.rs0, st.rs1]) {
    const secondaryDB = rs.getSecondary().getDB(dbName);
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: secondaryDB,
        filter: {
            ns: unionedColl.getFullName(),
            op: {$ne: "getmore"},
            "command.comment": unionWithComment,
            // We need to filter out any profiler entries with a stale config - this is the first
            // read on this secondary with a readConcern specified, so it is the first read on this
            // secondary that will enforce shard version.
            errCode: {$ne: ErrorCodes.StaleConfig}
        }
    });
}

// Now a more extreme test, add a nested $unionWith and a more complicated sub - pipeline to ensure
// any sub-operation always goes to the secondary if the read preference is secondary.
const secondTargetColl = mongosDB.second_union_target;
st.shardColl(secondTargetColl, {_id: 1}, {_id: 0}, {_id: -1});
assert.commandWorked(secondTargetColl.insert([{_id: -1, docNum: 4}, {_id: 1, docNum: 5}],
                                             {writeConcern: {w: "majority"}}));
// This find triggers a refresh on the secondaries so the following union is able to find all the
// documents in `second_union_target`. Without the find, the router is seeing this collection for
// the first time and so presumes UNSHARDED, which does not cause mismatch against the shard's
// UNKNOWN version due to SERVER-32198.
// TODO SERVER-32198: Update this test to not depend on the find for refresh.
assert.eq(new DBCommandCursor(mongosDB, assert.commandWorked(mongosDB.runCommand({
              query: {find: secondTargetColl.getName(), readConcern: {level: "local"}},
              $readPreference: {mode: "secondary"}
          }))).itcount(),
          2);
unionWithComment = 'complex union against secondary';
let runAgg = () => mongosColl
                       .aggregate(
                           [
                               {
                                   $unionWith: {
                                       coll: unionedColl.getName(),
                                       pipeline: [
                                           {$unionWith: secondTargetColl.getName()},
                                       ]
                                   }
                               },
                               {$group: {_id: "$_id", docNum: {$push: "$docNum"}}},
                               {$sort: {_id: 1}},
                           ],
                           {
                               comment: unionWithComment,
                               $readPreference: {mode: "secondary"},
                               readConcern: {level: "majority"}
                           })
                       .toArray();
assert.eq(runAgg(), [{_id: -1, docNum: [0, 2, 4]}, {_id: 1, docNum: [1, 3, 5]}]);

// Test that the union's sub-pipelines go to the secondary.
for (let rs of [st.rs0, st.rs1]) {
    jsTestLog(`Testing profile on shard ${rs.getURL()}`);
    const secondaryDB = rs.getSecondary().getDB(dbName);
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: secondaryDB,
        filter: {
            ns: unionedColl.getFullName(),
            op: {$ne: "getmore"},
            "command.comment": unionWithComment,
            // We need to filter out any profiler entries with a stale config - this is the first
            // read on this secondary with a readConcern specified, so it is the first read on this
            // secondary that will enforce shard version.
            errCode: {$ne: ErrorCodes.StaleConfig}
        }
    });
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: secondaryDB,
        filter: {
            ns: secondTargetColl.getFullName(),
            op: {$ne: "getmore"},
            "command.comment": unionWithComment,
            // We need to filter out any profiler entries with a stale config - this is the first
            // read on this secondary with a readConcern specified, so it is the first read on this
            // secondary that will enforce shard version.
            errCode: {$ne: ErrorCodes.StaleConfig}
        }
    });
}

st.stop();
}());
