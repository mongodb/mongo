/**
 * Tests $lookup, $graphLookup, and $unionWith in a sharded environment to verify the local read
 * behavior of subpipelines dispatched as part of these stages.
 *
 * # Ban this test in pre-6.0 versions because the SBE is disabled in v6.0 but enabled in v5.3,
 * # which could be problematic in multiversion tasks.
 * @tags: [requires_majority_read_concern, requires_fcv_60]
 */

(function() {
'use strict';

load('jstests/libs/profiler.js');             // For various profiler helpers.
load('jstests/aggregation/extras/utils.js');  // For arrayEq()
load("jstests/libs/fail_point_util.js");      // for configureFailPoint.
load("jstests/libs/log.js");                  // For findMatchingLogLines.

const st = new ShardingTest({name: jsTestName(), mongos: 1, shards: 2, rs: {nodes: 2}});

// In this test we perform writes which we expect to read on a secondary, so we need to enable
// causal consistency.
const dbName = jsTestName() + '_db';
st.s0.setCausalConsistency(true);
const mongosDB = st.s0.getDB(dbName);
const replSets = [st.rs0, st.rs1];

const local = mongosDB.local;
const foreign = mongosDB.foreign;

assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.shard0.shardName);

// Turn on the profiler and increase the query log level for both shards.
for (let rs of replSets) {
    const primary = rs.getPrimary();
    const secondary = rs.getSecondary();
    assert.commandWorked(primary.getDB(dbName).setProfilingLevel(2, -1));
    assert.commandWorked(primary.adminCommand({
        setParameter: 1,
        logComponentVerbosity: {query: {verbosity: 3}, replication: {heartbeats: 0}}
    }));
    assert.commandWorked(secondary.getDB(dbName).setProfilingLevel(2, -1));
    assert.commandWorked(secondary.adminCommand({
        setParameter: 1,
        logComponentVerbosity: {query: {verbosity: 3}, replication: {heartbeats: 0}}
    }));
}

// Clear the logs on the primary nodes before starting a test to isolate relevant log lines.
function clearLogs() {
    for (let i = 0; i < replSets.length; i++) {
        for (let node of [replSets[i].getPrimary(), replSets[i].getSecondary()]) {
            assert.commandWorked(node.adminCommand({clearLog: "global"}));
        }
    }
}

// Returns true if the number of log lines on any primary exceeded the internal log buffer size.
function logLinesExceededBufferSize() {
    for (let i = 0; i < replSets.length; i++) {
        for (let node of [replSets[i].getPrimary(), replSets[i].getSecondary()]) {
            const log = assert.commandWorked(node.adminCommand({getLog: "global"}));
            if (log.totalLinesWritten > 1024) {
                return true;
            }
        }
    }
    return false;
}

function getLocalReadCount(node, foreignNs, comment) {
    if (logLinesExceededBufferSize()) {
        jsTestLog('Warning: total log lines written since start of test is more than internal ' +
                  'buffer size. Some local read log lines may be missing!');
    }

    const log = assert.commandWorked(node.adminCommand({getLog: "global"})).log;

    const countMatchingLogs =
        namespace => [...findMatchingLogLines(
                          log, {id: 5837600, namespace, comment: {comment: comment}})]
                         .length;

    // Query the logs for local reads against the namespace specified in the top-level stage and the
    // 'foreign' namespace. The latter case catches reads when the original namespace was a view.
    const fullNs = dbName + '.' + foreignNs;
    let countFound = countMatchingLogs(fullNs);
    if (fullNs !== foreign.getFullName()) {
        countFound += countMatchingLogs(foreign.getFullName());
    }
    return countFound;
}

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
            profilerHasNumMatchingEntriesOrThrow({
                profileDB: node.getDB(dbName),
                filter: filter,
                numExpectedMatches: expected.toplevelExec[i]
            });
        }

        // Confirm that the subpipeline execution is as expected. Each subpipeline is either sent to
        // remote shards, which can be seen in the profiler, or performed as a local read, which can
        // be seen in a special log line. The filter on the namespace below ensures we catch both
        // pipelines run against the view namespace and pipelines run against the underlying coll.
        // The number of shard targeting and local read operations can depend on which shard,
        // primary or non-primary, executes a subpipeline first. To account for this, the caller can
        // specify an array of possible values for 'subPipelineRemote' and 'subPipelineLocal'.
        const filter = {
            $or: [
                {"command.aggregate": {$eq: foreignNs}},
                {"command.aggregate": {$eq: foreign.getName()}}
            ],
            "command.comment": comment
        };
        const remoteSubpipelineCount = node.getDB(dbName).system.profile.find(filter).itcount();
        let expectedRemoteCountList = expected.subPipelineRemote[i];
        if (!Array.isArray(expectedRemoteCountList)) {
            expectedRemoteCountList = [expectedRemoteCountList];
        }
        assert(expectedRemoteCountList.includes(remoteSubpipelineCount),
               () => 'Expected count of profiler entries to be in ' +
                   tojson(expectedRemoteCountList) + ' but found ' + remoteSubpipelineCount +
                   ' instead in profiler ' +
                   tojson({[node.name]: node.getDB(dbName).system.profile.find().toArray()}));

        if (expected.subPipelineLocal) {
            const localReadCount = getLocalReadCount(node, foreignNs, comment);
            let expectedLocalCountList = expected.subPipelineLocal[i];
            if (!Array.isArray(expectedLocalCountList)) {
                expectedLocalCountList = [expectedLocalCountList];
            }
            assert(expectedLocalCountList.includes(localReadCount),
                   () => 'Expected count of local reads to be in ' +
                       tojson(expectedLocalCountList) + ' but found ' + localReadCount +
                       ' instead for node ' + node.name);
        }
    }
}

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

