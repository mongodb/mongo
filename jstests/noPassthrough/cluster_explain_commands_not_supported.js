/**
 * Verify the explaining "cluster" versions of commands is not supported on any mongod
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
(function() {
"use strict";

const kDbName = "cluster_explain_commands";
const kCollName = "bar";

const clusterCommandsCases = [
    {cmd: {explain: {clusterAggregate: kCollName, pipeline: [{$match: {}}], cursor: {}}}},
    {cmd: {explain: {clusterCount: "x"}}},
    {cmd: {explain: {clusterFind: kCollName}}},
    {cmd: {explain: {clusterInsert: kCollName, documents: [{x: 1}]}}},
    {cmd: {explain: {clusterUpdate: kCollName, updates: [{q: {doesNotExist: 1}, u: {x: 1}}]}}},
    {cmd: {explain: {clusterDelete: `${kCollName}`, deletes: [{q: {}, limit: 1}]}}}
];

function runTestCaseExpectFail(conn, testCase, code) {
    assert.commandFailedWithCode(
        conn.getDB(kDbName).runCommand(testCase.cmd), code, tojson(testCase.cmd));
}

//
// Cluster explain commands not supported on standalone mongods
//
{
    const standalone = MongoRunner.runMongod({});
    assert(standalone);

    for (let testCase of clusterCommandsCases) {
        runTestCaseExpectFail(standalone, testCase, ErrorCodes.CommandNotSupported);
    }

    MongoRunner.stopMongod(standalone);
}

//
// Cluster explain commands not supported on replica set mongods
//
{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    for (let testCase of clusterCommandsCases) {
        runTestCaseExpectFail(rst.getPrimary(), testCase, ErrorCodes.CommandNotSupported);
    }

    rst.stopSet();
}

{
    const st = new ShardingTest({mongos: 1, shards: 1, config: 1});

    //
    // Cluster explain commands do not exist on mongos.
    //

    for (let testCase of clusterCommandsCases) {
        runTestCaseExpectFail(st.s, testCase, ErrorCodes.CommandNotFound);
    }

    //
    // Cluster explain commands are not supported on a config server node.
    //

    for (let testCase of clusterCommandsCases) {
        runTestCaseExpectFail(st.configRS.getPrimary(), testCase, ErrorCodes.CommandNotSupported);
    }

    //
    // Cluster explain commands are not supported sharding enabled shardsvr.
    //

    for (let testCase of clusterCommandsCases) {
        runTestCaseExpectFail(st.rs0.getPrimary(), testCase, ErrorCodes.CommandNotSupported);
    }

    st.stop();
}
}());
