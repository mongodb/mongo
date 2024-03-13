/**
 * Tests $lookup, $graphLookup, and $unionWith in a sharded environment to verify the local read
 * behavior of subpipelines dispatched as part of these stages.
 *
 * Requires 7.3 to avoid multiversion problems because we updated targeting logic.
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_fcv_73,
 *   # TODO (SERVER-85629): Re-enable this test once redness is resolved in multiversion suites.
 *   DISABLED_TEMPORARILY_DUE_TO_FCV_UPGRADE,
 *   requires_fcv_80
 * ]
 */

import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {enableLocalReadLogs, getLocalReadCount} from "jstests/libs/local_reads.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {
    profilerHasAtLeastOneMatchingEntryOrThrow,
    profilerHasZeroMatchingEntriesOrThrow
} from "jstests/libs/profiler.js";
import {
    moveDatabaseAndUnshardedColls
} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";

const st = new ShardingTest({
    name: jsTestName(),
    mongos: 1,
    shards: 2,
    rs: {
        nodes: 2,
        setParameter: {logComponentVerbosity: tojson({sharding: 2, command: 1})},
    },
    // Disable checking for index consistency to ensure that the config server doesn't issue an
    // aggregate command which triggers the shards to refresh their sharding metadata as this test
    // relies on shards to have specific metadata as specific times.
    other: {configOptions: {setParameter: {enableShardedIndexConsistencyCheck: false}}},
});

// In this test we perform writes which we expect to read on a secondary, so we need to enable
// causal consistency.
const dbName = jsTestName() + '_db';
st.s0.setCausalConsistency(true);
const mongosDB = st.s0.getDB(dbName);
const replSets = [st.rs0, st.rs1];

const local = mongosDB.local;
const foreign = mongosDB.foreign;

assert.commandWorked(
    mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.shard0.shardName}));

// Turn on the profiler and increase the query log level for both shards.
for (let rs of replSets) {
    const primary = rs.getPrimary();
    const secondary = rs.getSecondary();
    assert.commandWorked(primary.getDB(dbName).setProfilingLevel(2, -1));
    assert.commandWorked(primary.adminCommand(
        {setParameter: 1, logComponentVerbosity: {replication: {heartbeats: 0}}}));
    enableLocalReadLogs(primary);
    assert.commandWorked(secondary.getDB(dbName).setProfilingLevel(2, -1));
    assert.commandWorked(secondary.adminCommand(
        {setParameter: 1, logComponentVerbosity: {replication: {heartbeats: 0}}}));
    enableLocalReadLogs(secondary);
}

// Clear the logs on the primary nodes before starting a test to isolate relevant log lines.
function clearLogs() {
    for (let i = 0; i < replSets.length; i++) {
        for (let node of [replSets[i].getPrimary(), replSets[i].getSecondary()]) {
            assert.commandWorked(node.adminCommand({clearLog: "global"}));
        }
    }
}

function getPossibleViewLocalReadCount(node, foreignNs, comment) {
    // Query the logs for local reads against the namespace specified in the top-level stage and the
    // 'foreign' namespace. The latter case catches reads when the original namespace was a view.
    const fullNs = dbName + '.' + foreignNs;
    let countFound = getLocalReadCount(node, fullNs, comment);
    if (fullNs !== foreign.getFullName()) {
        countFound += getLocalReadCount(node, foreign.getFullName(), comment);
    }
    return countFound;
}

/**
 * Asserts that the assertions in the `expected` object hold by querying the profiler.
 * @param {Object} expected - See `assertAggResultAndRouting()`.
 * @param {String} comment - Identifier used as an option in the execution of the query.
 * @param {Array} pipeline - Pipeline executed.
 */
