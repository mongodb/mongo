/*
 * An arbiter that is stopped and restarted on a different port and rejoins the
 * replica set should enter removed state and should not start data replication.
 *
 * Skip this test in multiversion suites; the arbiter must restart using the same binary version.
 *
 * @tags: [
 *     multiversion_incompatible,
 * ]
 */
(function() {
"use strict";
const replTest = new ReplSetTest({name: 'test', nodes: 3});
replTest.startSet();
const nodes = replTest.nodeList();
let config = {
    "_id": "test",
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2], arbiterOnly: true}
    ]
};
replTest.initiate(config);

let primary = replTest.getPrimary();
replTest.awaitReplication();
replTest.awaitSecondaryNodes();

const arbiterId = 2;
const newPort = replTest.getPort(arbiterId) + 1;
jsTestLog("Restarting the arbiter node on a new port: " + newPort);
replTest.stop(arbiterId);
replTest.start(arbiterId, {port: newPort}, true);

jsTestLog("Reconfiguring the set to change the arbiter's port.");
config = replTest.getReplSetConfigFromNode();
jsTestLog(`Original config: ${tojson(config)}`);

const hostname = config.members[arbiterId].host.split(":")[0];
config.version++;
config.members[arbiterId].host = hostname + ":" + newPort;
jsTestLog(`New config: ${tojson(config)}`);
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: config}));
replTest.awaitReplication();
replTest.awaitNodesAgreeOnConfigVersion();
replTest.stopSet();
}());
