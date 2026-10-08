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
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {waitForState} from "jstests/replsets/rslib.js";

const replTest = new ReplSetTest({name: "test", nodes: 3});
replTest.startSet();
const nodes = replTest.nodeList();
let config = replTest.getReplSetConfig();
config.members[2].arbiterOnly = true;
replTest.initiate(config);

let primary = replTest.getPrimary();
replTest.awaitReplication();
replTest.awaitSecondaryNodes();

const arbiterId = 2;
const newPort = allocatePort();
jsTestLog("Restarting the arbiter node on a new port: " + newPort);
replTest.stop(arbiterId);
replTest.start(arbiterId, {port: newPort}, true);

// A node that is not a member of the current config reports REMOVED state by rejecting
// replSetGetStatus with InvalidReplicaSetConfig (see reconfig.js for the same pattern).
// The in-memory storage engine does not persist the config across the restart, so it enters
// NotYetInitialized instead. WiredTiger reports NotYetInitialized only transiently at startup
// before loading the stored config, so it must not be accepted there: the REMOVED transition
// closes all connections (PostMemberStateUpdateAction::kActionRollbackOrRemoved), which would
// race with the oplog check below.
jsTestLog("Verifying the arbiter enters the REMOVED state after rejoining on the new port.");
const arbiter = replTest.nodes[arbiterId];
arbiter.setSecondaryOk();
const isInMemory = jsTest.options().storageEngine == "inMemory";
const expectedRemovedCode = isInMemory
    ? ErrorCodes.NotYetInitialized
    : ErrorCodes.InvalidReplicaSetConfig;
assert.soonNoExcept(() => {
    assert.commandFailedWithCode(
        arbiter.getDB("admin").runCommand({replSetGetStatus: 1}),
        expectedRemovedCode,
        "arbiter should be in the REMOVED state",
    );
    return true;
}, "arbiter did not enter the REMOVED state after rejoining on a new port");

// The arbiter must not start data replication while in the REMOVED state. Arbiters never hold
// data, so an oplog appearing in its local database would mean it started replicating.
assert(
    !arbiter.getDB("local").getCollectionNames().includes("oplog.rs"),
    "arbiter must not create an oplog and start data replication while in the REMOVED state",
);

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

jsTestLog("Verifying the arbiter is back to ARBITER state after the reconfig.");
waitForState(
    arbiter,
    ReplSetTest.State.ARBITER,
    "arbiter did not return to the ARBITER state after the reconfig",
);

replTest.stopSet();
