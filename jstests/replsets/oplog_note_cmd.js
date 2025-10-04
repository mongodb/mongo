// Test that the "appendOplogNote" command works properly

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rs = new ReplSetTest({name: "oplogNoteTest", nodes: 1});
rs.startSet();
rs.initiate();

let primary = rs.getPrimary();
var db = primary.getDB("admin");
db.foo.insert({a: 1});

// Make sure "optime" field gets updated
let statusBefore = db.runCommand({replSetGetStatus: 1});
assert.commandWorked(db.runCommand({appendOplogNote: 1, data: {a: 1}}));
let statusAfter = db.runCommand({replSetGetStatus: 1});
assert.lt(statusBefore.members[0].optime.ts, statusAfter.members[0].optime.ts);

// Make sure note written successfully
let op = db.getSiblingDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next();
assert.eq(1, op.o.a);

rs.stopSet();
