/**
 * Tests that server status reports only the state of cluster parameters that have been set.
 *
 * @tags: [
 *  requires_fcv_80
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

var st = new ShardingTest({shards: 1});

function getShardingStats(conn) {
    return assert.commandWorked(conn.adminCommand({serverStatus: 1})).sharding;
}

function verifyClusterParametersReported(expected, connTypeName, conn) {
    const stats = getShardingStats(conn);
    const reported = "clusterParameters" in stats;
    assert(reported === expected,
           `Expected cluster parameters to ${
               expected
                   ? ""
                   : "not "} be reported in sharding section by ${connTypeName}: ${tojson(stats)}`);
}

function verifyClusterParameterReportedOnAllNodeTypes(expected) {
    verifyClusterParametersReported(expected, "mongos", st.s);
    verifyClusterParametersReported(expected, "mongod", st.rs0.getPrimary());
    verifyClusterParametersReported(expected, "config server", st.configRS.getPrimary());
}

verifyClusterParameterReportedOnAllNodeTypes(false);

assert.commandWorked(
    st.s.adminCommand({setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: true}}}));
assert.commandWorked(st.s.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}));

verifyClusterParameterReportedOnAllNodeTypes(true);

st.stop();