function assertProfilerEntriesMatch(expected, comment, pipeline) {
    const stage = Object.keys(pipeline[0])[0];
    const foreignNs = pipeline[0][stage].from ? pipeline[0][stage].from : pipeline[0][stage].coll;

    for (let i = 0; i < replSets.length; i++) {
        const node =
            expected.executeOnSecondaries ? replSets[i].getSecondary() : replSets[i].getPrimary();

        // Confirm that the top-level execution of the pipeline is as expected.
        if (expected.toplevelExec) {
            const filter = {"command.aggregate": local.getName(), "command.comment": comment};
            filter[`command.pipeline.${stage}`] = {$exists: true};
            if (expected.toplevelExec[i]) {
                profilerHasAtLeastOneMatchingEntryOrThrow({
                    profileDB: node.getDB(dbName),
                    filter: filter,
                });
            } else {
                profilerHasZeroMatchingEntriesOrThrow({
                    profileDB: node.getDB(dbName),
                    filter: filter,
                });
            }
        }

        // Confirm that the subpipeline execution is as expected. Each subpipeline is either
        // sent to remote shards, which can be seen in the profiler, or performed as a local
        // read, which can be seen in a special log line. The filter on the namespace below
        // ensures we catch both pipelines run against the view namespace and pipelines run
        // against the underlying coll. The number of shard targeting and local read operations
        // can depend on which shard, primary or non-primary, executes a subpipeline first. To
        // account for this, the caller should take care when specifying the values for
        // 'subPipelineRemote' and 'subPipelineLocal'.
        if (expected.subPipelineRemote) {
            const filter = {
                $or: [
                    {"command.aggregate": {$eq: foreignNs}},
                    {"command.aggregate": {$eq: foreign.getName()}}
                ],
                "command.comment": comment,
                "errName": {$ne: "StaleConfig"}
            };
            if (expected.subPipelineRemote[i]) {
                profilerHasAtLeastOneMatchingEntryOrThrow({
                    profileDB: node.getDB(dbName),
                    filter: filter,
                });
            } else {
                profilerHasZeroMatchingEntriesOrThrow({
                    profileDB: node.getDB(dbName),
                    filter: filter,
                });
            }
        }

        if (expected.subPipelineLocal) {
            const localReadCount = getPossibleViewLocalReadCount(node, foreignNs, comment);
            if (expected.subPipelineLocal[i]) {
                assert.gt(
                    localReadCount, 0, `Expected non-zero number of local reads for ${node.name}`);
            } else {
                assert.eq(0,
                          localReadCount,
                          `Expected zero local read but found: ${localReadCount} for ${node.name}`);
            }
        }
    }
}

/**
 * Runs the given pipeline with the given options. Asserts that the result set is the same as
 * `expectedResults` and inspects the profiler to verify the expectations in the `expected` object.
 * @param {Array} pipeline - Pipeline to execute against the local collection
 * @param {Array} expectedResults - Set of documents that we expect the pipeline to return
 * @param {Object} opts - Options that are passed to aggregate command
 * @param {Object} expected - Object containing "assertions" about the state of the profiler. This
 *     allows us to make assertions about the types of plans that were used to satisfy the given
 *     pipeline. This object can have the following fields, all of which are arrays of booleans with
 *     length equal to the number of shards in the cluster:
 *       - toplevelExec: true in the i'th position indicates that the i'th shard executed part of
 *         the pipeline; false indicates that shard was not targeted.
 *       - subPipelineRemote: true in the i'th position indicates that the i'th shard performed at
 *         least one non-local read (had to perform shard targeting); false indicates that the shard
 *         did not perform any such reads.
 *       - subPipelineLocal: true in the i'th position indicates that the i'th shard performed at
 *         least one local read; false indicates that the shard performed zero local reads.
 */
function assertAggResultAndRouting(pipeline, expectedResults, opts, expected) {
    // Write documents to each chunk.
    assert.commandWorked(
        local.insert([{_id: -2, a: -2}, {_id: -1, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}],
                     {writeConcern: {w: 'majority'}}));
    assert.commandWorked(
        foreign.insert([{_id: -1, b: 2}, {_id: 1, b: 1}], {writeConcern: {w: 'majority'}}));

    clearLogs();
    const res = local.aggregate(pipeline, opts).toArray();
    assert(arrayEq(expectedResults, res), tojson(res));

    // For each replica set, ensure queries are routed to the appropriate node.
    assertProfilerEntriesMatch(expected, opts.comment, pipeline);

    assert(local.drop());
    assert(foreign.drop());
}

//
// $unionWith tests
//

