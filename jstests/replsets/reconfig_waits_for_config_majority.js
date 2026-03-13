/**
 * Ensure that the reconfig command does not return success unless a majority
 * of voting nodes in a new config C have replicated C.
 *
 * Also ensure that config commitment status reports false if C has not been
 * majority replicated.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isConfigCommitted} from "jstests/replsets/rslib.js";

function ConfigCommittmentTest(nodes, numBlockedNodes) {
    jsTest.log.info(`Testing with ${nodes} nodes`);

    const rst = new ReplSetTest({nodes: nodes});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondaries = rst.getSecondaries();

    for (let i = 0; i < numBlockedNodes; i++) {
        configureFailPoint(secondaries[i], "blockHeartbeatReconfigFinish");
    }

    let config = primary.getDB("local").system.replset.findOne();
    config.version++;
    assert.commandFailedWithCode(
        primary.getDB("admin").runCommand({replSetReconfig: config, maxTimeMS: 5000}),
        ErrorCodes.MaxTimeMSExpired,
    );
    assert.eq(isConfigCommitted(primary), false);

    rst.stopSet();
}

ConfigCommittmentTest(3, 2 /* numBlockedNodes */);
// A normal 4 node set requires 3 nodes to replicate the config, so test that 2 nodes
// are not enough.
ConfigCommittmentTest(4, 2 /* numBlockedNodes */);
ConfigCommittmentTest(5, 3 /* numBlockedNodes */);
