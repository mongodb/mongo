// Tests that the $merge aggregation stage is resilient to move primary in both the source and
// output collection during execution.
import {withEachMergeMode} from "jstests/aggregation/extras/merge_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const mongosDB = st.s.getDB(jsTestName());
const sourceColl = mongosDB["source"];
const targetColl = mongosDB["target"];

function setAggHang(mode) {
    assert.commandWorked(
        st.shard0.adminCommand({configureFailPoint: "hangWhileBuildingDocumentSourceMergeBatch", mode: mode}),
    );
    assert.commandWorked(
        st.shard1.adminCommand({configureFailPoint: "hangWhileBuildingDocumentSourceMergeBatch", mode: mode}),
    );

    assert.commandWorked(
        st.shard0.adminCommand({configureFailPoint: "hangWhileBuildingDocumentSourceOutBatch", mode: mode}),
    );
    assert.commandWorked(
        st.shard1.adminCommand({configureFailPoint: "hangWhileBuildingDocumentSourceOutBatch", mode: mode}),
    );
}

function runPipelineWithStage({stage, shardedColl, expectedfailCode, expectedNumDocs}) {
    // Set the failpoint to hang in the first call to DocumentSourceCursor's getNext().
    setAggHang("alwaysOn");

    // Set the primary shard.
    assert.commandWorked(st.s.adminCommand({movePrimary: mongosDB.getName(), to: st.shard0.shardName}));

    let comment = jsTestName() + "_comment";
    let outFn = `
            const sourceDB = db.getSiblingDB(jsTestName());
            const sourceColl = sourceDB["${sourceColl.getName()}"];
            let cmdRes = sourceDB.runCommand({
                aggregate: "${sourceColl.getName()}",
                pipeline: [${tojson(stage)}],
                cursor: {},
                comment: "${comment}"
            });
            if (${expectedfailCode} !== undefined) {
                assert.commandFailedWithCode(cmdRes, ${expectedfailCode});
            } else {
                assert.commandWorked(cmdRes);
            }
        `;

    // Start the $merge aggregation in a parallel shell.
    let outShell = startParallelShell(outFn, st.s.port);

    // Wait for the parallel shell to hit the failpoint.
    assert.soon(
        () =>
            mongosDB.currentOp({
                $or: [
                    {op: "command", "command.comment": comment},
                    {op: "getmore", "cursor.originatingCommand.comment": comment},
                ],
            }).inprog.length == 1,
        () => tojson(mongosDB.currentOp().inprog),
    );

    // Migrate the primary shard from shard0 to shard1.
    assert.commandWorked(st.s.adminCommand({movePrimary: mongosDB.getName(), to: st.shard1.shardName}));

    // Unset the failpoint to unblock the $merge and join with the parallel shell.
    setAggHang("off");
    outShell();

    // Verify that the $merge succeeded.
    if (expectedfailCode === undefined) {
        assert.eq(expectedNumDocs, targetColl.find().itcount());
    }

    assert.commandWorked(targetColl.remove({}));
}

// The source collection is unsharded.
assert.commandWorked(sourceColl.insert({shardKey: -1}));
assert.commandWorked(sourceColl.insert({shardKey: 1}));

withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
    runPipelineWithStage({
        stage: {
            $merge: {
                into: targetColl.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode,
            },
        },
        shardedColl: sourceColl,
        expectedNumDocs: whenNotMatchedMode == "discard" ? 0 : 2,
        expectedfailCode: whenNotMatchedMode == "fail" ? 13113 : undefined,
    });
});
sourceColl.drop();

// Shard the source collection with shard key {shardKey: 1} and split into 2 chunks.
st.shardColl(sourceColl.getName(), {shardKey: 1}, {shardKey: 0}, false, mongosDB.getName());

// Write a document to each chunk of the source collection.
assert.commandWorked(sourceColl.insert({shardKey: -1}));
assert.commandWorked(sourceColl.insert({shardKey: 1}));

withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
    runPipelineWithStage({
        stage: {
            $merge: {
                into: targetColl.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode,
            },
        },
        shardedColl: sourceColl,
        expectedNumDocs: whenNotMatchedMode == "discard" ? 0 : 2,
        expectedfailCode: whenNotMatchedMode == "fail" ? 13113 : undefined,
    });
});

sourceColl.drop();

// Shard the source collection with shard key {shardKey: 1} and split into 2 chunks.
st.shardColl(sourceColl.getName(), {shardKey: 1}, {shardKey: 0}, false, mongosDB.getName());

// Write two documents in the source collection that should target the two chunks in the target
// collection.
assert.commandWorked(sourceColl.insert({shardKey: -1}));
assert.commandWorked(sourceColl.insert({shardKey: 1}));

// Note that the legacy $out is not supported with an existing sharded output collection.
withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
    runPipelineWithStage({
        stage: {
            $merge: {
                into: targetColl.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode,
            },
        },
        shardedColl: targetColl,
        expectedNumDocs: whenNotMatchedMode == "discard" ? 0 : 2,
        expectedfailCode: whenNotMatchedMode == "fail" ? 13113 : undefined,
    });
});

sourceColl.drop();
targetColl.drop();

// Shard the collections with shard key {shardKey: 1} and split into 2 chunks.
st.shardColl(sourceColl.getName(), {shardKey: 1}, {shardKey: 0}, false, mongosDB.getName());
st.shardColl(targetColl.getName(), {shardKey: 1}, {shardKey: 0}, false, mongosDB.getName());

// Write two documents in the source collection that should target the two chunks in the target
// collection.
assert.commandWorked(sourceColl.insert({shardKey: -1}));
assert.commandWorked(sourceColl.insert({shardKey: 1}));

withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
    runPipelineWithStage({
        stage: {
            $merge: {
                into: targetColl.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode,
            },
        },
        shardedColl: targetColl,
        expectedNumDocs: whenNotMatchedMode == "discard" ? 0 : 2,
        expectedfailCode: whenNotMatchedMode == "fail" ? 13113 : undefined,
    });
});

st.stop();
