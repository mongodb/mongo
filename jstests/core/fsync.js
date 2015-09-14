// Tests the db.fsyncLock/fsyncUnlock features

// Start with a clean DB
var fsyncLockDB = db.getSisterDB('fsyncLockTestDB');
fsyncLockDB.dropDatabase();

// Test it doesn't work unless invoked against the admin DB
var resFail = fsyncLockDB.runCommand({fsync:1, lock:1});
assert(!resFail.ok, "fsyncLock command succeeded against DB other than admin.");

// Uses admin automatically and locks the server for writes
var fsyncLockRes = db.fsyncLock();
assert(fsyncLockRes.ok, "fsyncLock command failed against admin DB");
assert(db.currentOp().fsyncLock, "Value in db.currentOp incorrect for fsyncLocked server");

// Make sure writes are blocked. Spawn a write operation in a separate shell and make sure it
// is blocked. There is really now way to do that currently, so just check that the write didn't
// go through.
var writeOpHandle = startParallelShell("db.getSisterDB('fsyncLockTestDB').coll.insert({x:1});");
sleep(1000);

// Make sure reads can still run even though there is a pending write and also that the write
// didn't get through
assert.eq(0, fsyncLockDB.coll.count({}));

// Unlock and make sure the insert succeeded
var fsyncUnlockRes = db.fsyncUnlock();
assert(fsyncUnlockRes.ok, "fsyncUnlock command failed");
assert(db.currentOp().fsyncLock == null, "fsyncUnlock is not null in db.currentOp");

// Make sure the db is unlocked and the initial write made it through.
writeOpHandle();
fsyncLockDB.coll.insert({x:2});

assert.eq(2, fsyncLockDB.coll.count({}));

// Ensure eval is not allowed to invoke fsyncLock
assert(!db.eval('db.fsyncLock()').ok, "eval('db.fsyncLock()') should fail.");

// Check that the fsyncUnlock pseudo-command (a lookup on cmd.$sys.unlock)
// still has the same effect as a legitimate 'fsyncUnlock' command
// TODO: remove this in in the release following MongoDB 3.2 when pseudo-commands
// are removed
var fsyncCommandRes = db.fsyncLock();
assert(fsyncLockRes.ok, "fsyncLock command failed against admin DB");
assert(db.currentOp().fsyncLock, "Value in db.currentOp incorrect for fsyncLocked server");
var fsyncPseudoCommandRes = db.getSiblingDB("admin").$cmd.sys.unlock.findOne();
assert(fsyncPseudoCommandRes.ok, "fsyncUnlock pseudo-command failed");
assert(db.currentOp().fsyncLock == null, "fsyncUnlock is not null in db.currentOp");
