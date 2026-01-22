/**
 * Test that downgrading is forbidden when a priorityPort is present in the ReplSetConfig and that
 * reconfigs are drained correctly to ensure stability of these checks.
 *
 * @tags: [
 *      featureFlagReplicationUsageOfPriorityPort,
 *      # The priority port is based on ASIO, so gRPC testing is excluded
 *      grpc_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const numNodes = 3;

const rs = new ReplSetTest({nodes: numNodes, usePriorityPorts: true});
rs.startSet();
rs.initiate();

jsTest.log.info("Test that replica set with priority ports enabled cannot downgrade");
assert.commandFailedWithCode(
    rs.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    ErrorCodes.CannotDowngrade,
);

jsTest.log.info("Remove the priority ports one by one and ensure downgrade is only possible once they are all removed");
for (let i = 0; i < numNodes; i++) {
    let config = rs.getReplSetConfigFromNode();
    delete config.members[i].priorityPort;
    config.version += 1;
    assert.commandWorked(rs.getPrimary().adminCommand({replSetReconfig: config}));
    if (i == numNodes - 1) {
        assert.commandWorked(rs.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    } else {
        assert.commandFailedWithCode(
            rs.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
            ErrorCodes.CannotDowngrade,
        );
    }
}

jsTest.log.info("Reset FCV to fully upgraded");
assert.commandWorked(rs.getPrimary().adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

jsTest.log.info("Test concurrent downgrade and reconfig");
let timeoutOFCVDrainFP = configureFailPoint(rs.getPrimary(), "immediatelyTimeOutWaitForStaleOFCV");
let reconfigFP = configureFailPoint(rs.getPrimary(), "hangBeforeNewConfigValidationChecks");

jsTest.log.info("Begin a reconfig and pause it midway");
let newConfig = rs.getReplSetConfigFromNode();
newConfig.version += 1;
const awaitReconfig = startParallelShell(
    funWithArgs(function (newConfig) {
        assert.commandWorked(db.adminCommand({replSetReconfig: newConfig}));
    }, newConfig),
    rs.getPrimary().port,
);
reconfigFP.wait();
jsTest.log.info("Downgrade the FCV, it should fail waiting for the reconfig to complete");
assert.commandFailedWithCode(
    rs.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    ErrorCodes.ExceededTimeLimit,
);

jsTest.log.info("Release the reconfig, it should succeed");
reconfigFP.off();
awaitReconfig();
timeoutOFCVDrainFP.off();

jsTest.log.info("Check that we cannot reconfig adding priority ports while we are in downgrading");
let membersWithPP = rs.getReplSetConfig().members;
newConfig = rs.getReplSetConfigFromNode();
newConfig.members = membersWithPP;
newConfig.version += 1;
assert.commandFailedWithCode(
    rs.getPrimary().adminCommand({replSetReconfig: newConfig}),
    ErrorCodes.NewReplicaSetConfigurationIncompatible,
);

jsTest.log.info("Reset FCV to fully upgraded");
assert.commandWorked(rs.getPrimary().adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

jsTest.log.info("Check that we cannot reconfig adding priority ports with force reconfig");
assert.commandFailedWithCode(
    rs.getPrimary().adminCommand({replSetReconfig: newConfig, force: true}),
    ErrorCodes.NewReplicaSetConfigurationIncompatible,
);

rs.stopSet();
