// Test the populating lastOpVisible field of the ReplSetMetadata.
// First we do a writeConcern-free write and ensure that a local read will return the same
// lastOpVisible, and that majority read with afterOpTime of lastOpVisible will return it as well.
// We then confirm that a writeConcern majority write will be seen as the lastVisibleOp by a
// majority read.
// @tags: [requires_majority_read_concern]
import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "lastOpVisible";
let replTest = new ReplSetTest({name: name, nodes: 3, waitForKeys: true});
replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();

// Do an insert without writeConcern.
let res = primary.getDB(name).runCommand({insert: name, documents: [{x: 1}], $replData: 1});
assert.commandWorked(res);
let last_op_visible = res["$replData"].lastOpVisible;

// A find should return the same lastVisibleOp.
res = primary.getDB(name).runCommand({find: name, readConcern: {level: "local"}, $replData: 1});
assert.commandWorked(res);
assert.eq(last_op_visible, res["$replData"].lastOpVisible);

// A majority readConcern with afterOpTime: lastOpVisible should also return the same
// lastVisibleOp.
res = primary
    .getDB(name)
    .runCommand({find: name, readConcern: {level: "majority", afterOpTime: last_op_visible}, $replData: 1});
assert.commandWorked(res);
assert.eq(last_op_visible, res["$replData"].lastOpVisible);

// Do an insert without writeConcern.
res = primary.getDB(name).runCommand({insert: name, documents: [{x: 1}], writeConcern: {w: "majority"}, $replData: 1});
assert.commandWorked(res);
last_op_visible = res["$replData"].lastOpVisible;

// A majority readConcern should return the same lastVisibleOp.
res = primary.getDB(name).runCommand({find: name, readConcern: {level: "majority"}, $replData: 1});
assert.commandWorked(res);
assert.eq(last_op_visible, res["$replData"].lastOpVisible);
replTest.stopSet();
