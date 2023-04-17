/**
 * Verify the "cluster" versions of commands can only run on a sharding enabled shardsvr mongod.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/config_shard_util.js");

const kDBName = "foo";
const kCollName = "bar";

// Note some commands may still fail, e.g. committing a non-existent transaction, so we validate the
// error was different than when a command is rejected for not having sharding enabled.
const clusterCommandsCases = [
    {cmd: {clusterAbortTransaction: 1}, expectedErr: ErrorCodes.InvalidOptions},
    {cmd: {clusterAggregate: kCollName, pipeline: [{$match: {}}], cursor: {}}},
    {cmd: {clusterCommitTransaction: 1}, expectedErr: ErrorCodes.InvalidOptions},
    {cmd: {clusterCount: "x"}},
    {cmd: {clusterDelete: kCollName, deletes: [{q: {}, limit: 1}]}},
    {cmd: {clusterFind: kCollName}},
    {
        cmd: {clusterGetMore: NumberLong(1), collection: kCollName},
        expectedErr: ErrorCodes.CursorNotFound
    },
    {cmd: {clusterInsert: kCollName, documents: [{x: 1}]}},
    {cmd: {clusterUpdate: kCollName, updates: [{q: {doesNotExist: 1}, u: {x: 1}}]}},
];

function runTestCaseExpectFail(conn, testCase, code) {
    assert.commandFailedWithCode(conn.adminCommand(testCase.cmd), code, tojson(testCase.cmd));
}

function runTestCaseExpectSuccess(conn, testCase) {
    assert.commandWorked(conn.adminCommand(testCase.cmd), tojson(testCase.cmd));
}

//
// Standalone mongods have cluster commands, but they cannot be run.
//
{
    const standalone = MongoRunner.runMongod({});
    assert(standalone);

    for (let testCase of clusterCommandsCases) {
        runTestCaseExpectFail(standalone, testCase, ErrorCodes.ShardingStateNotInitialized);
    }

    MongoRunner.stopMongod(standalone);
}

//
// Standalone replica sets mongods have cluster commands, but they cannot be run.
//
{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    for (let testCase of clusterCommandsCases) {
        runTestCaseExpectFail(rst.getPrimary(), testCase, ErrorCodes.ShardingStateNotInitialized);
    }

    rst.stopSet();
}

//
// Cluster commands exist on shardsvrs but require sharding to be enabled.
//
{
    const shardsvrRst = new ReplSetTest({nodes: 1});
    shardsvrRst.startSet({shardsvr: ""});
    shardsvrRst.initiate();

    for (let testCase of clusterCommandsCases) {
        runTestCaseExpectFail(
            shardsvrRst.getPrimary(), testCase, ErrorCodes.ShardingStateNotInitialized);
    }

    shardsvrRst.stopSet();
}

{
    const st = new ShardingTest({mongos: 1, shards: 1, config: 1});

    //
    // Cluster commands do not exist on mongos.
    //

    for (let testCase of clusterCommandsCases) {
        runTestCaseExpectFail(st.s, testCase, ErrorCodes.CommandNotFound);
    }

    //
    // Cluster commands are allowed on a config shard enabled config server.
    //

    const isConfigShardEnabled = ConfigShardUtil.isEnabledIgnoringFCV(st);
    for (let testCase of clusterCommandsCases) {
        if (isConfigShardEnabled) {
            if (testCase.expectedErr) {
                runTestCaseExpectFail(st.rs0.getPrimary(), testCase, testCase.expectedErr);
            } else {
                runTestCaseExpectSuccess(st.rs0.getPrimary(), testCase);
            }
        } else {
            runTestCaseExpectFail(st.configRS.getPrimary(), testCase, ErrorCodes.NoShardingEnabled);
        }
    }

    //
    // Cluster commands work on sharding enabled shardsvr.
    //

    for (let testCase of clusterCommandsCases) {
        if (testCase.expectedErr) {
            runTestCaseExpectFail(st.rs0.getPrimary(), testCase, testCase.expectedErr);
        } else {
            runTestCaseExpectSuccess(st.rs0.getPrimary(), testCase);
        }
    }

    st.stop();
}
}());
