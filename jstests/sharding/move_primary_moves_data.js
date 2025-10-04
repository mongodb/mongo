import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let s = new ShardingTest({shards: 2});

assert.commandWorked(s.getDB("test1").runCommand({dropDatabase: 1}));
var db = s.getDB("test1");
let c = db.foo;
c.save({a: 1});
c.save({a: 2});
c.save({a: 3});
assert.eq(3, c.count());

assert.commandWorked(db.runCommand({create: "view", viewOn: "foo", pipeline: [{$match: {a: 3}}]}));

let fromShard = s.getPrimaryShard("test1");
let toShard = s.getOther(fromShard);

assert.eq(3, fromShard.getDB("test1").foo.count(), "from doesn't have data before move");
assert.eq(0, toShard.getDB("test1").foo.count(), "to has data before move");
assert.eq(1, s.s.getDB("test1").view.count(), "count on view incorrect before move");

s.printShardingStatus();
assert.eq(
    s.normalize(s.config.databases.findOne({_id: "test1"}).primary),
    s.normalize(fromShard.name),
    "not in db correctly to start",
);

let oldShardName = s.config.databases.findOne({_id: "test1"}).primary;

assert.commandWorked(s.s0.adminCommand({movePrimary: "test1", to: toShard.name}));
s.printShardingStatus();
assert.eq(
    s.normalize(s.config.databases.findOne({_id: "test1"}).primary),
    s.normalize(toShard.name),
    "to in config db didn't change after first move",
);

// Collection data should only be moved if the collection is untracked.
if (FixtureHelpers.isTracked(s.s.getDB("test1").getCollection("foo"))) {
    assert.eq(3, fromShard.getDB("test1").foo.count(), "from doesn't have data after move");
    assert.eq(0, toShard.getDB("test1").foo.count(), "to has data after move");
    assert.eq(1, s.s.getDB("test1").view.count(), "count on view incorrect after move");
} else {
    assert.eq(0, fromShard.getDB("test1").foo.count(), "from still has data after move");
    assert.eq(3, toShard.getDB("test1").foo.count(), "to doesn't have data after move");
    assert.eq(1, s.s.getDB("test1").view.count(), "count on view incorrect after move");
}

// Move back, now using shard name instead of server address
assert.commandWorked(s.s0.adminCommand({movePrimary: "test1", to: oldShardName}));
s.printShardingStatus();
assert.eq(
    s.normalize(s.config.databases.findOne({_id: "test1"}).primary),
    oldShardName,
    "to in config db didn't change after second move",
);

assert.eq(3, fromShard.getDB("test1").foo.count(), "from doesn't have data after move back");
assert.eq(0, toShard.getDB("test1").foo.count(), "to has data after move back");
assert.eq(1, s.s.getDB("test1").view.count(), "count on view incorrect after move back");

s.stop();
