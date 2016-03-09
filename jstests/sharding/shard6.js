// shard6.js

summary = "";

s = new ShardingTest({name: "shard6", shards: 2});

s.config.settings.update({_id: "balancer"}, {$set: {stopped: true}}, true);

s.adminCommand({enablesharding: "test"});
s.ensurePrimaryShard('test', 'shard0001');
s.adminCommand({shardcollection: "test.data", key: {num: 1}});

db = s.getDB("test");

function poolStats(where) {
    var total = 0;
    var msg = "poolStats " + where + " ";
    var x = db.runCommand("connPoolStats").hosts;
    for (var h in x) {
        var z = x[h];
        msg += z.created + " ";
        total += z.created;
    }
    printjson(x);
    print("****\n" + msg + "\n*****");
    summary += msg + "\n";
    return [total, x.length];
}

poolStats("at start");

// we want a lot of data, so lets make a 50k string to cheat :)
bigString = "";
while (bigString.length < 50000)
    bigString += "this is a big string. ";

// ok, now lets insert a some data
var num = 0;
for (; num < 100; num++) {
    db.data.save({num: num, bigString: bigString});
}

assert.eq(100, db.data.find().toArray().length, "basic find after setup");

connBefore = poolStats("setup done");

// limit

assert.eq(77, db.data.find().limit(77).itcount(), "limit test 1");
assert.eq(1, db.data.find().limit(1).itcount(), "limit test 2");
for (var i = 1; i < 10; i++) {
    assert.eq(i, db.data.find().limit(i).itcount(), "limit test 3a : " + i);
    assert.eq(i, db.data.find().skip(i).limit(i).itcount(), "limit test 3b : " + i);
    poolStats("after loop : " + i);
}

// we do not want the number of connections from mongos to mongod to increase
// but it may have because of the background replica set monitor, and that case is ok.
// This is due to SERVER-22564.
limitTestAfterConns = poolStats("limit test done");

// only check the number of connections is the same if the number of hosts we are connected to
// remains the same. TODO: remove host count check after SERVER-22564 is fixed.
if (limitTestAfterConns[1] == connBefore[1]) {
    assert.eq(connBefore[0], limitTestAfterConns[0], "limit test conns");
}

function assertOrder(start, num) {
    var a = db.data.find().skip(start).limit(num).sort({num: 1}).map(function(z) {
        return z.num;
    });
    var c = [];
    for (var i = 0; i < num; i++)
        c.push(start + i);
    assert.eq(c, a, "assertOrder start: " + start + " num: " + num);
}

assertOrder(0, 10);
assertOrder(5, 10);

poolStats("after checking order");

function doItCount(skip, sort, batchSize) {
    var c = db.data.find();
    if (skip)
        c.skip(skip);
    if (sort)
        c.sort(sort);
    if (batchSize)
        c.batchSize(batchSize);
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

// --- test save support ---

o = db.data.findOne();
o.x = 16;
db.data.save(o);
o = db.data.findOne({_id: o._id});
assert.eq(16, o.x, "x1 - did save fail? " + tojson(o));

poolStats("at end");

print(summary);

assert.throws(function() {
    s.adminCommand({enablesharding: "admin"});
});

s.stop();
