/**
 * Tests that server status reports the state of enabled cluster parameters.
 */

var st = new ShardingTest({shards: 1});

function getShardingStats(conn) {
    return assert.commandWorked(conn.adminCommand({serverStatus: 1})).sharding;
}

function verifyClusterParametersReported(connTypeName, conn) {
    const stats = getShardingStats(conn);
    assert(
        "clusterParameters" in stats,
        `No cluster parameters reported in sharding section by ${connTypeName}: ${tojson(stats)}`);
}

verifyClusterParametersReported("mongos", st.s);
verifyClusterParametersReported("mongod", st.rs0.getPrimary());
verifyClusterParametersReported("config server", st.configRS.getPrimary());

st.stop();
