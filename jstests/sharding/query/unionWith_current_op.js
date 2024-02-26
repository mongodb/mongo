/**
 * Tests to verify that the current op output shows all the sub-pipelines of a $unionWith. In this
 * test we also validate that current op shows the expected stages and comment.
 *
 * This test uses a new $_internalSplitPipeline syntax introduced in 8.0.
 * @tags: [  requires_fcv_80, ]
 */
import {waitForCurOpByFailPoint} from "jstests/libs/curop_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const st = new ShardingTest({shards: 2});
const testDB = st.s.getDB(jsTestName());
const shardedColl1 = testDB.shardedColl1;
const shardedColl2 = testDB.shardedColl2;
const unshardedColl = testDB.unsharded;

assert.commandWorked(
    st.s0.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));

function setupShardColl(shardedColl) {
    // Shard shardedColl on {x:1}, split it at {x:0}, and move chunk {x:1} to shard1.
    st.shardColl(shardedColl, {x: 1}, {x: 0}, {x: 1});

    // Insert one document on each shard.
    assert.commandWorked(shardedColl.insert({x: 1, _id: 1}));
    assert.commandWorked(shardedColl.insert({x: -1, _id: 0}));
}

setupShardColl(shardedColl1);
setupShardColl(shardedColl2);
assert.commandWorked(unshardedColl.insert({x: 1, _id: 1}));

const kFailPointName = "waitAfterCommandFinishesExecution";
function setPostCommandFailpointOnShards({mode, options}) {
    FixtureHelpers.runCommandOnEachPrimary({
        db: testDB.getSiblingDB("admin"),
        cmdObj: {configureFailPoint: kFailPointName, data: options, mode: mode}
    });
}

function runTest({command, expectedRunningOps, collToPause}) {
    setPostCommandFailpointOnShards(
        {mode: "alwaysOn", options: {ns: collToPause.getFullName(), commands: ['aggregate']}});

    const commentObj = {
        testName: jsTestName() + "_comment",
        commentField: "comment_aggregate",
        uuid: UUID().hex()
    };
    command["comment"] = commentObj;

    const parallelFunction = `
        const sourceDB = db.getSiblingDB(jsTestName());
        let cmdRes = sourceDB.runCommand(${tojson(command)});
        assert.commandWorked(cmdRes);
    `;

    // Run the 'command' in a parallel shell.
    let unionShell = startParallelShell(parallelFunction, st.s.port);

    const filter = {"command.aggregate": collToPause.getName(), "command.comment": commentObj};
    const deepestUnion = expectedRunningOps[expectedRunningOps.length - 1];
    // Wait for the parallel shell to hit the failpoint and verify that the 'comment' field is
    // present in $currentOp.
    assert.soon(() => {
        const results =
            waitForCurOpByFailPoint(testDB, collToPause.getFullName(), kFailPointName, filter);
        return results.length == deepestUnion.count;
    });

    // Verify that MongoS has an operation running for the base aggregation.
    filter['command.aggregate'] = expectedRunningOps[0].coll.getName();
    const mongosOp = testDB.getSiblingDB("admin")
                         .aggregate([{$currentOp: {localOps: true}}, {$match: filter}])
                         .toArray();
    assert.eq(mongosOp.length, 1, mongosOp);
    for (let expectedOp of expectedRunningOps) {
        let filterForOp = {};
        filterForOp['command.aggregate'] = expectedOp.coll.getName();
        for (let stage of expectedOp.stages) {
            filterForOp['command.pipeline.' + stage] = {$exists: true};
        }
        filterForOp['command.comment'] = commentObj;

        assert.eq(testDB.getSiblingDB("admin")
                      .aggregate([{$currentOp: {localOps: false}}, {$match: filterForOp}])
                      .toArray()
                      .length,
                  expectedOp.count,
                  testDB.getSiblingDB("admin")
                      .aggregate([{$currentOp: {localOps: false}}, {$match: filterForOp}])
                      .toArray());
    }

    // Unset the failpoint to unblock the command and join with the parallel shell.
    setPostCommandFailpointOnShards({mode: "off", options: {}});
    unionShell();
}

// Test that the current op shows all the sub-pipelines when a sharded collection unions with
// another sharded collection. Also validate that the 'comment' is attached to all the related
// operations.
runTest({
    command: {
        aggregate: shardedColl1.getName(),
        pipeline: [
            {$match: {p: null}},
            {
                $unionWith: {
                    coll: shardedColl2.getName(),
                    pipeline: [{$group: {_id: "$x"}}, {$project: {_id: 1}}]
                }
            }
        ],
        cursor: {}
    },
    // We expect to see the merging half of the pipeline still running because the $unionWith hasn't
    // finished, even though we should have exhaused the input cursors by the time we're looking at
    // the union sub-pipeline.
    expectedRunningOps: [
        {coll: shardedColl1, count: 1, stages: ['$mergeCursors', '$unionWith']},
        {coll: shardedColl2, count: 2, stages: ['$group']}
    ],
    collToPause: shardedColl2
});

// Test that the current op shows all the sub-pipelines when a sharded collection unions with an
// unsharded collection which in-turn unions with another sharded collection. Also validate that
// the 'comment' is attached to all the related operations.
runTest({
    command: {
        aggregate: shardedColl1.getName(),
        pipeline: [
            {$match: {p: null}},
            {
                $unionWith: {
                    coll: unshardedColl.getName(),
                    pipeline: [{
                        $unionWith:
                            {coll: shardedColl2.getName(), pipeline: [{$group: {_id: "$x"}}]}
                    }]
                }
            },
            {$_internalSplitPipeline: {mergeType: {"specificShard": st.shard0.shardName}}}
        ],
        cursor: {}
    },
    expectedRunningOps: [
        {coll: shardedColl1, count: 1, stages: ['$mergeCursors', '$unionWith']},
        // the $unionWith on the unsharded collection is run on the primary shard and can be
        // executed as a local read, which will not be logged.
        {coll: shardedColl2, count: 2, stages: ['$group']}
    ],
    collToPause: shardedColl2
});

// Test that the current op shows all the sub-pipelines when an unsharded collection unions with a
// sharded collection which in-turn unions with another sharded collection. Also validate that the
// 'comment' is attached to all the related operations.
runTest({
    command: {
        aggregate: unshardedColl.getName(),
        pipeline: [
            {$match: {p: null}},
            {
                $unionWith: {
                    coll: shardedColl1.getName(),
                    pipeline: [{
                        $unionWith:
                            {coll: shardedColl2.getName(), pipeline: [{$group: {_id: "$x"}}]}
                    }]
                }
            }
        ],
        cursor: {}
    },
    expectedRunningOps: [
        {coll: unshardedColl, count: 1, stages: ['$unionWith']},
        // Note that we don't expect any operation on 'shardedColl1' since the failpoint being used
        // will only block commands on the "deepest" namespace, which is 'shardedColl2' for this
        // command. The cursors on 'shardedColl1' will have been exhausted by the time the command
        // on 'shardedColl2' is started. Note there will still be a $mergeCursors stage around that
        // exhausted those cursors, but it will not get its own current op entry because it's a
        // sub-pipeline on the same host and operation as the pipeline for unshardedColl.
        {coll: shardedColl2, count: 2, stages: ['$group']}
    ],
    collToPause: shardedColl2
});

st.stop();