// Ensure the $unionWith stage is executed on the primary to reduce flakiness.
let pipeline = [
    {$unionWith: {coll: foreign.getName(), pipeline: [{$match: {b: {$gte: 0}}}]}},
    {$_internalSplitPipeline: {mergeType: "primaryShard"}}
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
    toplevelExec: [1, 0],
    // The node executing the $unionWith will open a cursor on every shard that contains the
    // foreign collection.
    subPipelineLocal: [0, 0],
    subPipelineRemote: [1, 1],
});

// Test $unionWith when the foreign collection is unsharded.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

assertAggResultAndRouting(pipeline, expectedRes, {comment: "unionWith_to_unsharded"}, {
    toplevelExec: [1, 0],
    // The node executing the $unionWith can read locally from the foreign collection, since it
    // is also on the primary and is unsharded.
    subPipelineLocal: [1, 0],
    subPipelineRemote: [0, 0]
});

// Test $unionWith when the foreign namespace is a view of a sharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});
assert.commandWorked(mongosDB.createView("viewOfSharded", foreign.getName(), []));

pipeline[0].$unionWith.coll = "viewOfSharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "unionWith_to_view_of_sharded"}, {
    toplevelExec: [1, 0],
    // The node executing the $unionWith will open a cursor on every shard that contains the
    // foreign namespace.
    subPipelineLocal: [0, 0],
    subPipelineRemote: [1, 1]
});

// Test $unionWith when the foreign namespace is a view of an unsharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
assert.commandWorked(mongosDB.createView("viewOfUnsharded", foreign.getName(), []));

pipeline[0].$unionWith.coll = "viewOfUnsharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "unionWith_to_view_of_unsharded"}, {
    toplevelExec: [1, 0],
    // The node executing the $unionWith can read locally from the foreign namespace, since it
    // is also on the primary and is unsharded.
    subPipelineLocal: [1, 0],
    subPipelineRemote: [0, 0],
});

// Test $unionWith when the foreign collection does not exist.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

expectedRes = [{_id: -2, a: -2}, {_id: -1, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}];
pipeline[0].$unionWith.coll = "unionWithCollDoesNotExist";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "unionWith_foreign_does_not_exist"}, {
    toplevelExec: [1, 0],
    // The node executing the $unionWith believes it has stale information about the foreign
    // collection and needs to target shards to properly resolve it.
    subPipelineLocal: [1, 0],
    subPipelineRemote: [1, 0],
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
    toplevelExec: [1, 1],
    // Each node executing the $graphLookup will perform a scatter-gather query and open a cursor on
    // every shard that contains the foreign collection. We need a query into the foreign coll for
    // each doc in the local coll, plus one additional recursive query for {b: {$eq: -1}}.
    subPipelineLocal: [0, 0],
    subPipelineRemote: [5, 5],
});

// Test $graphLookup when the foreign collection is unsharded.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

assertAggResultAndRouting(pipeline, expectedRes, {comment: "graphLookup_to_unsharded"}, {
    toplevelExec: [1, 1],
    // The primary shard executing the $graphLookup can read locally from the foreign collection,
    // since it is unsharded. The other node sends the subpipelines over the network.
    subPipelineLocal: [2, 0],
    subPipelineRemote: [3, 0]
});