// Ensure the $unionWith stage is executed on the same shard to reduce flakiness.
let pipeline = [
    {$unionWith: {coll: foreign.getName(), pipeline: [{$match: {b: {$gte: 0}}}]}},
    {$_internalSplitPipeline: {mergeType: {"specificShard": st.shard0.shardName}}}
];
let expectedRes = [
    {_id: -2, a: -2},
    {_id: -1, a: 1},
    {_id: 1, a: 2},
    {_id: 2, a: 3},
    {_id: -1, b: 2},
    {_id: 1, b: 1}
];

// Test $unionWith when the foreign collection is sharded.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});

assertAggResultAndRouting(pipeline, expectedRes, {comment: "unionWith_to_sharded"}, {
    // The $unionWith stage is always run only on the primary.
    toplevelExec: [true, false],
    // The node executing the $unionWith will open a cursor on every shard that contains the
    // foreign collection.
    subPipelineLocal: [false, false],
    subPipelineRemote: [true, true],
});

// Test $unionWith when the foreign collection is unsharded.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

assertAggResultAndRouting(pipeline, expectedRes, {comment: "unionWith_to_unsharded"}, {
    toplevelExec: [true, false],
    // The node executing the $unionWith can read locally from the foreign collection, since it
    // is also on the primary and is unsharded.
    subPipelineLocal: [true, false],
    subPipelineRemote: [false, false]
});

// Test $unionWith when the foreign namespace is a view of a sharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});
assert.commandWorked(mongosDB.createView("viewOfSharded", foreign.getName(), []));

pipeline[0].$unionWith.coll = "viewOfSharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "unionWith_to_view_of_sharded"}, {
    toplevelExec: [true, false],
    // The node executing the $unionWith will open a cursor on every shard that contains the
    // foreign namespace.
    subPipelineLocal: [false, false],
    subPipelineRemote: [true, true]
});

// Test $unionWith when the foreign namespace is a view of an unsharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
assert.commandWorked(mongosDB.createView("viewOfUnsharded", foreign.getName(), []));

pipeline[0].$unionWith.coll = "viewOfUnsharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "unionWith_to_view_of_unsharded"}, {
    toplevelExec: [true, false],
    // The node executing the $unionWith can read locally from the foreign namespace, since it
    // is also on the primary and is unsharded.
    subPipelineLocal: [true, false],
    subPipelineRemote: [false, false],
});

// Test $unionWith when the foreign collection does not exist.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

expectedRes = [{_id: -2, a: -2}, {_id: -1, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}];
pipeline[0].$unionWith.coll = "unionWithCollDoesNotExist";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "unionWith_foreign_does_not_exist"}, {
    toplevelExec: [true, false],
    // The node executing the $unionWith believes it has stale information about the foreign
    // collection and needs to target shards to properly resolve it.
    subPipelineLocal: [false, false],
    subPipelineRemote: [true, false],
});

//
// $graphLookup tests
//

pipeline = [
    {$graphLookup: {
        from: foreign.getName(),
        startWith: "$a",
        connectFromField: "_id",
        connectToField: "b",
        as: "bs"
    }},
];
expectedRes = [
    {_id: -2, a: -2, bs: []},
    {_id: -1, a: 1, bs: [{_id: 1, b: 1}]},
    {_id: 1, a: 2, bs: [{_id: -1, b: 2}]},
    {_id: 2, a: 3, bs: []}
];

// Test $graphLookup when the foreign collection is sharded.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});

assertAggResultAndRouting(pipeline, expectedRes, {comment: "graphLookup_to_sharded"}, {
    // The $graphLookup stage can always run in parallel across all nodes where the local collection
    // exists.
    toplevelExec: [true, true],
    // Each node executing the $graphLookup will perform a scatter-gather query and open a cursor on
    // every shard that contains the foreign collection. We need a query into the foreign coll for
    // each doc in the local coll, plus one additional recursive query for {b: {$eq: -1}}.
    subPipelineLocal: [false, false],
    subPipelineRemote: [true, true],
});

// Test $graphLookup when the foreign collection is unsharded.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

assertAggResultAndRouting(pipeline, expectedRes, {comment: "graphLookup_to_unsharded"}, {
    toplevelExec: [true, true],
    // The shard executing the $graphLookup can read locally from the foreign collection,
    // since it is unsharded.
    subPipelineLocal: [true, false],
    subPipelineRemote: [true, false]
});

// Test $graphLookup when the foreign namespace is a view of a sharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});

