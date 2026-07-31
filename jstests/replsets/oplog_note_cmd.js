// Test that the "appendOplogNote" command works properly

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rs = new ReplSetTest({name: "oplogNoteTest", nodes: 1});
rs.startSet();
rs.initiate();

let primary = rs.getPrimary();
var db = primary.getDB("admin");
db.foo.insert({a: 1});

// Make sure "optime" field gets updated. Status optimes may lag the write on disagg, so poll
// instead of asserting immediately.
let statusBefore = db.runCommand({replSetGetStatus: 1});
assert.commandWorked(db.runCommand({appendOplogNote: 1, data: {a: 1}}));
assert.soon(
    () => {
        const statusAfter = db.runCommand({replSetGetStatus: 1});
        return (
            timestampCmp(statusBefore.members[0].optime.ts, statusAfter.members[0].optime.ts) < 0
        );
    },
    "optime did not advance after appendOplogNote",
    60 * 1000,
);

// Make sure note written successfully. On disagg the note is not necessarily the newest oplog
// entry, so query for it instead of reading the latest entry.
assert.soon(
    () => db.getSiblingDB("local").oplog.rs.findOne({op: "n", "o.a": 1}) !== null,
    "oplog note entry not found in the oplog",
    60 * 1000,
);

rs.stopSet();