// Test $graphLookup when the foreign namespace is a view of a sharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});

pipeline[0].$graphLookup.from = "viewOfSharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "graphLookup_to_view_of_sharded"}, {
    toplevelExec: [1, 1],
    // Each node executing the $graphLookup will perform a scatter-gather query and open a cursor on
    // every shard that contains the foreign collection. The non-primary shard sends one additional
    // query which helps it resolve the sharded view.
    subPipelineLocal: [0, 0],
    subPipelineRemote: [6, 5]
});

// Test $graphLookup when the foreign namespace is a view of an unsharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

pipeline[0].$graphLookup.from = "viewOfUnsharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "graphLookup_to_view_of_unsharded"}, {
    toplevelExec: [1, 1],
    // The primary shard executing the $graphLookup can read locally from the foreign collection,
    // since it is unsharded. The other node sends the subpipelines over the network.
    subPipelineLocal: [2, 0],
    subPipelineRemote: [3, 0]
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
    toplevelExec: [1, 1],
    // If the primary node tries to execute a subpipeline first, then it believes it has stale info
    // about the foreign coll and needs to target shards to properly resolve it. Afterwards, it can
    // do local reads. As before, the other node sends its subpipelines over the network. This
    // results in 3 remote reads. If the non-primary shard sends a subpipeline to execute on the
    // primary shard first, then the primary does a coll refresh before it attempts to run one of
    // its own subpipelines and does not need to target shards. This results in 2 remote reads.
    subPipelineLocal: [2, 0],
    subPipelineRemote: [[2, 3], 0]
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
    toplevelExec: [1, 1],
    // For every document that flows through the $lookup stage, each node executing the $lookup
    // will perform a scatter-gather query and open a cursor on every shard that contains the
    // foreign collection.
    subPipelineLocal: [0, 0],
    subPipelineRemote: [4, 4]
});

// Test $lookup when the foreign collection is unsharded.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

assertAggResultAndRouting(pipeline, expectedRes, {comment: "lookup_to_unsharded"}, {
    // The $lookup cannot be executed in parallel because the foreign collection is unsharded.
    toplevelExec: [1, 0],
    // The node executing the $lookup can read locally from the foreign namespace, since it is also
    // on the primary and is unsharded.
    subPipelineLocal: [4, 0],
    subPipelineRemote: [0, 0]
});

// Test $lookup when the foreign namespace is a view of a sharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});

pipeline[0].$lookup.from = "viewOfSharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "lookup_to_view_of_sharded"}, {
    // The $lookup is not executed in parallel because mongos does not know the foreign
    // namespace is sharded.
    toplevelExec: [1, 0],
    // For every document that flows through the $lookup stage, each node executing the $lookup
    // will perform a scatter-gather query and open a cursor on every shard that contains the
    // foreign collection.
    subPipelineLocal: [0, 0],
    subPipelineRemote: [4, 4]
});

// Test $lookup when the foreign namespace is a view of an unsharded collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

pipeline[0].$lookup.from = "viewOfUnsharded";
assertAggResultAndRouting(pipeline, expectedRes, {comment: "lookup_to_view_of_unsharded"}, {
    // The $lookup is not executed in parallel because mongos defaults to believing the foreign
    // namespace is unsharded.
    toplevelExec: [1, 0],
    // The node executing the $lookup can read locally from the foreign namespace, since it is also
    // on the primary and is unsharded.
    subPipelineLocal: [4, 0],
    subPipelineRemote: [0, 0],
});

// Test $lookup when it is routed to a secondary which is not yet aware of the foreign collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

pipeline[0].$lookup.from = foreign.getName();
assertAggResultAndRouting(
    pipeline,
    expectedRes,
    {
        comment: "lookup_on_stale_secondary",
        $readPreference: {mode: 'secondary'},
        readConcern: {level: 'majority'}
    },
    {
        executeOnSecondaries: true,
        // The $lookup cannot be executed in parallel because the foreign collection is unsharded.
        toplevelExec: [1, 0],
        // The secondary executing the $lookup targets remote shards on the first query, since it is
        // missing information about the foreign collection. After it refreshes, it can read locally
        // from the foreign namespace for all remaining queries.
        subPipelineLocal: [3, 0],
        subPipelineRemote: [1, 0],
    });

// Test $lookup when it is routed to a secondary which is aware of the foreign collection.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

