/*
 * When a one-node set is restarted on a different port and reconfigured with the new port, it
 * should re-elect itself.
 *
 * @tags: [
 *     requires_persistence,
 * ]
 */
(function() {
"use strict";

load("jstests/replsets/rslib.js");

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();
replTest.getPrimary();

/*
 * Prepare to restart the sole node on a new port, it no longer finds itself in the old config.
 */
const config = replTest.getReplSetConfigFromNode(0);
const newPort = replTest.getPort(0) + 1;
const hostname = config.members[0].host.split(":")[0];
const newHostAndPort = `${hostname}:${newPort}`;

jsTestLog("Restarting the sole node on a new port: " + newPort);
replTest.restart(0, {port: newPort});
let restartedNode;
assert.soonNoExcept(() => {
    restartedNode = new Mongo(newHostAndPort);
    return true;
}, `Couldn't connect to restarted node "${newHostAndPort}`);
waitForState(restartedNode, ReplSetTest.State.REMOVED);

/*
 * Update the config to match the node's new port.
 */
jsTestLog("Reconfiguring the set to change the sole node's port.");
jsTestLog(`Original config: ${tojson(config)}`);
config.version++;
config.members[0].host = newHostAndPort;
jsTestLog(`New config: ${tojson(config)}`);

// Force reconfig since the restarted node is in REMOVED state, not PRIMARY.
// The connection to the mongod may have been closed after reaching the REMOVED state. In case of a
// network error, retry the command until it succeeds.
assert.soonNoExcept(() => {
    assert.commandWorked(
        restartedNode.getDB("admin").runCommand({replSetReconfig: config, force: true}));
    return true;
}, `Couldn't run 'replSetReconfig' with config ${config} on the node ${newHostAndPort}`);
waitForState(restartedNode, ReplSetTest.State.PRIMARY);

replTest.stopSet();
}());
