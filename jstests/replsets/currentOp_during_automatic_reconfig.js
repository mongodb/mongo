/**
 * Tests that currentOp displays information about in-progress automatic reconfigs.
 *
 * @tags: [
 * ]
 */

import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isMemberNewlyAdded, waitForNewlyAddedRemovalForNodeToBeCommitted} from "jstests/replsets/rslib.js";

const testName = jsTestName();
const dbName = "testdb";
const collName = "testcoll";

const rst = new ReplSetTest({name: testName, nodes: [{}], settings: {chainingAllowed: false}, useBridge: true});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

assert.commandWorked(primaryColl.insert({"starting": "doc"}));

jsTestLog("Adding a new node to the replica set");
const secondary = rst.add({
    rsConfig: {priority: 0},
    setParameter: {
        "failpoint.initialSyncHangBeforeFinish": tojson({mode: "alwaysOn"}),
        "numInitialSyncAttempts": 1,
    },
});
rst.reInitiate();
assert.commandWorked(
    secondary.adminCommand({
        waitForFailPoint: "initialSyncHangBeforeFinish",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

jsTestLog("Checking that the 'newlyAdded' field is set on the new node");
assert(isMemberNewlyAdded(primary, 1));

jsTestLog("Allowing primary to initiate the 'newlyAdded' field removal");
let hangDuringAutomaticReconfigFP = configureFailPoint(primaryDb, "hangDuringAutomaticReconfig");
assert.commandWorked(secondary.adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}));
rst.awaitSecondaryNodes(null, [secondary]);

hangDuringAutomaticReconfigFP.wait();

jsTestLog("Looking for the automatic reconfig in the currentOp output");
const curOpRes = assert.commandWorked(primaryDb.adminCommand({currentOp: 1}));

const ops = curOpRes.inprog;
let found = false;
for (let i = 0; i < ops.length; i++) {
    let op = ops[i];
    assert(op.hasOwnProperty("command"), op);
    const commandField = op["command"];
    if (commandField.hasOwnProperty("replSetReconfig")) {
        if (commandField["replSetReconfig"] === "automatic") {
            assert(commandField.hasOwnProperty("configVersionAndTerm"));
            assert(commandField.hasOwnProperty("memberId"), op);
            assert.eq(1, commandField["memberId"], op);

            assert(op.hasOwnProperty("desc"), op);
            assert(op["desc"].startsWith("ReplCoord")); // client name

            jsTestLog("Found automatic reconfig: " + tojson(op));
            found = true;
            break;
        }
    }
}

assert(found, ops);

hangDuringAutomaticReconfigFP.off();
waitForNewlyAddedRemovalForNodeToBeCommitted(primary, 1);
rst.stopSet();
