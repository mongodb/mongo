// Enable failpoint

// The `allocateDiskFull` failpoint is mmap only.
// @tags: [requires_mmapv1]
assert.commandWorked(db.adminCommand({configureFailPoint: "allocateDiskFull", mode: "alwaysOn"}));

var d = db.getSisterDB("DiskFullTestDB");
var c = d.getCollection("DiskFullTestCollection");

var writeError1 = c.insert({a: 6}).getWriteError();
assert.eq(12520, writeError1.code);

// All subsequent requests should fail
var writeError2 = c.insert({a: 6}).getWriteError();
assert.eq(12520, writeError2.code);
