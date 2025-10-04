// Runner that runs full validation on all collections of the initial sync node and checks the
// dbhashes of all of the nodes including the initial sync node.
import {ReplSetTest} from "jstests/libs/replsettest.js";

let startTime = Date.now();

let primaryInfo = null;
assert.soonRetryOnNetworkErrors(
    () => {
        primaryInfo = db.isMaster();
        return primaryInfo.hasOwnProperty("ismaster") && primaryInfo.ismaster;
    },
    () => {
        return `shell is not connected to the primary node: ${tojson(primaryInfo)}`;
    },
);

// The initial sync hooks only work for replica sets.
let rst = new ReplSetTest(db.getMongo().host);

// Call getPrimary to populate rst with information about the nodes.
let primary = rst.getPrimary();
assert(primary, "calling getPrimary() failed");

// Find the hidden node.
let hiddenNode;
for (let secondary of rst.getSecondaries()) {
    let isMasterRes = secondary.getDB("admin").isMaster();
    if (isMasterRes.hidden) {
        hiddenNode = secondary;
        break;
    }
}

assert(hiddenNode, "No hidden initial sync node was found in the replica set");

// Confirm that the hidden node is in SECONDARY state.
let res;
assert.soonRetryOnNetworkErrors(
    () => {
        res = assert.commandWorked(hiddenNode.adminCommand({replSetGetStatus: 1}));
        return res.myState === ReplSetTest.State.SECONDARY;
    },
    () => {
        return `res: ${tojson(res)}`;
    },
);

/* The checkReplicatedDataHashes call waits until all operations have replicated to and
   have been applied on the secondaries, so we run the validation script after it
   to ensure we're validating the entire contents of the collection */

// For checkDBHashes
const excludedDBs = jsTest.options().excludedDBsFromDBHash;
rst.checkReplicatedDataHashes(undefined, excludedDBs);

await import("jstests/hooks/run_validate_collections.js");

let totalTime = Date.now() - startTime;
print("Finished consistency checks of initial sync node in " + totalTime + " ms.");