// Ensure the secondary knows about the foreign collection.
assert.eq(foreign.aggregate([], {$readPreference: {mode: 'secondary'}}).itcount(), 0);
assertAggResultAndRouting(
    pipeline,
    expectedRes,
    {
        comment: "lookup_on_secondary",
        $readPreference: {mode: 'secondary'},
        readConcern: {level: 'majority'}
    },
    {
        executeOnSecondaries: true,
        // The $lookup cannot be executed in parallel because the foreign collection is unsharded.
        toplevelExec: [1, 0],
        // This time, the secondary can read locally from the foreign namespace for all queries
        // since it does not need to refresh.
        subPipelineLocal: [4, 0],
        subPipelineRemote: [0, 0],
    });

// Test $lookup when it is routed to a secondary which thinks the foreign collection is unsharded,
// but it is stale.
st.shardColl(local, {_id: 1}, {_id: 0}, {_id: 0});

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
        toplevelExec: [1, 1],
        // If the primary executes a subpipeline first, it will try and fail to read locally. It
        // falls back to targeting shards, which also fails due to a StaleShardVersionError. The
        // entire subpipeline is re-tried after the refresh. If the non-primary shard executes a
        // subpipeline first, it will refresh and target the correct shards, and the primary will
        // do a refresh before it executes one of its own subpipelines. From then on, for every
        // document that flows through the $lookup stage, each node executing the $lookup will
        // perform a scatter-gather query and open a cursor on every shard that contains the foreign
        // collection.
        subPipelineLocal: [[0, 1], 0],
        subPipelineRemote: [[4, 5], 4],
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
    toplevelExec: [1, 0],
    // The node executing the $lookup believes it has stale information about the foreign
    // collection and needs to target shards to properly resolve it. Then, it can use the local
    // read path for each subpipeline query.
    subPipelineLocal: [4, 0],
    // Because $lookup is pushed down, we will try to take a lock on the foreign collection to check
    // foreign collection's sharding state. Given that the stale shard version is resolved earlier
    // and we've figured out that the foreign collection is unsharded, we no longer need to target a
    // shard and instead can read locally. As such, we will not generate an entry in the profiler
    // for querying the foreign collection.
    subPipelineRemote: [0, 0],
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

const parallelScript = (pipeline, expectedRes, comment) =>
    `{
    load("jstests/aggregation/extras/utils.js");  // For arrayEq.
    const pipeline = ` +
    tojson(pipeline) +
    `;
    const options = {comment: "` +
    comment +
    `"};

    db = db.getSiblingDB(jsTestName() + '_db');
    const res = db.local.aggregate(pipeline, options).toArray();

    const expectedRes = ` +
    tojson(expectedRes) +
    `;

    // Assert we haven't lost any documents
    assert(arrayEq(expectedRes, res));
}`;

// Start a parallel shell to run the nested $lookup.
clearLogs();
let awaitShell =
    startParallelShell(parallelScript(pipeline, expectedRes, parallelTestComment), st.s.port);

// When we hit this failpoint, the nested $lookup will have just completed its first subpipeline.
// Shard 'foreign' to verify that $lookup execution changes correctly mid-query.
failPoint.wait();
st.shardColl(foreign, {_id: 1}, {_id: 0}, {_id: 0});

// Let the aggregate complete.
failPoint.off();
awaitShell();

let expectedRouting = {
    // At the beginning, the foreign collection is unsharded, so the $lookup cannot be parallelized.
    toplevelExec: [1, 0],
    // We know from prior tests that the $lookup will do a local read for the first local document,
    // since it is executing on the primary with an unsharded foreign collection. Checking the local
    // read log here can be flakey because of the failpoint; the local read log may be rotated off
    // the internal log buffer before we can check for it. Instead, we can just check that for the
    // remaining two local documents, $lookup has to open a cursor on every shard to get correct
    // documents for 'foreign'.
    subPipelineRemote: [2, 2]
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
awaitShell =
    startParallelShell(parallelScript(pipeline, expectedRes, parallelTestComment), st.s.port);

// When we hit this failpoint, the nested $lookup will have just completed its first subpipeline.
// Move the primary to the other shard to verify that $lookup execution changes correctly mid-query.
failPoint.wait();
assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

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
    subPipelineRemote: [0, 2]
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
awaitShell =
    startParallelShell(parallelScript(pipeline, expectedRes, parallelTestComment), st.s.port);

// When we hit this failpoint, the nested $lookup will have just completed its first subpipeline.
// Move the primary to the shard executing $graphLookup to verify that we still get correct results.
failPoint.wait();
assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

// Let the aggregate complete.
failPoint.off();
awaitShell();

st.stop();
}());
