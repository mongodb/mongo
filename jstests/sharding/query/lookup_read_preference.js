// Tests that $lookup and its subpipelines obey the read preference specified by the user.
// @tags: [requires_majority_read_concern, requires_fcv_51]
(function() {
'use strict';

load('jstests/libs/profiler.js');             // For various profiler helpers.
load('jstests/aggregation/extras/utils.js');  // For arrayEq()

const st = new ShardingTest({name: 'lookup_read_preference', mongos: 1, shards: 2, rs: {nodes: 2}});

// In this test we perform writes which we expect to read on a secondary, so we need to enable
// causal consistency.
const dbName = jsTestName() + '_db';
st.s0.setCausalConsistency(true);
const mongosDB = st.s0.getDB(dbName);

const local = mongosDB[jsTestName()];
const foreign = mongosDB.foreign;
const otherForeign = mongosDB.other_foreign;

// Shard each collection on _id with 2 chunks: [MinKey, 0), [0, MaxKey].
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(otherForeign, {_id: 1}, {_id: 0}, {_id: 0});

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
assert.commandWorked(
    local.insert([{_id: -1, a: 1}, {_id: 1, a: 2}], {writeConcern: {w: 'majority'}}));
assert.commandWorked(
    foreign.insert([{_id: -1, b: 2}, {_id: 1, b: 1}], {writeConcern: {w: 'majority'}}));
assert.commandWorked(
    otherForeign.insert([{_id: -1, c: 1}, {_id: 1, c: 2}], {writeConcern: {w: 'majority'}}));

function assertAggRouting(pipeline, expectedResults, comment, readPrefSecondary) {
    // Ensure the query with the given read preference returns expected results. We specify
    // {batchSize: 1} to exercise the case where $lookup subpipelines are dispatched during a
    // getMore. This way, we can ensure sub-operations in a getMore have the right read preference.
    let options = {comment: comment, batchSize: 1};
    if (readPrefSecondary) {
        Object.assign(options,
                      {$readPreference: {mode: 'secondary'}, readConcern: {level: 'majority'}});
    }
    assert(arrayEq(expectedResults, local.aggregate(pipeline, options).toArray()));

    const isNestedLookup =
        pipeline[0].$lookup.pipeline && pipeline[0].$lookup.pipeline[0].hasOwnProperty('$lookup');
    const involvedColls = isNestedLookup ? [local, foreign, otherForeign] : [local, foreign];

    // For each replica set and for each involved collection, ensure queries are routed to the
    // appropriate node.
    for (let rs of [st.rs0, st.rs1]) {
        const targetedNode = readPrefSecondary ? rs.getSecondary() : rs.getPrimary();
        const otherNode = readPrefSecondary ? rs.getPrimary() : rs.getSecondary();

        for (let coll of involvedColls) {
            // Check that aggregates are sent to the appropriate node in the replica set.
            profilerHasAtLeastOneMatchingEntryOrThrow({
                profileDB: targetedNode.getDB(dbName),
                filter: {
                    ns: coll.getFullName(),
                    'command.aggregate': coll.getName(),
                    'command.comment': comment,
                }
            });

            // Check that no aggregates are sent to the other node in the replica set.
            profilerHasZeroMatchingEntriesOrThrow({
                profileDB: otherNode.getDB(dbName),
                filter: {
                    ns: coll.getFullName(),
                    'command.aggregate': coll.getName(),
                    'command.comment': comment,
                }
            });
        }
    }
}

// Test that $lookup and its subpipelines go to the primaries by default.
let pipeline = [{$lookup: {from: foreign.getName(), as: 'bs', localField: 'a', foreignField: 'b'}}];
let expectedRes = [{_id: -1, a: 1, bs: [{_id: 1, b: 1}]}, {_id: 1, a: 2, bs: [{_id: -1, b: 2}]}];
assertAggRouting(pipeline, expectedRes, 'lookup against primary', false);

// Test that $lookup and its subpipelines go to the secondaries when the readPreference is
// secondary.
assertAggRouting(pipeline, expectedRes, 'lookup against secondary', true);

// Test that $lookup, its subpipelines, and a nested $lookup's subpipelines go to the secondaries
// if the read preference is secondary.
pipeline = [
    {$lookup: {
        from: foreign.getName(),
        as: 'bs',
        localField: 'a',
        foreignField: 'b',
        pipeline: [
            {$lookup: {from: otherForeign.getName(), localField: 'b', foreignField: 'c', as: 'cs'}},
            {$unwind: '$cs'},
        ]
    }},
    {$unwind: '$bs'},
    {$project: {'bs._id': 1, 'bs.cs._id': 1}}
];
expectedRes = [{_id: -1, bs: {_id: 1, cs: {_id: -1}}}, {_id: 1, bs: {_id: -1, cs: {_id: 1}}}];
assertAggRouting(pipeline, expectedRes, 'nested lookup against secondary', true);

st.stop();
}());
