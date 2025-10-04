// @tags: [requires_scripting]
import {ShardingTest} from "jstests/libs/shardingtest.js";

let s = new ShardingTest({shards: 2, mongos: 1});
assert.commandWorked(s.s0.adminCommand({enablesharding: "test", primaryShard: s.shard1.shardName}));

let db = s.getDB("test");

// ---- distinct ----

db.foo.save({x: 1});
db.foo.save({x: 2});
db.foo.save({x: 3});
db.foo.createIndex({x: 1});

let a = s.shard0.getDB("test");
let b = s.shard1.getDB("test");

assert.eq("1,2,3", db.foo.distinct("x"), "distinct 1");
assert(a.foo.distinct("x").length == 3 || b.foo.distinct("x").length == 3, "distinct 2");
assert(a.foo.distinct("x").length == 0 || b.foo.distinct("x").length == 0, "distinct 3");

assert.eq(1, s.onNumShards("test", "foo"), "A1");

s.shardColl("foo", {x: 1}, {x: 2}, {x: 3}, null, true /* waitForDelete */);

assert.eq(2, s.onNumShards("test", "foo"), "A2");

assert.eq("1,2,3", db.foo.distinct("x"), "distinct 4");

// ----- delete ---

assert.eq(3, db.foo.count(), "D1");

db.foo.remove({x: 3});
assert.eq(2, db.foo.count(), "D2");

db.foo.save({x: 3});
assert.eq(3, db.foo.count(), "D3");

db.foo.remove({x: {$gt: 2}});
assert.eq(2, db.foo.count(), "D4");

db.foo.remove({x: {$gt: -1}});
assert.eq(0, db.foo.count(), "D5");

db.foo.save({x: 1});
db.foo.save({x: 2});
db.foo.save({x: 3});
assert.eq(3, db.foo.count(), "D6");
db.foo.remove({});
assert.eq(0, db.foo.count(), "D7");

// --- _id key ---

db.foo2.save({_id: new ObjectId()});
db.foo2.save({_id: new ObjectId()});
db.foo2.save({_id: new ObjectId()});

assert.eq(1, s.onNumShards("test", "foo2"), "F1");

printjson(db.foo2.getIndexes());
s.adminCommand({shardcollection: "test.foo2", key: {_id: 1}});

assert.eq(3, db.foo2.count(), "F2");
db.foo2.insert({});
assert.eq(4, db.foo2.count(), "F3");

// --- map/reduce

db.mr.save({x: 1, tags: ["a", "b"]});
db.mr.save({x: 2, tags: ["b", "c"]});
db.mr.save({x: 3, tags: ["c", "a"]});
db.mr.save({x: 4, tags: ["b", "c"]});
db.mr.createIndex({x: 1});

let m = function () {
    this.tags.forEach(function (z) {
        emit(z, {count: 1});
    });
};

let r = function (key, values) {
    let total = 0;
    for (let i = 0; i < values.length; i++) {
        total += values[i].count;
    }
    return {count: total};
};

let doMR = function (n) {
    print(n);

    // on-disk

    var res = db.mr.mapReduce(m, r, "smr1_out");
    printjson(res);

    let x = db[res.result];
    assert.eq(3, x.find().count(), "MR T1 " + n);

    var z = {};
    x.find().forEach(function (a) {
        z[a._id] = a.value.count;
    });
    assert.eq(3, Object.keySet(z).length, "MR T2 " + n);
    assert.eq(2, z.a, "MR T3 " + n);
    assert.eq(3, z.b, "MR T4 " + n);
    assert.eq(3, z.c, "MR T5 " + n);

    x.drop();

    // inline

    var res = db.mr.mapReduce(m, r, {out: {inline: 1}});
    printjson(res);

    var z = {};
    res.results.forEach(function (a) {
        z[a._id] = a.value.count;
    });
    printjson(z);
    assert.eq(3, Object.keySet(z).length, "MR T7 " + n);
    assert.eq(2, z.a, "MR T8 " + n);
    assert.eq(3, z.b, "MR T9 " + n);
    assert.eq(3, z.c, "MR TA " + n);
};

doMR("before");

assert.eq(1, s.onNumShards("test", "mr"), "E1");
s.shardColl("mr", {x: 1}, {x: 2}, {x: 3}, null, true /* waitForDelete */);
assert.eq(2, s.onNumShards("test", "mr"), "E1");

doMR("after");

s.adminCommand({split: "test.mr", middle: {x: 3}});
s.adminCommand({split: "test.mr", middle: {x: 4}});
s.adminCommand({movechunk: "test.mr", find: {x: 3}, to: s.getPrimaryShard("test").name});

doMR("after extra split");

let cmd = {mapreduce: "mr", map: "emit( ", reduce: "fooz + ", out: "broken1"};

let x = db.runCommand(cmd);
let y = s.shard0.getDB("test").runCommand(cmd);

printjson(x);
printjson(y);

// count

db.countaa.save({"regex": /foo/i});
db.countaa.save({"regex": /foo/i});
db.countaa.save({"regex": /foo/i});
assert.eq(3, db.countaa.count(), "counta1");
assert.eq(3, db.countaa.find().itcount(), "counta1");

let hello = db.runCommand({hello: 1});
assert(hello.isWritablePrimary);
assert.eq("isdbgrid", hello.msg);

s.stop();
