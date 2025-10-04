/*
 * Tests that the minWireVersion and maxWireVersion in StreamableReplicaSetMonitor's
 * TopologyDescription are correct before and after FCV changes.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

/*
 * Returns a regex for the topology change log message for the given replica set where
 * all nodes have the given minWireVersion and maxWireVersion.
 */
function makeTopologyChangeLogMsgRegex(rs, minWireVersion, maxWireVersion) {
    return new RegExp(
        `Topology Change.*${rs.name}` +
            `.*minWireVersion":${minWireVersion}.*maxWireVersion":${maxWireVersion}`.repeat(rs.nodes.length),
    );
}

function runTest(downgradeFCV) {
    jsTestLog("Running test with downgradeFCV: " + downgradeFCV);
    const st = new ShardingTest({mongos: [{setParameter: {replicaSetMonitorProtocol: "sdam"}}], config: 1, shards: 0});

    const latestWireVersion = st.configRS.getPrimary().getMaxWireVersion();
    const downgradedWireVersion =
        downgradeFCV === lastContinuousFCV ? latestWireVersion - 1 : latestWireVersion - numVersionsSinceLastLTS;
    const downgradeRegex = makeTopologyChangeLogMsgRegex(st.configRS, downgradedWireVersion, latestWireVersion);
    const latestRegex = makeTopologyChangeLogMsgRegex(st.configRS, latestWireVersion, latestWireVersion);

    jsTest.log("Verify that the RSM on the mongos sees that the config server node has the latest wire version");
    checkLog.contains(st.s, latestRegex);

    jsTest.log("Downgrade FCV and verify that the RSM on the mongos detects the topology change");
    assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true}));
    checkLog.contains(st.s, downgradeRegex);

    jsTest.log("Upgrade FCV and verify that the RSM on the mongos detects the topology change");
    assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    checkLog.contains(st.s, latestRegex);

    st.stop();
}

runTest(lastContinuousFCV);
runTest(lastLTSFCV);