pipeline[0].$graphLookup.from = "viewOfSharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "graphLookup_to_view_of_sharded"}, {
    toplevelExec: [true, true],
    subPipelineLocal: [false, false],
    // The node executing the $graphLookup will perform a scatter-gather query and open a cursor on
    // every shard that contains the foreign collection.
    subPipelineRemote: [true, true]
});

// Test $graphLookup when the foreign namespace is a view of an unsharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

pipeline[0].$graphLookup.from = "viewOfUnsharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "graphLookup_to_view_of_unsharded"}, {
    toplevelExec: [true, true],
    // The shard executing the $graphLookup can read locally from the foreign collection, since it
    // is unsharded. The other node sends the subpipelines over the network.
    subPipelineLocal: [true, false],
    subPipelineRemote: [true, false]
});

// Test $graphLookup when the foreign collection does not exist.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

pipeline[0].$graphLookup.from = "graphLookupCollDoesNotExist";
expectedRes = [
    {_id: -2, a: -2, bs: []},
    {_id: -1, a: 1, bs: []},
    {_id: 1, a: 2, bs: []},
    {_id: 2, a: 3, bs: []}
];
assertAggResultAndRouting(pipeline, expectedRes, {comment: "graphLookup_foreign_does_not_exist"}, {
    toplevelExec: [true, true],
    // If the primary node tries to execute a subpipeline first, then it believes it has stale info
    // about the foreign coll and needs to target shards to properly resolve it. Afterwards, it can
    // do local reads. As before, the other node sends its subpipelines over the network. This
    // results in 3 remote reads. If the non-primary shard sends a subpipeline to execute on the
    // primary shard first, then the primary does a coll refresh before it attempts to run one of
    // its own subpipelines and does not need to target shards. This results in 2 remote reads.
    subPipelineLocal: [true, false],
    subPipelineRemote: [true, false]
});

//
// $lookup tests
//

pipeline = [{$lookup: {from: foreign.getName(), as: 'bs', localField: 'a', foreignField: 'b'}}];
expectedRes = [
    {_id: -2, a: -2, bs: []},
    {_id: -1, a: 1, bs: [{_id: 1, b: 1}]},
    {_id: 1, a: 2, bs: [{_id: -1, b: 2}]},
    {_id: 2, a: 3, bs: []}
];

// Test $lookup when the foreign collection is sharded.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});

assertAggResultAndRouting(pipeline, expectedRes, {comment: "lookup_to_sharded"}, {
    // The $lookup can be executed in parallel because mongos knows both collections are sharded.
    toplevelExec: [true, true],
    // For every document that flows through the $lookup stage, each node executing the $lookup
    // will perform a scatter-gather query and open a cursor on every shard that contains the
    // foreign collection.
    subPipelineLocal: [false, false],
    subPipelineRemote: [true, true]
});

// Test $lookup when the foreign collection is unsharded.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

assertAggResultAndRouting(pipeline, expectedRes, {comment: "lookup_to_unsharded"}, {
    // The $lookup cannot be executed in parallel because the foreign collection is unsharded.
    toplevelExec: [true, false],
    // The node executing the $lookup can read locally from the foreign namespace, since it is also
    // on the primary and is unsharded.
    subPipelineLocal: [true, false],
    subPipelineRemote: [false, false]
});

// Test $lookup when the foreign namespace is a view of a sharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});

pipeline[0].$lookup.from = "viewOfSharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "lookup_to_view_of_sharded"}, {
    // The $lookup is not executed in parallel because mongos does not know the foreign
    // namespace is sharded.
    toplevelExec: [true, false],
    // For every document that flows through the $lookup stage, each node executing the $lookup
    // will perform a scatter-gather query and open a cursor on every shard that contains the
    // foreign collection.
    subPipelineLocal: [false, false],
    subPipelineRemote: [true, true]
});

// Test $lookup when both collection are sharded, but each shard only needs local data to perform
// the lookup.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});
const idLookupPipeline =
    [{$lookup: {from: foreign.getName(), as: 'bs', localField: '_id', foreignField: '_id'}}];
const idLookupExpectedRes = [
    {_id: -2, a: -2, bs: []},
    {_id: -1, a: 1, bs: [{_id: -1, b: 2}]},
    {_id: 1, a: 2, bs: [{_id: 1, b: 1}]},
    {_id: 2, a: 3, bs: []}
];

