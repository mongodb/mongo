// Include helpers for analyzing explain output.
import {getChunkSkipsFromAllShards} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const s = new ShardingTest({name: "shard3", shards: 2, mongos: 2, other: {enableBalancer: true}});
const s2 = s.s1;

assert.commandWorked(s.s0.adminCommand({enablesharding: "test", primaryShard: s.shard1.shardName}));
assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {num: 1}}));

s.configRS.awaitLastOpCommitted();

assert(s.getBalancerState(), "A1");

s.stopBalancer();
assert(!s.getBalancerState(), "A2");

s.startBalancer();
assert(s.getBalancerState(), "A3");

s.stopBalancer();
assert(!s.getBalancerState(), "A4");

s.config.databases.find().forEach(printjson);

const a = s.getDB("test").foo;
const b = s2.getDB("test").foo;

const primary = s.getPrimaryShard("test").getDB("test").foo;
const secondary = s.getOther(primary.name).getDB("test").foo;

a.save({num: 1});
a.save({num: 2});
a.save({num: 3});

assert.eq(3, a.find().toArray().length, "normal A");
assert.eq(3, b.find().toArray().length, "other A");

assert.eq(3, primary.count(), "p1");
assert.eq(0, secondary.count(), "s1");

assert.eq(1, s.onNumShards("test", "foo"), "on 1 shards");

assert.commandWorked(s.s0.adminCommand({split: "test.foo", middle: {num: 2}}));
assert.commandWorked(s.s0.adminCommand({
    movechunk: "test.foo",
    find: {num: 3},
    to: s.getOther(s.getPrimaryShard("test")).name,
    _waitForDelete: true
}));

assert(primary.find().toArray().length > 0, "blah 1");
assert(secondary.find().toArray().length > 0, "blah 2");
assert.eq(3, primary.find().itcount() + secondary.find().itcount(), "blah 3");

assert.eq(3, a.find().toArray().length, "normal B");
assert.eq(3, b.find().toArray().length, "other B");

printjson(primary._db._adminCommand("shardingState"));

// --- filtering ---

function doCounts(name, total, onlyItCounts) {
    total = total || (primary.count() + secondary.count());
    if (!onlyItCounts)
        assert.eq(total, a.count(), name + " count");
    assert.eq(total, a.find().sort({n: 1}).itcount(), name + " itcount - sort n");
    assert.eq(total, a.find().itcount(), name + " itcount");
    assert.eq(total, a.find().sort({_id: 1}).itcount(), name + " itcount - sort _id");
    return total;
}

const total = doCounts("before wrong save");
assert.commandWorked(secondary.insert({_id: 111, num: -3}));
doCounts("after wrong save", total, true);

const explainResult = a.find().explain("executionStats");
const stats = explainResult.executionStats;
assert.eq(3, stats.nReturned, "ex1");
assert.eq(0, stats.totalKeysExamined, "ex2");
assert.eq(4, stats.totalDocsExamined, "ex3");
assert.commandWorked(secondary.remove(
    {_id: 111}));  // Need to remove because next move chunk errors on invalid document.

const chunkSkips = getChunkSkipsFromAllShards(explainResult);
assert.eq(1, chunkSkips, "ex4");

// SERVER-4612
// make sure idhack obeys chunks
const x = a.findOne({_id: 111});
assert(!x, "idhack didn't obey chunk boundaries " + tojson(x));

// --- move all to 1 ---
print("MOVE ALL TO 1");

assert.eq(2, s.onNumShards("test", "foo"), "on 2 shards");
s.printCollectionInfo("test.foo");

assert(a.findOne({num: 1}));
assert(b.findOne({num: 1}));

print("GOING TO MOVE");
assert(a.findOne({num: 1}), "pre move 1");
s.printCollectionInfo("test.foo");

const myto = s.getOther(s.getPrimaryShard("test")).name;
print("counts before move: " + tojson(s.shardCounts("test,", "foo")));
assert.commandWorked(
    s.s0.adminCommand({movechunk: "test.foo", find: {num: 1}, to: myto, _waitForDelete: true}));
print("counts after move: " + tojson(s.shardCounts("test", "foo")));
s.printCollectionInfo("test.foo");
assert.eq(1, s.onNumShards("test", "foo"), "on 1 shard again");
assert(a.findOne({num: 1}), "post move 1");
assert(b.findOne({num: 1}), "post move 2");

print("*** drop");

s.printCollectionInfo("test.foo", "before drop");
a.drop();
s.printCollectionInfo("test.foo", "after drop");

assert.eq(0, a.count(), "a count after drop");
assert.eq(0, b.count(), "b count after drop");

s.printCollectionInfo("test.foo", "after counts");

assert.eq(0, primary.count(), "p count after drop");
assert.eq(0, secondary.count(), "s count after drop");

// ---- retry commands SERVER-1471 ----

assert.commandWorked(
    s.s0.adminCommand({enablesharding: "test2", primaryShard: s.shard0.shardName}));
assert.commandWorked(s.s0.adminCommand({shardcollection: "test2.foo", key: {num: 1}}));

const dba = s.getDB("test2");
const dbb = s2.getDB("test2");
dba.foo.save({num: 1});
dba.foo.save({num: 2});
dba.foo.save({num: 3});

assert.eq(1, s.onNumShards("test2", "foo"), "B on 1 shards");
assert.eq(3, dba.foo.count(), "Ba");
assert.eq(3, dbb.foo.count(), "Bb");

assert.commandWorked(s.s0.adminCommand({split: "test2.foo", middle: {num: 2}}));
assert.commandWorked(s.s0.adminCommand({
    movechunk: "test2.foo",
    find: {num: 3},
    to: s.getOther(s.getPrimaryShard("test2")).name,
    _waitForDelete: true
}));

assert.eq(2, s.onNumShards("test2", "foo"), "B on 2 shards");
printjson(dba.foo.stats());
printjson(dbb.foo.stats());

s.stop();
