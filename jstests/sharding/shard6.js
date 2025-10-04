// shard6.js
import {ShardingTest} from "jstests/libs/shardingtest.js";

let summary = "";

let s = new ShardingTest({name: "shard6", shards: 2});

s.adminCommand({enablesharding: "test", primaryShard: s.shard1.shardName});
s.adminCommand({shardcollection: "test.data", key: {num: 1}});

let version = s.getDB("admin").runCommand({buildinfo: 1}).versionArray;
let post32 = version[0] > 4 || (version[0] == 3 && version[1] > 2);

var db = s.getDB("test");

function poolStats(where) {
    let total = 0;
    let msg = "poolStats " + where + " ";
    let stats = db.runCommand("connPoolStats");
    for (let h in stats.hosts) {
        if (!stats.hosts.hasOwnProperty(h)) {
            continue;
        }
        let host = stats.hosts[h];
        msg += host.created + " ";
        total += host.created;
    }
    printjson(stats.hosts);
    print("****\n" + msg + "\n*****");
    summary += msg + "\n";
    if (post32) {
        assert.eq(total, stats.totalCreated, "mismatched number of total connections created");
    }
    return total;
}

poolStats("at start");

// we want a lot of data, so lets make a 50k string to cheat :)
let bigString = "this is a big string. ".repeat(50000);

// ok, now lets insert a some data
let num = 0;
for (; num < 100; num++) {
    db.data.save({num: num, bigString: bigString});
}

assert.eq(100, db.data.find().toArray().length, "basic find after setup");

poolStats("setup done");

// limit

assert.eq(77, db.data.find().limit(77).itcount(), "limit test 1");
assert.eq(1, db.data.find().limit(1).itcount(), "limit test 2");
for (let i = 1; i < 10; i++) {
    assert.eq(i, db.data.find().limit(i).itcount(), "limit test 3a : " + i);
    assert.eq(i, db.data.find().skip(i).limit(i).itcount(), "limit test 3b : " + i);
    poolStats("after loop : " + i);
}

poolStats("limit test done");

function assertOrder(start, num) {
    let a = db.data
        .find()
        .skip(start)
        .limit(num)
        .sort({num: 1})
        .map(function (z) {
            return z.num;
        });
    let c = [];
    for (let i = 0; i < num; i++) c.push(start + i);
    assert.eq(c, a, "assertOrder start: " + start + " num: " + num);
}

assertOrder(0, 10);
assertOrder(5, 10);

poolStats("after checking order");

function doItCount(skip, sort, batchSize) {
    let c = db.data.find();
    if (skip) c.skip(skip);
    if (sort) c.sort(sort);
    if (batchSize) c.batchSize(batchSize);
    return c.itcount();
}

function checkItCount(batchSize) {
    assert.eq(5, doItCount(num - 5, null, batchSize), "skip 1 " + batchSize);
    assert.eq(5, doItCount(num - 5, {num: 1}, batchSize), "skip 2 " + batchSize);
    assert.eq(5, doItCount(num - 5, {_id: 1}, batchSize), "skip 3 " + batchSize);
    assert.eq(0, doItCount(num + 5, {num: 1}, batchSize), "skip 4 " + batchSize);
    assert.eq(0, doItCount(num + 5, {_id: 1}, batchSize), "skip 5 " + batchSize);
}

poolStats("before checking itcount");

checkItCount(0);
checkItCount(2);

poolStats("after checking itcount");

poolStats("at end");

print(summary);

assert.throws(function () {
    s.adminCommand({enablesharding: "admin"});
});

s.stop();
