// Term counter should be present in oplog entries under protocol version 1.
import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "oplog_term";
let replSet = new ReplSetTest({name: name, nodes: 1});
replSet.startSet();
replSet.initiate();
replSet.waitForState(replSet.nodes[0], ReplSetTest.State.PRIMARY, 5 * 1000);

// Default protocol version is 1 - 'term' field should present in oplog entry.
let primary = replSet.getPrimary();
let collection = primary.getDB("test").getCollection(name);
assert.commandWorked(collection.save({_id: 1}));

// Look up this test's own insert rather than the oplog tip: unrelated background writes (HMAC key
// generation, query analysis setup) can land after the insert and would otherwise make the tip
// refer to an entry this test never made.
let oplogEntry = replSet
    .findOplog(primary, {op: "i", ns: collection.getFullName(), "o._id": 1}, 1)
    .toArray()[0];
assert(oplogEntry, "could not find the oplog entry for the inserted document");
assert(oplogEntry.hasOwnProperty("t"), "oplog entry must contain term: " + tojson(oplogEntry));

let status = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
assert.eq(
    status.term,
    oplogEntry.t,
    "term in oplog entry does not match term in status: " + tojson(oplogEntry),
);

replSet.stopSet();
