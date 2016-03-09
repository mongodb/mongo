
function debug(str) {
    print("---\n" + str + "\n-----");
}

var name = "badNonUpdate";
debug("Starting sharded cluster test stuff");

var s = new ShardingTest({name: name, shards: 2, mongos: 2});

var mongosA = s.s0;
var mongosB = s.s1;

ns = "test.coll";
ns2 = "test.coll2";

adminSA = mongosA.getDB("admin");
adminSA.runCommand({enableSharding: "test"});

adminSA.runCommand({moveprimary: "test", to: "shard0000"});
adminSA.runCommand({moveprimary: "test2", to: "shard0001"});

adminSA.runCommand({shardCollection: ns, key: {_id: 1}});

try {
    s.stopBalancer();
} catch (e) {
    print("coundn't stop balancer via command");
}

adminSA.settings.update({_id: 'balancer'}, {$set: {stopped: true}});

var db = mongosA.getDB("test");
var coll = db.coll;
var coll2 = db.coll2;

numDocs = 10;
for (var i = 1; i < numDocs; i++) {
    coll.insert({_id: i, control: 0});
    coll2.insert({_id: i, control: 0});
}

debug("Inserted docs, now split chunks");

adminSA.runCommand({split: ns, find: {_id: 3}});
adminSA.runCommand({movechunk: ns, find: {_id: 10}, to: "shard0001"});

var command = 'printjson(db.coll.update({ _id: 9 }, { $set: { a: "9" }}, true));';

// without this first query through mongo, the second time doesn't "fail"
debug("Try query first time");
runMongoProgram("mongo", "--quiet", "--port", "" + s._mongos[1].port, "--eval", command);

var res = mongosB.getDB("test").coll2.update({_id: 0}, {$set: {c: "333"}});
assert.eq(0, res.nModified);

s.stop();
