//
// Testing mongos with replica sets:
//  - config server should update when replica set config changes.
//  - test insert and find commands are routed appropriately to shard primaries
//    and secondaries.
//
// This test involves using fsync to lock the secondaries, so cannot be run on
// storage engines which do not support the command.
// @tags: [
//    requires_fsync,
// ]

(function() {
'use strict';

// The mongod secondaries are set to priority 0 to prevent the primaries from stepping down during
// migrations on slow evergreen builders.
var s = new ShardingTest({
    shards: 2,
    other: {
        chunkSize: 1,
        rs0: {
            nodes: [
                {},
                {rsConfig: {priority: 0}},
            ],
        },
        rs1: {
            nodes: [
                {},
                {rsConfig: {priority: 0}},
            ],
        }
    }
});

var db = s.getDB("test");
var t = db.foo;

assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
// The default WC is 'majority' and fsyncLock will prevent satisfying any majority writes.
// The default RC is 'local' but fsync will block any refresh on secondaries, that's why RC is
// defaulted to 'available'.
assert.commandWorked(s.s.adminCommand({
    setDefaultRWConcern: 1,
    defaultReadConcern: {level: "available"},
    defaultWriteConcern: {w: 1},
    writeConcern: {w: "majority"}
}));
s.ensurePrimaryShard('test', s.shard0.shardName);

// -------------------------------------------------------------------------------------------
// ---------- test that config server updates when replica set config changes ----------------
// -------------------------------------------------------------------------------------------

db.foo.save({_id: 5, x: 17});
assert.eq(1, db.foo.count());

s.config.databases.find().forEach(printjson);
s.config.shards.find().forEach(printjson);

function countNodes() {
    return s.config.shards.findOne({_id: s.shard0.shardName}).host.split(",").length;
}

assert.eq(2, countNodes(), "A1");

var rs = s.rs0;
if (!TestData.configShard) {
    rs.add({'shardsvr': ""});
} else {
    rs.add({'configsvr': ""});
}
try {
    rs.reInitiate();
} catch (e) {
    // this os ok as rs's may close connections on a change of primary
    print(e);
}

assert.soon(function() {
    try {
        printjson(rs.getPrimary().getDB("admin").runCommand("hello"));
        s.config.shards.find().forEach(printjsononeline);
        return countNodes() == 3;
    } catch (e) {
        print(e);
    }
}, "waiting for config server to update", 180 * 1000, 1000);

// cleanup after adding node
for (var i = 0; i < 5; i++) {
    try {
        db.foo.findOne();
    } catch (e) {
    }
}

jsTest.log(
    "Awaiting replication of all nodes, so spurious sync'ing queries don't upset our counts...");
rs.awaitReplication();
// Make sure we wait for secondaries here - otherwise a secondary could come online later and be
// used for the
// count command before being fully replicated
jsTest.log("Awaiting secondary status of all nodes");
rs.waitForState(rs.getSecondaries(), ReplSetTest.State.SECONDARY, 180 * 1000);

// -------------------------------------------------------------------------------------------
// ---------- test routing to secondaries ----------------
// -------------------------------------------------------------------------------------------

// --- not sharded ----

var m = new Mongo(s.s.name);
var ts = m.getDB("test").foo;

var before = rs.getPrimary().adminCommand("serverStatus").opcounters;

for (var i = 0; i < 10; i++) {
    assert.eq(17, ts.findOne().x, "B1");
}

m.setSecondaryOk();

for (var i = 0; i < 10; i++) {
    assert.eq(17, ts.findOne().x, "B2");
}

var after = rs.getPrimary().adminCommand("serverStatus").opcounters;

printjson(before);
printjson(after);

assert.lte(before.query + 10, after.query, "B3");

// --- add more data ----

db.foo.createIndex({x: 1});

var bulk = db.foo.initializeUnorderedBulkOp();
for (var i = 0; i < 100; i++) {
    if (i == 17)
        continue;
    bulk.insert({x: i});
}
assert.commandWorked(bulk.execute({w: 3}));

// Counts pass the options of the connection - which is secondaryOk'd, so we need to wait for
// replication for this and future tests to pass
rs.awaitReplication();

assert.eq(100, ts.count(), "B4");
assert.eq(100, ts.find().itcount(), "B5");
assert.eq(100, ts.find().batchSize(5).itcount(), "B6");

var cursor = t.find().batchSize(3);
cursor.next();
cursor.close();

// --- sharded ----

assert.eq(100, db.foo.count(), "C1");

assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {x: 1}}));