assertAggResultAndRouting(idLookupPipeline, idLookupExpectedRes, {comment: "lookup_on_shard_key"}, {
    // The $lookup is executed on each shard.
    toplevelExec: [true, true],
    // Because $lookup is done on shard key, shards must be able to determine that data is needed
    // only from the same shard and perform reads locally.
    subPipelineLocal: [true, true],
    subPipelineRemote: [false, false]
});

// Test $lookup when the foreign namespace is a view of an unsharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

pipeline[0].$lookup.from = "viewOfUnsharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "lookup_to_view_of_unsharded"}, {
    // The $lookup is not executed in parallel because mongos defaults to believing the foreign
    // namespace is unsharded.
    toplevelExec: [true, false],
    // The node executing the $lookup can read locally from the foreign namespace, since it is also
    // on the primary and is unsharded.
    subPipelineLocal: [true, false],
    subPipelineRemote: [false, false],
});

// TODO SERVER-77915 remove this test case since the unsharded collection are now tracked and cannot
// be unknown by a the secondary node: The secondary node in the below test case has staleDbVersion.
// This leads the pipeline targeter to run a remote request and refresh. Now that collections are
// tracked, the aggregation is sent using ShardVersion, which is never stale due to an
// AutoGetCollection in the pipeline execution path that causes always to refresh if not installed.
const isTrackUnshardedUponCreationEnabled = FeatureFlagUtil.isPresentAndEnabled(
    st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation");
if (!isTrackUnshardedUponCreationEnabled) {
    // Test $lookup when it is routed to a secondary which is not yet aware of the foreign
    // collection.
    st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

    pipeline[0].$lookup.from = foreign.getName();
    assertAggResultAndRouting(pipeline,
                              expectedRes,
                              {
                                  comment: "lookup_on_stale_secondary",
                                  $readPreference: {mode: 'secondary'},
                                  readConcern: {level: 'majority'}
                              },
                              {
                                  executeOnSecondaries: true,
                                  // The $lookup cannot be executed in parallel because the foreign
                                  // collection is unsharded.
                                  toplevelExec: [true, false],
                                  subPipelineLocal: [true, false],
                                  subPipelineRemote: [true, false],
                              });
}

// Test $lookup when it is routed to a secondary which is aware of the foreign collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
pipeline[0].$lookup.from = foreign.getName();

// Ensure the secondary knows about the foreign collection.
assert.eq(foreign.aggregate([], {$readPreference: {mode: 'secondary'}}).itcount(), 0);
assertAggResultAndRouting(pipeline,
                          expectedRes,
                          {
                              comment: "lookup_on_secondary",
                              $readPreference: {mode: 'secondary'},
                              readConcern: {level: 'majority'}
                          },
                          {
                              executeOnSecondaries: true,
                              // The $lookup cannot be executed in parallel because the foreign
                              // collection is unsharded.
                              toplevelExec: [true, false],
                              subPipelineLocal: [true, false],
                              subPipelineRemote: [false, false],
                          });

// Test $lookup when it is routed to a secondary which thinks the foreign collection is unsharded,
// but it is stale.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
pipeline[0].$lookup.from = foreign.getName();

// Ensure the secondaries know about the local collection.
assert.eq(local.aggregate([], {$readPreference: {mode: 'secondary'}}).itcount(), 0);

// Ensure the secondary knows about the foreign collection, and thinks it is unsharded.
assert.eq(foreign.aggregate([], {$readPreference: {mode: 'secondary'}}).itcount(), 0);

// Shard the collection through mongos.
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});
assertAggResultAndRouting(
    pipeline,
    expectedRes,
    {
        comment: "lookup_on_stale_secondary_foreign_becomes_sharded",
        $readPreference: {mode: 'secondary'},
        readConcern: {level: 'majority'}
    },
    {
        executeOnSecondaries: true,
        // The $lookup can be executed in parallel since mongos knows both collections are sharded.
        toplevelExec: [true, true],
        subPipelineRemote: [true, true],
        // Omit `subPupelineLocal` assertion.
        // If the primary executes a subpipeline first, it will try and fail to read locally. It
        // falls back to targeting shards, which also fails due to a StaleShardVersionError. The
        // entire subpipeline is re-tried after the refresh. If the non-primary shard executes a
        // subpipeline first, it will refresh and target the correct shards, and the primary will
        // do a refresh before it executes one of its own subpipelines. From then on, for every
        // document that flows through the $lookup stage, each node executing the $lookup will
        // perform a scatter-gather query and open a cursor on every shard that contains the foreign
        // collection.
    });

