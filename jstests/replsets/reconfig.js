/*
 * Simple test to ensure that an invalid reconfig fails, a valid one succeeds, and a reconfig won't
 * succeed without force if force is needed.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isConfigCommitted} from "jstests/replsets/rslib.js";

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;

let numNodes = 5;
let replTest = new ReplSetTest({name: "testSet", nodes: numNodes});
let nodes = replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();

replTest.awaitSecondaryNodes();

jsTestLog("Valid reconfig");
let config = primary.getDB("local").system.replset.findOne();
printjson(config);
config.version++;
config.members[nodes.indexOf(primary)].priority = 2;
assert.commandWorked(primary.getDB("admin").runCommand({replSetReconfig: config}));
// Successful reconfig writes a no-op into the oplog.
const expectedNoOp = {
    op: "n",
    o: {msg: "Reconfig set", version: config.version},
};
const primaryOplog = primary.getDB("local")["oplog.rs"];
const lastOp = primaryOplog.find(expectedNoOp).sort({"$natural": -1}).limit(1).toArray();
assert(lastOp.length > 0);
replTest.awaitReplication();

// Make sure that all nodes have installed the config before moving on.
replTest.waitForConfigReplication(primary, nodes);
assert.soonNoExcept(() => isConfigCommitted(primary));

jsTestLog("Invalid reconfig");
config.version++;
let badMember = {_id: numNodes, host: "localhost:12345", priority: "High"};
config.members.push(badMember);
let invalidConfigCode = 93;
assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config}), invalidConfigCode);

jsTestLog("No force when needed.");
config.members = config.members.slice(0, numNodes - 1);
let secondary = replTest.getSecondary();
config.members[nodes.indexOf(secondary)].priority = 5;
let admin = secondary.getDB("admin");
let forceRequiredCode = 10107;
assert.commandFailedWithCode(admin.runCommand({replSetReconfig: config}), forceRequiredCode);

jsTestLog("Force when appropriate");
assert.commandWorked(admin.runCommand({replSetReconfig: config, force: true}));

// Wait for the last node to know it is REMOVED before stopping the test.
jsTestLog("Waiting for the last node to be REMOVED.");
assert.soonNoExcept(() => {
    assert.commandFailedWithCode(nodes[4].adminCommand({"replSetGetStatus": 1}), ErrorCodes.InvalidReplicaSetConfig);
    return true;
});
jsTestLog("Finished waiting for the last node to be REMOVED.");

replTest.stopSet();
