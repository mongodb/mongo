// test the lock/unlock snapshotting feature a bit

x=db.runCommand({fsync:1,lock:1}); // not on admin db
assert(!x.ok,"D");

x=db.fsyncLock(); // uses admin automatically

assert(x.ok,"C");

y = db.currentOp();
assert(y.fsyncLock,"B");

z = db.fsyncUnlock();
assert( db.currentOp().fsyncLock == null, "A2" );

// make sure the db is unlocked
db.jstests_fsync.insert({x:1});
db.getLastError();

assert( db.currentOp().fsyncLock == null, "A" );