// Test $lookup when the foreign collection does not exist.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

pipeline[0].$lookup.from = "lookupCollDoesNotExist";
expectedRes = [
    {_id: -2, a: -2, bs: []},
    {_id: -1, a: 1, bs: []},
    {_id: 1, a: 2, bs: []},
    {_id: 2, a: 3, bs: []}
];
assertAggResultAndRouting(pipeline, expectedRes, {comment: "lookup_foreign_does_not_exist"}, {
    // The $lookup is not executed in parallel because mongos defaults to believing the foreign
    // namespace is unsharded.
    toplevelExec: [true, false],
    // The node executing the $lookup believes it has stale information about the foreign
    // collection and needs to target shards to properly resolve it. Then, it can use the local
    // read path for each subpipeline query.
    subPipelineLocal: [true, false],
    // Because $lookup is pushed down, we will try to take a lock on the foreign collection to check
    // foreign collection's sharding state. Given that the stale shard version is resolved earlier
    // and we've figured out that the foreign collection is unsharded, we no longer need to target a
    // shard and instead can read locally. As such, we will not generate an entry in the profiler
    // for querying the foreign collection.
    subPipelineRemote: [false, false],
});

//
// Test $lookup where the foreign collection becomes sharded in the middle of the query.
//

// Set-up the involved collections, keeping 'foreign' unsharded. At the start of the query, the
// top-level $lookup should be able to do a local read when running subpipelines against 'foreign'.
const otherForeign = mongosDB.otherForeign;
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(otherForeign, {_id: 1}, {_id: 0}, {_id: 0});

assert.commandWorked(local.insert([{_id: -1, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}],
                                  {writeConcern: {w: 'majority'}}));
assert.commandWorked(foreign.insert([{_id: -1, b: 2}, {_id: 1, b: 1}, {_id: 2, b: 3}],
                                    {writeConcern: {w: 'majority'}}));
assert.commandWorked(otherForeign.insert([{_id: -1, c: 2}, {_id: 1, c: 1}, {_id: 2, c: 3}],
                                         {writeConcern: {w: 'majority'}}));

// Set a failpoint on the first aggregate to be run against the nested $lookup's foreign collection.
let parallelTestComment = "lookup_foreign_becomes_sharded";
const data = {
    ns: otherForeign.getFullName(),
    commands: ['aggregate'],
    comment: parallelTestComment
};
let failPoint = configureFailPoint(st.shard0, "waitAfterCommandFinishesExecution", data);

pipeline = [
    {$lookup: {from: "foreign", as: 'bs', localField: 'a', foreignField: 'b', pipeline:
        [{$lookup: {from: "otherForeign", as: 'cs', localField: 'b', foreignField: 'c'}}]
    }
}];
expectedRes = [
    {_id: -1, a: 1, bs: [{_id: 1, b: 1, cs: [{_id: 1, c: 1}]}]},
    {_id: 1, a: 2, bs: [{_id: -1, b: 2, cs: [{_id: -1, c: 2}]}]},
    {_id: 2, a: 3, bs: [{_id: 2, b: 3, cs: [{_id: 2, c: 3}]}]}
];

const assertDocumentsAsync = async function(pipeline, expectedRes, comment) {
    const {arrayEq} = await import("jstests/aggregation/extras/utils.js");
    const options = {comment};
    const testDb = db.getSiblingDB(jsTestName() + '_db');
    const res = testDb.local.aggregate(pipeline, options).toArray();

    // Assert we haven't lost any documents
    assert(arrayEq(expectedRes, res));
};

// Start a parallel shell to run the nested $lookup.
clearLogs();
let awaitShell = startParallelShell(
    funWithArgs(assertDocumentsAsync, pipeline, expectedRes, parallelTestComment), st.s.port);

// When we hit this failpoint, the nested $lookup will have just completed its first subpipeline.
// Shard 'foreign' to verify that $lookup execution changes correctly mid-query.
failPoint.wait();
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});

