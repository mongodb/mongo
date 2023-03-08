/**
 * Test that mongos can establish cursors on all nodes within a sharded cluster.
 */

(function() {
"use strict";
function runTest({shards, nodes}) {
    const st = new ShardingTest({
        mongos: 1,
        shards,
        config: 1,
        rs: {nodes},
    });
    const db = st.s.getDB(jsTestName());
    const results = db.getSiblingDB("admin")
                        .aggregate([
                            {$_internalShardServerInfo: {}},
                        ])
                        .toArray();

    // Assert there are $currentOp results from all hosts.
    const totalHosts = shards * nodes;
    assert.eq(totalHosts, results.length);
    st.stop();
}

runTest({shards: 1, nodes: 2});
runTest({shards: 1, nodes: 1});
runTest({shards: 2, nodes: 1});
runTest({shards: 2, nodes: 2});
runTest({shards: 2, nodes: 3});
runTest({shards: 3, nodes: 2});
}());

/**
 * Test that remote cursors are closed on all nodes when there is an error on one or more nodes.
 */
(function() {
"use strict";
const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 2},
});
const db = st.s.getDB(jsTestName());
const primary = st.rs0.nodes[0];
const secondary = st.rs0.nodes[1];

assert.commandWorked(primary.adminCommand({
    configureFailPoint: 'failCommand',
    mode: 'alwaysOn',
    data: {
        errorCode: ErrorCodes.FailPointEnabled,
        failCommands: ["aggregate"],
        failInternalCommands: true,
    }
}));
assert.commandFailed(db.adminCommand({
    aggregate: 1,
    pipeline: [
        {$_internalShardServerInfo: {}},
    ],
    cursor: {batchSize: 1},
}));
primary.adminCommand({configureFailPoint: 'failCommand', mode: 'off'});

assert.soon(() => {
    const primaryCursors =
        primary.getDB("admin")
            .aggregate([{$currentOp: {idleCursors: true}}, {$match: {type: "idleCursor"}}])
            .toArray();
    const secondaryCursors =
        secondary.getDB("admin")
            .aggregate([{$currentOp: {idleCursors: true}}, {$match: {type: "idleCursor"}}])
            .toArray();

    try {
        assert.eq(0, primaryCursors.length, tojson(primaryCursors));
        assert.eq(0, secondaryCursors.length, tojson(secondaryCursors));
    } catch (err) {
        jsTestLog(tojson(err));
        return false;
    }

    return true;
});

st.stop();
}());
