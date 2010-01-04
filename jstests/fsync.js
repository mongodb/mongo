// test the lock/unlock snapshotting feature a bit

x=db.runCommand({fsync:1,lock:1});
assert(!x.ok,"D");

d=db.getSisterDB("admin");

x=d.runCommand({fsync:1,lock:1});

assert(x.ok,"C");

y = d.currentOp();
assert(y.fsyncLock,"B");

z = d.$cmd.sys.unlock.findOne();

// it will take some time to unlock, and unlock does not block and wait for that
// doing a write will make us wait until db is writeable.
db.foo.insert({x:1});

assert( d.currentOp().fsyncLock == null, "A" );

