// test the lock/unlock snapshotting feature a bit

debug = function( t ) {
    print( t );
}

x=db.runCommand({fsync:1,lock:1});
assert(!x.ok,"D");

d=db.getSisterDB("admin");

x=d.runCommand({fsync:1,lock:1});

assert(x.ok,"C");

y = d.currentOp();
assert(y.fsyncLock,"B");

debug( "current op ok" );

printjson( db.foo.find().toArray() );
db.foo.insert({x:2});

debug( "done insert attempt" );

z = d.runCommand({unlock:1});

debug( "sent unlock" );

// it will take some time to unlock, and unlock does not block and wait for that
// doing a write will make us wait until db is writeable.
db.foo.insert({x:1});

debug( "done insert" );

assert( d.currentOp().fsyncLock == null, "A" );

