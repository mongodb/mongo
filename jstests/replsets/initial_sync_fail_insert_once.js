/**
 * Tests that initial sync can complete after a failed insert to a cloned collection.
 * The failpoint may fail once or twice depending on the order of the results of listCollection,
 * so we allow initial sync to retry 3 times.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "initial_sync_fail_insert_once";
let replSet = new ReplSetTest({name: name, nodes: 2, nodeOptions: {setParameter: "numInitialSyncAttempts=3"}});

replSet.startSet();
replSet.initiate();
let primary = replSet.getPrimary();
let secondary = replSet.getSecondary();

let coll = primary.getDB("test").getCollection(name);
assert.commandWorked(coll.insert({_id: 0, x: 1}, {writeConcern: {w: 2}}));

jsTest.log("Re-syncing " + tojson(secondary));
const params = {
    "failpoint.failCollectionInserts": tojson({mode: {times: 2}, data: {collectionNS: coll.getFullName()}}),
};

secondary = replSet.restart(secondary, {startClean: true, setParameter: params});
replSet.awaitReplication();
replSet.awaitSecondaryNodes();

assert.eq(1, secondary.getDB("test")[name].count());
assert.docEq({_id: 0, x: 1}, secondary.getDB("test")[name].findOne());

jsTest.log("Stopping repl set test; finished.");
replSet.stopSet();
