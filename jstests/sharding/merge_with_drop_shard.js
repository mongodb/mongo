// Tests that the $merge aggregation stage is resilient to drop shard in both the source and
// output collection during execution.
(function() {
'use strict';

load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode.

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const mongosDB = st.s.getDB(jsTestName());
const sourceColl = mongosDB["source"];
const targetColl = mongosDB["target"];

assert.commandWorked(st.s.getDB("admin").runCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.shard1.name);

function setAggHang(mode) {
    assert.commandWorked(st.shard0.adminCommand(
        {configureFailPoint: "hangWhileBuildingDocumentSourceMergeBatch", mode: mode}));
    assert.commandWorked(st.shard1.adminCommand(
        {configureFailPoint: "hangWhileBuildingDocumentSourceMergeBatch", mode: mode}));
}

function removeShard(shard) {
    // We need the balancer to drain all the chunks out of the shard that is being removed.
    assert.commandWorked(st.startBalancer());
    var res = st.s.adminCommand({removeShard: shard.shardName});
    assert.commandWorked(res);
    assert.eq('started', res.state);
    assert.soon(function() {
        res = st.s.adminCommand({removeShard: shard.shardName});
        assert.commandWorked(res);
        return ('completed' === res.state);
    }, "removeShard never completed for shard " + shard.shardName);

    // Drop the test database on the removed shard so it does not interfere with addShard later.
    assert.commandWorked(shard.getDB(mongosDB.getName()).dropDatabase());

    st.configRS.awaitLastOpCommitted();
    assert.commandWorked(st.s.adminCommand({flushRouterConfig: 1}));
    assert.commandWorked(st.stopBalancer());
}

function addShard(shard) {
    assert.commandWorked(st.s.adminCommand({addShard: shard}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: sourceColl.getFullName(), find: {shardKey: 0}, to: shard}));
}

function runMergeWithMode(
    whenMatchedMode, whenNotMatchedMode, shardedColl, dropShard, expectFailCode) {
    // Set the failpoint to hang in the first call to DocumentSourceCursor's getNext().
    setAggHang("alwaysOn");

    let comment = whenMatchedMode + "_" + whenNotMatchedMode + "_" + shardedColl.getName() + "_1";
    let outFn = `
            const sourceDB = db.getSiblingDB(jsTestName());
            const sourceColl = sourceDB["${sourceColl.getName()}"];
            let cmdRes = sourceDB.runCommand({
                aggregate: "${sourceColl.getName()}",
                pipeline: [{$merge: {
                    into: "${targetColl.getName()}",
                    whenMatched: ${tojson(whenMatchedMode)},
                    whenNotMatched: "${whenNotMatchedMode}"
                }}],
                cursor: {},
                comment: "${comment}"
            });
            
            if (${expectFailCode} !== undefined) {
                assert.commandFailedWithCode(cmdRes, ${expectFailCode});
            } else {
                assert.commandWorked(cmdRes);
            }
        `;

    // Start the $merge aggregation in a parallel shell.
    let mergeShell = startParallelShell(outFn, st.s.port);

    // Wait for the parallel shell to hit the failpoint.
    // currentOp can fail with ShardNotFound since we remove
    // the shard on some test runs.
    assert.soon(
        () => {
            const response = assert.commandWorkedOrFailedWithCode(mongosDB.currentOp({
                $or: [
                    {op: "command", "command.comment": comment},
                    {op: "getmore", "cursor.originatingCommand.comment": comment}
                ]
            }),
                                                                  [ErrorCodes.ShardNotFound]);
            return (response.ok) ? response.inprog.length >= 1 : false;
        },
        () => {
            const msg = "Timeout waiting for parallel shell to hit the failpoint";
            const response = mongosDB.currentOp();
            return (response.ok) ? msg + ":\n" + tojson(response.inprog) : msg;
        });

    if (dropShard) {
        removeShard(st.shard0);
    } else {
        addShard(st.rs0.getURL());
    }
    // Unset the failpoint to unblock the $merge and join with the parallel shell.
    setAggHang("off");
    mergeShell();

    assert.eq(2, targetColl.find().itcount());
}

// Shard the source collection with shard key {shardKey: 1} and split into 2 chunks.
st.shardColl(sourceColl.getName(), {shardKey: 1}, {shardKey: 0}, false, mongosDB.getName());

// Shard the output collection with shard key {shardKey: 1} and split into 2 chunks.
st.shardColl(targetColl.getName(), {shardKey: 1}, {shardKey: 0}, false, mongosDB.getName());

// Write two documents in the source collection that should target the two chunks in the target
// collection.
assert.commandWorked(sourceColl.insert({shardKey: -1, _id: 0}));
assert.commandWorked(sourceColl.insert({shardKey: 1, _id: 1}));

withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
    assert.commandWorked(targetColl.remove({}));

    // Match the data from source into target so that we don't fail the assertion for
    // 'whenNotMatchedMode:fail/discard'.
    if (whenNotMatchedMode == "fail" || whenNotMatchedMode == "discard") {
        assert.commandWorked(targetColl.insert({shardKey: -1, _id: 0}));
        assert.commandWorked(targetColl.insert({shardKey: 1, _id: 1}));
    }

    runMergeWithMode(whenMatchedMode, whenNotMatchedMode, targetColl, true, undefined);
    runMergeWithMode(whenMatchedMode,
                     whenNotMatchedMode,
                     targetColl,
                     false,
                     whenMatchedMode == "fail" ? ErrorCodes.DuplicateKey : undefined);
});

st.stop();
})();
