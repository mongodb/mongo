/**
 * Tests that rolling back the insertion of the shardIdentity document on a shard causes the node
 * rolling it back to shut down.
 * @tags: [multiversion_incompatible, requires_persistence]
 */

// This test triggers an unclean shutdown (an fassert), which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

import {stopServerReplication, restartServerReplication} from "jstests/libs/write_concern_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1});

let replTest = new ReplSetTest({nodes: 3});
replTest.startSet({shardsvr: ""});
replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

let priConn = replTest.getPrimary();
let secondaries = replTest.getSecondaries();
let configConnStr = st.configRS.getURL();

// Wait for the secondaries to have the latest oplog entries before stopping the fetcher to
// avoid the situation where one of the secondaries will not have an overlapping oplog with
// the other nodes once the primary is killed.
replTest.awaitSecondaryNodes();

replTest.awaitReplication();

stopServerReplication(secondaries);

jsTest.log("inserting shardIdentity document to primary that shouldn't replicate");

let shardIdentityDoc = {
    _id: "shardIdentity",
    configsvrConnectionString: configConnStr,
    shardName: "newShard",
    clusterId: ObjectId(),
};

assert.commandWorked(
    priConn.getDB("admin").system.version.update({_id: "shardIdentity"}, shardIdentityDoc, {upsert: true}),
);

// Ensure sharding state on the primary was initialized
let res = priConn.getDB("admin").runCommand({shardingState: 1});
assert(res.enabled, tojson(res));
assert.eq(shardIdentityDoc.shardName, res.shardName);
assert.eq(shardIdentityDoc.clusterId, res.clusterId);
assert.soon(() => shardIdentityDoc.configsvrConnectionString == priConn.adminCommand({shardingState: 1}).configServer);

// Ensure sharding state on the secondaries was *not* initialized
secondaries.forEach(function (secondary) {
    secondary.setSecondaryOk();
    res = secondary.getDB("admin").runCommand({shardingState: 1});
    assert(!res.enabled, tojson(res));
});

// Ensure manually deleting the shardIdentity document is not allowed.
assert.writeErrorWithCode(priConn.getDB("admin").system.version.remove({_id: "shardIdentity"}), 40070);

jsTest.log("shutting down primary");
// Shut down the primary so a secondary gets elected that definitely won't have replicated the
// shardIdentity insert, which should trigger a rollback on the original primary when it comes
// back online.
replTest.stop(priConn);

// Disable the fail point so that the elected node can exit drain mode and finish becoming
// primary.
restartServerReplication(secondaries);

// Wait for a new healthy primary
let newPriConn = replTest.getPrimary();
assert.neq(priConn, newPriConn);
assert.commandWorked(newPriConn.getDB("test").foo.insert({a: 1}, {writeConcern: {w: "majority"}}));

// Restart the original primary so it triggers a rollback of the shardIdentity insert. Pass
// {waitForConnect : false} to avoid a race condition between the node crashing (which we expect)
// and waiting to be able to connect to the node.
jsTest.log("Restarting original primary");
priConn = replTest.start(priConn, {waitForConnect: false}, true);

// Wait until we cannot create a connection to the former primary, which indicates that it must
// have shut itself down during the rollback.
jsTest.log("Waiting for original primary to rollback and shut down");
// Wait until the node shuts itself down during the rollback. We will hit the first assertion if
// we rollback using 'recoverToStableTimestamp'.
assert.soon(() => {
    return rawMongoProgramOutput("Fatal assertion").search(/(40498|50712)/) !== -1;
});

// Restart the original primary again.  This time, the shardIdentity document should already be
// rolled back, so there shouldn't be any rollback and the node should stay online.
jsTest.log("Restarting original primary a second time and waiting for it to successfully become " + "secondary");
try {
    // Join() with the crashed mongod and ignore its bad exit status.
    MongoRunner.stopMongod(priConn);
} catch (e) {
    // expected
}
// Since we pass "restart: true" here, the node will start with the same options as above unless
// specified. We do want to wait to be able to connect to the node here however, so we need to pass
// {waitForConnect: true}.
priConn = replTest.start(priConn.nodeId, {shardsvr: "", waitForConnect: true}, true);
priConn.setSecondaryOk();

// Wait for the old primary to replicate the document that was written to the new primary while
// it was shut down.
assert.soonNoExcept(function () {
    return priConn.getDB("test").foo.findOne();
});

// Ensure that there's no sharding state on the restarted original primary, since the
// shardIdentity doc should have been rolled back.
res = priConn.getDB("admin").runCommand({shardingState: 1});
assert(!res.enabled, tojson(res));
assert.eq(null, priConn.getDB("admin").system.version.findOne({_id: "shardIdentity"}));

replTest.stopSet();

st.stop();
