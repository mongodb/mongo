// test the lock/unlock snapshotting feature a bit

x=db.runCommand({fsync:1,lock:1}); // not on admin db
assert(!x.ok,"D");

x=db.fsyncLock(); // uses admin automatically

assert(x.ok,"C");

y = db.currentOp();
assert(y.fsyncLock,"B");

z = db.fsyncUnlock();

// it will take some time to unlock, and unlock does not block and wait for that
// doing a write will make us wait until db is writeable.
db.jstests_fsync.insert({x:1});

assert( db.currentOp().fsyncLock == null, "A" );

