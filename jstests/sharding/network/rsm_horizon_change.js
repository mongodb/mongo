/*
 * Tests that split horizon reconfig results in unknown ServerDescription in
 * StreamableReplicaSetMonitor.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    mongos: [{setParameter: {replicaSetMonitorProtocol: "streamable"}}],
    config: 1,
    shards: 1,
});
const configRSPrimary = st.configRS.getPrimary();

const unknownTopologyChangeRegex = new RegExp(
    `Topology Change.*${st.configRS.name}.*topologyType":.*ReplicaSetNoPrimary.*type":.*Unknown`,
);
const knownTopologyChangeRegex = new RegExp(
    `Topology Change.*${st.configRS.name}.*topologyType":.*ReplicaSetWithPrimary.*type":.*RSPrimary`,
);
const expeditedMonitoringAfterNetworkErrorRegex = new RegExp(
    `RSM monitoring host in expedited mode until we detect a primary`,
);
const droppingAllPooledConnections = new RegExp("Dropping all pooled connections");

const unknownServerDescriptionRegex = new RegExp(
    "(" +
        unknownTopologyChangeRegex.source +
        ")|(" +
        expeditedMonitoringAfterNetworkErrorRegex.source +
        ")|(" +
        droppingAllPooledConnections.source +
        ")",
);

jsTest.log("Wait until the RSM on the mongos finds out about the config server primary");
checkLog.contains(st.s, knownTopologyChangeRegex);

jsTest.log("Run split horizon reconfig and verify that it results in unknown server description");
const rsConfig = configRSPrimary.getDB("local").system.replset.findOne();
for (let i = 0; i < rsConfig.members.length; i++) {
    rsConfig.members[i].horizons = {specialHorizon: "horizon.com:100" + i};
}
rsConfig.version++;

assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
assert.commandWorked(configRSPrimary.adminCommand({replSetReconfig: rsConfig}));

checkLog.contains(st.s, unknownServerDescriptionRegex);

jsTest.log("Verify that the RSM eventually has the right topology description again");
checkLog.contains(st.s, knownTopologyChangeRegex);
st.stop();
