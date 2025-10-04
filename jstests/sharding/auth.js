/**
 * Tests administrative sharding operations and map-reduce work or fail as expected, when key-based
 * authentication is used
 *
 * This test is labeled resource intensive because its total io_write is 30MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [
 *   resource_intensive,
 *   requires_scripting
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {traceMissingDoc} from "jstests/libs/trace_missing_docs.js";
import {awaitRSClientHosts} from "jstests/replsets/rslib.js";

// Replica set nodes started with --shardsvr do not enable key generation until they are added
// to a sharded cluster and reject commands with gossiped clusterTime from users without the
// advanceClusterTime privilege. This causes ShardingTest setup to fail because the shell
// briefly authenticates as __system and receives clusterTime metadata then will fail trying to
// gossip that time later in setup.
//

let adminUser = {db: "admin", username: "foo", password: "bar"};

let testUser = {db: "test", username: "bar", password: "baz"};

let testUserReadOnly = {db: "test", username: "sad", password: "bat"};

function login(userObj, thingToUse) {
    if (!thingToUse) {
        thingToUse = s;
    }

    thingToUse.getDB(userObj.db).auth(userObj.username, userObj.password);
}

function logout(userObj, thingToUse) {
    if (!thingToUse) thingToUse = s;

    s.getDB(userObj.db).runCommand({logout: 1});
}

function getShardName(rsTest) {
    let primary = rsTest.getPrimary();
    let config = primary.getDB("local").system.replset.findOne();
    let members = config.members.map(function (elem) {
        return elem.host;
    });
    return config._id + "/" + members.join(",");
}

var s = new ShardingTest({
    name: "auth",
    mongos: 1,
    shards: TestData.configShard ? 1 : 0,
    other: {keyFile: "jstests/libs/key1", chunkSize: 1},
});

if (s.getDB("admin").runCommand("buildInfo").bits < 64) {
    print("Skipping test on 32-bit platforms");
    quit();
}

print("Configuration: Add user " + tojson(adminUser));
s.getDB(adminUser.db).createUser({user: adminUser.username, pwd: adminUser.password, roles: jsTest.adminUserRoles});
login(adminUser);

// Set the chunk size, disable the secondary throttle (so the test doesn't run so slow)
assert.commandWorked(
    s
        .getDB("config")
        .settings.update(
            {_id: "balancer"},
            {$set: {"_secondaryThrottle": false, "_waitForDelete": true}},
            {upsert: true},
        ),
);

printjson(s.getDB("config").settings.find().toArray());

print("Restart mongos with different auth options");
s.restartMongos(0);
login(adminUser);

let d1 = new ReplSetTest({name: "d1", nodes: 3, useHostName: true, waitForKeys: false});
d1.startSet({keyFile: "jstests/libs/key2", shardsvr: ""});
d1.initiate();

print("d1 initiated");
let shardName = authutil.asCluster(d1.nodes, "jstests/libs/key2", function () {
    return getShardName(d1);
});

print("adding shard w/out auth " + shardName);
logout(adminUser);

let result = s.getDB("admin").runCommand({addShard: shardName});
assert.commandFailedWithCode(result, ErrorCodes.Unauthorized);

login(adminUser);

print("adding shard w/wrong key " + shardName);

let thrown = false;
try {
    result = s.adminCommand({addShard: shardName});
} catch (e) {
    thrown = true;
    printjson(e);
}
assert(thrown);

print("start rs w/correct key");

d1.stopSet(null /* signal */, true /* forRestart */);
// If we are using the in-memory storage engine, we need to re-initiate the replica set
// after restart before an election can occur, since the config does not persist. So
// we must disable the auto stepup-on-restart behavior.
if (jsTest.options().storageEngine == "inMemory") {
    d1.startSet({keyFile: "jstests/libs/key1"}, true /* restart */, true /* skipStepUpOnRestart */);
    d1.initiate();
} else {
    d1.startSet({keyFile: "jstests/libs/key1", restart: true});
}

let primary = d1.getPrimary();

print("adding shard w/auth " + shardName);

result = s.getDB("admin").runCommand({addShard: shardName});
assert.commandWorked(result);

s.getDB("admin").runCommand({enableSharding: "test"});
s.getDB("admin").runCommand({shardCollection: "test.foo", key: {x: 1}});

d1.awaitSecondaryNodes(5 * 60 * 1000);

s.getDB(testUser.db).createUser({user: testUser.username, pwd: testUser.password, roles: jsTest.basicUserRoles});
s.getDB(testUserReadOnly.db).createUser({
    user: testUserReadOnly.username,
    pwd: testUserReadOnly.password,
    roles: jsTest.readOnlyUserRoles,
});

logout(adminUser);

print("query try");
let e = assert.throws(function () {
    s.s.getDB("foo").bar.findOne();
});
printjson(e);

print("cmd try");
assert.eq(0, s.s.getDB("foo").runCommand({listDatabases: 1}).ok);

print("insert try 1");
s.getDB("test").foo.insert({x: 1});

login(testUser);
assert.eq(s.getDB("test").foo.findOne(), null);

print("insert try 2");
assert.commandWorked(s.getDB("test").foo.insert({x: 1}));
assert.eq(1, s.getDB("test").foo.find().itcount(), tojson(result));

logout(testUser);

let d2 = new ReplSetTest({name: "d2", nodes: 3, useHostName: true, waitForKeys: false});
d2.startSet({keyFile: "jstests/libs/key1", shardsvr: ""});
d2.initiate();
d2.awaitSecondaryNodes();

