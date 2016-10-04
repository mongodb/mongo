// Test that the "appendOplogNote" command works properly

var rs = new ReplSetTest({name: "oplogNoteTest", nodes: 1});
rs.startSet();
rs.initiate();

var primary = rs.getPrimary();
var db = primary.getDB('admin');
db.foo.insert({a: 1});

// Make sure "optime" field gets updated
var statusBefore = db.runCommand({replSetGetStatus: 1});
assert.commandWorked(db.runCommand({appendOplogNote: 1, data: {a: 1}}));
var statusAfter = db.runCommand({replSetGetStatus: 1});
if (rs.getReplSetConfigFromNode().protocolVersion != 1) {
    assert.lt(statusBefore.members[0].optime, statusAfter.members[0].optime);
} else {
    assert.lt(statusBefore.members[0].optime.ts, statusAfter.members[0].optime.ts);
}

// Make sure note written successfully
var op = db.getSiblingDB('local').oplog.rs.find().sort({$natural: -1}).limit(1).next();
assert.eq(1, op.o.a);

rs.stopSet();