// Let the aggregate complete.
failPoint.off();
awaitShell();

let expectedRouting = {
    // At the beginning, the foreign collection is unsharded, so the $lookup cannot be parallelized.
    toplevelExec: [true, false],
    // We know from prior tests that the $lookup will do a local read for the first local document,
    // since it is executing on the primary with an unsharded foreign collection. Checking the local
    // read log here can be flakey because of the failpoint; the local read log may be rotated off
    // the internal log buffer before we can check for it. Instead, we can just check that for the
    // remaining two local documents, $lookup has to open a cursor on every shard to get correct
    // documents for 'foreign'.
    subPipelineRemote: [true, true]
};
assertProfilerEntriesMatch(expectedRouting, parallelTestComment, pipeline);

//
// Test $lookup where the primary is moved in the middle of the query.
//

// Create the same set-up as before, with the same failpoint.
assert(foreign.drop());
assert.commandWorked(foreign.insert([{_id: -1, b: 2}, {_id: 1, b: 1}, {_id: 2, b: 3}],
                                    {writeConcern: {w: 'majority'}}));

parallelTestComment = "lookup_primary_is_moved";
data.comment = parallelTestComment;
failPoint = configureFailPoint(st.shard0, "waitAfterCommandFinishesExecution", data);

// Start a parallel shell to run the nested $lookup.
clearLogs();
awaitShell = startParallelShell(
    funWithArgs(assertDocumentsAsync, pipeline, expectedRes, parallelTestComment), st.s.port);

// When we hit this failpoint, the nested $lookup will have just completed its first subpipeline.
// Move the primary to the other shard to verify that $lookup execution changes correctly mid-query.
failPoint.wait();
moveDatabaseAndUnshardedColls(
    st.s0.getDB(dbName), st.shard1.shardName, false /* moveShardedData */);

// Let the aggregate complete.
failPoint.off();
awaitShell();

expectedRouting = {
    // We know from prior tests that the $lookup will do a local read for the first document, since
    // it is executing on the primary with an unsharded foreign collection. Checking the local
    // read log here can be flakey because of the failpoint; the local read log may be rotated off
    // the internal log buffer before we can check for it. Instead, we can just check that for the
    // remaining two local documents, $lookup tries and fails to read locally and must target
    // shards to get correct foreign documents.
    subPipelineRemote: [false, true]
};
assertProfilerEntriesMatch(expectedRouting, parallelTestComment, pipeline);

//
// Test $graphLookup where the primary is moved in the middle of the query.
//

// At this point, the primary is Shard1. Create a new sharded local collection, and shard it so that
// all of the data will be on Shard0.
assert(local.drop());
st.shardColl(local, {_id: 1}, {_id: -2}, {_id: -2});
assert.commandWorked(local.insert([{_id: -1, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}],
                                  {writeConcern: {w: 'majority'}}));

// Run a $graphLookup with a nested $lookup. $graphLookup will be parallelized to run on both Shard0
// and Shard1, though only Shard0 will do any real work, since it has all of the local data. Then,
// the primary will be changed back to Shard0. The query should still complete succesfully and give
// the same output as above.
assert.commandWorked(mongosDB.createView("nestedLookupView", foreign.getName(), [
    {$lookup: {from: "otherForeign", as: 'cs', localField: 'b', foreignField: 'c'}}
]));
pipeline = [
    {$graphLookup: {
        from: "nestedLookupView",
        startWith: "$a",
        connectFromField: "_id",
        connectToField: "b",
        as: "bs",
        maxDepth: 0
    }},
];

parallelTestComment = "graphLookup_becomes_primary";
data.comment = parallelTestComment;
failPoint = configureFailPoint(st.shard1, "waitAfterCommandFinishesExecution", data);

// Start a parallel shell to run the $graphLookup.
awaitShell = startParallelShell(
    funWithArgs(assertDocumentsAsync, pipeline, expectedRes, parallelTestComment), st.s.port);

// When we hit this failpoint, the nested $lookup will have just completed its first subpipeline.
// Move the primary to the shard executing $graphLookup to verify that we still get correct results.
failPoint.wait();
assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

// Let the aggregate complete.
failPoint.off();
awaitShell();

st.stop();
