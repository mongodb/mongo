//
// Tests the use of the wOpTime option in getLastError
//
// This test requires fsync to lock the secondary, so cannot be run on storage engines which do not
// support the command.
// @tags: [requires_fsync]

var rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

var primary = rst.getPrimary();
var secondary = rst.getSecondary();

var coll = primary.getCollection("foo.bar");

// Insert a doc and replicate it to two servers
coll.insert({some: "doc"});
var gleObj = coll.getDB().getLastErrorObj(2);  // w : 2
assert.eq(null, gleObj.err);
var opTimeBeforeFailure = gleObj.lastOp;

// Lock the secondary
assert.commandWorked(secondary.getDB("admin").fsyncLock());

// Insert a doc and replicate it to the primary only
coll.insert({some: "doc"});
gleObj = coll.getDB().getLastErrorObj(1);  // w : 1
assert.eq(null, gleObj.err);
var opTimeAfterFailure = gleObj.lastOp;

printjson(opTimeBeforeFailure);
printjson(opTimeAfterFailure);
printjson(primary.getDB("admin").runCommand({replSetGetStatus: true}));

// Create a new connection with new client and no opTime
var newClientConn = new Mongo(primary.host);

// New client has no set opTime, so w : 2 has no impact
gleObj = newClientConn.getCollection(coll.toString()).getDB().getLastErrorObj(2);  // w : 2
assert.eq(null, gleObj.err);

// Using an explicit optime on the new client should work if the optime is earlier than the
// secondary was locked
var gleOpTimeBefore = {getLastError: true, w: 2, wOpTime: opTimeBeforeFailure};
gleObj = newClientConn.getCollection(coll.toString()).getDB().runCommand(gleOpTimeBefore);
assert.eq(null, gleObj.err);

// Using an explicit optime on the new client should not work if the optime is later than the
// secondary was locked
var gleOpTimeAfter = {getLastError: true, w: 2, wtimeout: 1000, wOpTime: opTimeAfterFailure};
gleObj = newClientConn.getCollection(coll.toString()).getDB().runCommand(gleOpTimeAfter);
assert.neq(null, gleObj.err);
assert(gleObj.wtimeout);

jsTest.log("DONE!");

// Unlock the secondary
secondary.getDB("admin").fsyncUnlock();
rst.stopSet();
