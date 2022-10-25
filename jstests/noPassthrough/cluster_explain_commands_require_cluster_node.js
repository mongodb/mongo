/**
 * Verify the explaining "cluster" versions of commands is rejected on a non shardsvr mongod.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

const kDbName = "cluster_explain_commands";
const kCollName = "bar";
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const clusterCommandsCases = [
    {cmd: {explain: {clusterAggregate: kCollName, pipeline: [{$match: {}}], cursor: {}}}},
    {cmd: {explain: {clusterCount: "x"}}},
    {cmd: {explain: {clusterDelete: kCollName, deletes: [{q: {}, limit: 1}]}}},
    {cmd: {explain: {clusterFind: kCollName}}},
    {cmd: {explain: {clusterInsert: kCollName, documents: [{x: 1}]}}},
    {cmd: {explain: {clusterUpdate: kCollName, updates: [{q: {doesNotExist: 1}, u: {x: 1}}]}}},
    {cmd: {explain: {clusterDelete: `${kCollName}`, deletes: [{q: {}, limit: 1}]}}}
];

function runTestCaseExpectFail(conn, testCase, code) {
    assert.commandFailedWithCode(
        conn.getDB(kDbName).runCommand(testCase.cmd), code, tojson(testCase.cmd));
}

for (let testCase of clusterCommandsCases) {
    runTestCaseExpectFail(rst.getPrimary(), testCase, ErrorCodes.ShardingStateNotInitialized);
}

rst.stopSet();
}());