shardName = authutil.asCluster(d2.nodes, "jstests/libs/key1", function () {
    return getShardName(d2);
});

print("adding shard " + shardName);
login(adminUser);
print("logged in");
result = s.getDB("admin").runCommand({addShard: shardName});
assert.commandWorked(result);

awaitRSClientHosts(s.s, d1.nodes, {ok: true});
awaitRSClientHosts(s.s, d2.nodes, {ok: true});

s.getDB("test").foo.remove({});

let num = 10;
assert.commandWorked(s.s.adminCommand({split: "test.foo", middle: {x: num / 2}}));
const bigString = "X".repeat(1024 * 1024); // 1MB
let bulk = s.getDB("test").foo.initializeUnorderedBulkOp();
for (i = 0; i < num; i++) {
    bulk.insert({_id: i, x: i, abc: "defg", date: new Date(), str: bigString});
}
assert.commandWorked(bulk.execute());

s.startBalancer(60000);

// SERVER-33753: count() without predicate can be wrong on sharded collections.
// assert.eq(s.getDB("test").foo.count(), num+1);
let numDocs = s.getDB("test").foo.find().itcount();
if (numDocs != num) {
    // Missing documents. At this point we're already in a failure mode, the code in this
    // statement
    // is to get a better idea how/why it's failing.

    let numDocsSeen = 0;
    let lastDocNumber = -1;
    let missingDocNumbers = [];
    let docs = s.getDB("test").foo.find().sort({x: 1}).toArray();
    for (var i = 0; i < docs.length; i++) {
        if (docs[i].x != lastDocNumber + 1) {
            for (let missing = lastDocNumber + 1; missing < docs[i].x; missing++) {
                missingDocNumbers.push(missing);
            }
        }
        lastDocNumber = docs[i].x;
        numDocsSeen++;
    }
    assert.eq(numDocs, numDocsSeen, "More docs discovered on second find()");
    assert.eq(num - numDocs, missingDocNumbers.length);

    for (var i = 0; i < missingDocNumbers.length; i++) {
        jsTest.log("Tracing doc: " + missingDocNumbers[i]);
        traceMissingDoc(s.getDB("test").foo, {_id: missingDocNumbers[i], x: missingDocNumbers[i]});
    }

    assert(false, "Number of docs found does not equal the number inserted. Missing docs: " + missingDocNumbers);
}

// We're only sure we aren't duplicating documents iff there's no balancing going on here
// This call also waits for any ongoing balancing to stop
s.stopBalancer(60000);

let cursor = s.getDB("test").foo.find({x: {$lt: 5}});

let count = 0;
while (cursor.hasNext()) {
    cursor.next();
    count++;
}

assert.eq(count, 5);

logout(adminUser);

d1.awaitSecondaryNodes(5 * 60 * 1000);
d2.awaitSecondaryNodes(5 * 60 * 1000);

authutil.asCluster(d1.nodes, "jstests/libs/key1", function () {
    d1.awaitReplication();
});
authutil.asCluster(d2.nodes, "jstests/libs/key1", function () {
    d2.awaitReplication();
});

// add admin on shard itself, hack to prevent localhost auth bypass
d1.getPrimary()
    .getDB(adminUser.db)
    .createUser(
        {user: adminUser.username, pwd: adminUser.password, roles: jsTest.adminUserRoles},
        {w: 3, wtimeout: 60000},
    );
d2.getPrimary()
    .getDB(adminUser.db)
    .createUser(
        {user: adminUser.username, pwd: adminUser.password, roles: jsTest.adminUserRoles},
        {w: 3, wtimeout: 60000},
    );

login(testUser);
print("testing map reduce");

// Sharded map reduce can be tricky since all components talk to each other. For example
// SERVER-4114 is triggered when 1 mongod connects to another for final reduce it's not
// properly tested here since addresses are localhost, which is more permissive.
let res = s.getDB("test").runCommand({
    mapreduce: "foo",
    map: function () {
        emit(this.x, 1);
    },
    reduce: function (key, values) {
        return values.length;
    },
    out: "mrout",
});
printjson(res);
assert.commandWorked(res);

// Test read only users
print("starting read only tests");

let readOnlyS = new Mongo(s.getDB("test").getMongo().host);
let readOnlyDB = readOnlyS.getDB("test");

print("   testing find that should fail");
assert.throws(function () {
    readOnlyDB.foo.findOne();
});

print("   logging in");
login(testUserReadOnly, readOnlyS);

print("   testing find that should work");
readOnlyDB.foo.findOne();

print("   testing write that should fail");
assert.writeError(readOnlyDB.foo.insert({eliot: 1}));

print("   testing read command (should succeed)");
assert.commandWorked(readOnlyDB.runCommand({count: "foo"}));

print("make sure currentOp/killOp fail");
assert.commandFailed(readOnlyDB.currentOp());
assert.commandFailed(readOnlyDB.killOp(2000000000));

// fsyncUnlock doesn't work in mongos anyway, so no need check authorization for it
/*
broken because of SERVER-4156
print( "   testing write command (should fail)" );
assert.commandFailed(readOnlyDB.runCommand(
    {mapreduce : "foo",
     map : function() { emit(this.y, 1); },
     reduce : function(key, values) { return values.length; },
     out:"blarg"
    }));
*/

print("   testing logout (should succeed)");
assert.commandWorked(readOnlyDB.runCommand({logout: 1}));

print("make sure currentOp/killOp fail again");
assert.commandFailed(readOnlyDB.currentOp());
assert.commandFailed(readOnlyDB.killOp(2000000000));

s.stop();
d1.stopSet();
d2.stopSet();
