var replTest = new ReplSetTest( {name: 'reindexTest', nodes: 2} );

var nodes = replTest.startSet();

replTest.initiate();

var master = replTest.getMaster();
replTest.awaitSecondaryNodes()

var slaves = replTest.liveNodes.slaves;
assert( slaves.length == 1, "Expected 1 slave but length was " + slaves.length );
slave = slaves[0];

db = master.getDB("reindexTest");
slaveDb = slave.getDB("reindexTest");

// Setup index
db.foo.insert({a:1000});

db.foo.ensureIndex({a:1});

replTest.awaitReplication();

assert.eq(2, db.system.indexes.count(), "Master didn't have proper indexes before reindex");
assert.eq(2, slaveDb.system.indexes.count(), "Slave didn't have proper indexes before reindex");


// Try to reindex secondary
slaveDb.foo.reIndex();

assert.eq(2, db.system.indexes.count(), "Master didn't have proper indexes after reindex");
assert.eq(2, slaveDb.system.indexes.count(), "Slave didn't have proper indexes after reindex");

replTest.stopSet(15);