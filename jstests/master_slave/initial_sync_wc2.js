/**
 * This test ensures that the w:2 write correctly waits until initial sync is done.
 * Before SERVER-25618 the w:2 write could return during initial sync since the slave reported
 * progress during initial sync.
 */
var rt = new ReplTest("initial_sync_wc2");

// The database name needs to be at the top of the list or at least before the "test" db.
var dbToCloneName = "a_toclone";

// Start the master and insert some data to ensure that the slave has to clone a database.
var master = rt.start(true);
assert.writeOK(master.getDB(dbToCloneName).mycoll.insert({a: 1}));
assert.eq(1, master.getDB(dbToCloneName).mycoll.find({}).itcount());

// Start the slave.
var slave = rt.start(false);

// Perform a w=2 write to ensure that slave can be read from, and initial sync is complete.
assert.writeOK(master.getDB("test").mycoll.insert({}, {writeConcern: {w: 2}}));
assert.eq(1, slave.getDB("test").mycoll.find({}).itcount());