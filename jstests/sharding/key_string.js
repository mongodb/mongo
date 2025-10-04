import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let s = new ShardingTest({name: "keystring", shards: 2});

s.adminCommand({enablesharding: "test", primaryShard: s.shard1.shardName});
s.adminCommand({shardcollection: "test.foo", key: {name: 1}});

let primary = s.getPrimaryShard("test").getDB("test");
let seconday = s.getOther(primary).getDB("test");

assert.eq(1, findChunksUtil.countChunksForNs(s.config, "test.foo"), "sanity check A");

var db = s.getDB("test");

db.foo.save({name: "eliot"});
db.foo.save({name: "sara"});
db.foo.save({name: "bob"});
db.foo.save({name: "joe"});
db.foo.save({name: "mark"});
db.foo.save({name: "allan"});

assert.eq(6, db.foo.find().count(), "basic count");

s.adminCommand({split: "test.foo", middle: {name: "allan"}});
s.adminCommand({split: "test.foo", middle: {name: "sara"}});
s.adminCommand({split: "test.foo", middle: {name: "eliot"}});

s.adminCommand({
    movechunk: "test.foo",
    find: {name: "eliot"},
    to: seconday.getMongo().name,
    _waitForDelete: true,
});

s.printChunks();

assert.eq(3, primary.foo.find().toArray().length, "primary count");
assert.eq(3, seconday.foo.find().toArray().length, "secondary count");

assert.eq(6, db.foo.find().toArray().length, "total count");
assert.eq(6, db.foo.find().sort({name: 1}).toArray().length, "total count sorted");

assert.eq(6, db.foo.find().sort({name: 1}).count(), "total count with count()");

assert.eq(
    "allan,bob,eliot,joe,mark,sara",
    db.foo
        .find()
        .sort({name: 1})
        .toArray()
        .map(function (z) {
            return z.name;
        }),
    "sort 1",
);
assert.eq(
    "sara,mark,joe,eliot,bob,allan",
    db.foo
        .find()
        .sort({name: -1})
        .toArray()
        .map(function (z) {
            return z.name;
        }),
    "sort 2",
);

// TODO(SERVER-97588): Remove version check from tests when 8.1 becomes last LTS.
const fcvDoc = db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, "8.1") >= 0) {
    // Ensure it does not fail on boundaries that are already split.
    assert.eq(true, s.adminCommand({split: "test.foo", middle: {name: "allan"}}));
    assert.eq(true, s.adminCommand({split: "test.foo", middle: {name: "eliot"}}));
} else {
    // make sure we can't force a split on an extreme key
    // [allan->joe)
    assert.throws(function () {
        s.adminCommand({split: "test.foo", middle: {name: "allan"}});
    });
    assert.throws(function () {
        s.adminCommand({split: "test.foo", middle: {name: "eliot"}});
    });
}
s.stop();