// We're doing some manual chunk stuff, so stop the balancer first
s.stopBalancer();

assert.eq(100, t.count(), "C2");
assert.commandWorked(s.s0.adminCommand({split: "test.foo", middle: {x: 50}}));

s.printShardingStatus();

var other = s.config.shards.findOne({_id: {$ne: s.shard0.shardName}});
assert.commandWorked(s.getDB('admin').runCommand({
    moveChunk: "test.foo",
    find: {x: 10},
    to: other._id,
    _secondaryThrottle: true,
    writeConcern: {w: 2},
    _waitForDelete: true
}));
rs.awaitReplication();
assert.eq(100, t.count(), "C3");

assert.eq(50, rs.getPrimary().getDB("test").foo.count(), "C4");

// by non-shard key

m = new Mongo(s.s.name);
ts = m.getDB("test").foo;

before = rs.getPrimary().adminCommand("serverStatus").opcounters;

for (var i = 0; i < 10; i++) {
    assert.eq(17, ts.findOne({_id: 5}).x, "D1");
}

m.setSecondaryOk();
for (var i = 0; i < 10; i++) {
    assert.eq(17, ts.findOne({_id: 5}).x, "D2");
}

after = rs.getPrimary().adminCommand("serverStatus").opcounters;

assert.lte(before.query + 10, after.query, "D3");

// by shard key

m = new Mongo(s.s.name);

s.printShardingStatus();

ts = m.getDB("test").foo;

before = rs.getPrimary().adminCommand("serverStatus").opcounters;

for (var i = 0; i < 10; i++) {
    assert.eq(57, ts.findOne({x: 57}).x, "E1");
}

m.setSecondaryOk();
for (var i = 0; i < 10; i++) {
    assert.eq(57, ts.findOne({x: 57}).x, "E2");
}

after = rs.getPrimary().adminCommand("serverStatus").opcounters;

assert.lte(before.query + 10, after.query, "E3");

rs.awaitReplication();
assert.eq(100, ts.count(), "E4");
assert.eq(100, ts.find().itcount(), "E5");
printjson(ts.find().batchSize(5).explain());

// fsyncLock the secondaries and enable command logging on all the mongods.
assert.commandWorked(
    rs.getPrimary().adminCommand({setParameter: 1, logComponentVerbosity: {command: 2}}));
rs.getSecondaries().forEach(function(secondary) {
    assert.commandWorked(
        secondary.adminCommand({setParameter: 1, logComponentVerbosity: {command: 2}}));
    assert.commandWorked(secondary.getDB("test").fsyncLock());
});

// Modify data only on the primary replica of the primary shard.
// { x: 60 } goes to the shard of "rs", which is the primary shard.
assert.commandWorked(ts.insert({primaryOnly: true, x: 60}));
// Read from secondary through mongos, the doc is not there due to replication delay or fsync.
// But we can guarantee not to read from primary.
assert.eq(0, ts.find({primaryOnly: true, x: 60}).itcount());
// Unlock the secondaries
rs.getSecondaries().forEach(function(secondary) {
    secondary.getDB("test").fsyncUnlock();
});
// Clean up the data
assert.commandWorked(ts.remove({primaryOnly: true, x: 60}, {writeConcern: {w: 3}}));

rs.awaitReplication();
for (var i = 0; i < 10; i++) {
    m = new Mongo(s.s.name);
    m.setSecondaryOk();
    ts = m.getDB("test").foo;
    assert.eq(100, ts.find().batchSize(5).itcount(), "F2." + i);
}

for (var i = 0; i < 10; i++) {
    m = new Mongo(s.s.name);
    ts = m.getDB("test").foo;
    assert.eq(100, ts.find().batchSize(5).itcount(), "F3." + i);
}

printjson(db.adminCommand("getShardMap"));

s.stop();
})();
