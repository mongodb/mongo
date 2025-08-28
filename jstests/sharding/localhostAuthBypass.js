// SERVER-6591: Localhost authentication exception doesn't work right on sharded cluster
//
// This test is to ensure that localhost authentication works correctly against a sharded
// cluster whether they are hosted with "localhost" or a hostname.

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// The following checks, which occurs on ShardingTest.stop, involve using a mongos to read data on
// the config server, but this test uses a special shutdown function which stops the mongoses before
// calling ShardingTest.stop.
// @tags : [requires_scripting]
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckMetadataConsistency = true;

let replSetName = "replsets_server-6591";
let keyfile = "jstests/libs/key1";
let numShards = 2;
let username = "foo";
let password = "bar";
let adhocShard = 0;

let createUser = function (mongo) {
    print("============ adding a user.");
    mongo.getDB("admin").createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
};

let addUsersToEachShard = function (st) {
    // In config shard mode skip the first shard because it is also the config server and will
    // already have a user made on it through mongos.
    for (let i = TestData.configShard ? 1 : 0; i < numShards; i++) {
        print("============ adding a user to shard " + i);
        let d = st["shard" + i];
        d.getDB("admin").createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
    }
};

let addShard = function (st, shouldPass) {
    adhocShard++;
    const rs = new ReplSetTest({nodes: 1, host: "localhost", name: "localhostAuthShard-" + adhocShard});
    rs.startSet({shardsvr: "", keyFile: keyfile, auth: ""});
    rs.initiate();

    let res = st.getDB("admin").runCommand({addShard: rs.getURL()});
    if (shouldPass) {
        assert.commandWorked(res, "Add shard");
    } else {
        assert.commandFailed(res, "Add shard");
    }
    return rs;
};

let findEmptyShard = function (st, ns) {
    let counts = st.chunkCounts("foo");

    for (let shard in counts) {
        if (counts[shard] == 0) {
            return shard;
        }
    }

    return null;
};

let assertCannotRunCommands = function (mongo, st) {
    print("============ ensuring that commands cannot be run.");

    // CRUD
    let test = mongo.getDB("test");
    assert.throws(function () {
        test.system.users.findOne();
    });
    assert.writeError(test.foo.save({_id: 0}));
    assert.throws(function () {
        test.foo.findOne({_id: 0});
    });
    assert.writeError(test.foo.update({_id: 0}, {$set: {x: 20}}));
    assert.writeError(test.foo.remove({_id: 0}));

    // Multi-shard
    assert.throws(function () {
        test.foo.mapReduce(
            function () {
                emit(1, 1);
            },
            function (id, count) {
                return Array.sum(count);
            },
            {out: "other"},
        );
    });

    // Config
    assert.throws(function () {
        mongo.getDB("config").shards.findOne();
    });

    let authorizeErrorCode = 13;
    let res = mongo.getDB("admin").runCommand({
        moveChunk: "test.foo",
        find: {_id: 1},
        to: st.shard0.shardName, // Arbitrary shard.
    });
    assert.commandFailedWithCode(res, authorizeErrorCode, "moveChunk");
    // Create collection
    assert.commandFailedWithCode(
        mongo.getDB("test").createCollection("log", {capped: true, size: 5242880, max: 5000}),
        authorizeErrorCode,
        "createCollection",
    );
    // Set/Get system parameters
    let params = [
        {param: "journalCommitInterval", val: 200},
        {param: "logLevel", val: 2},
        {param: "logUserIds", val: 1},
        {param: "notablescan", val: 1},
        {param: "quiet", val: 1},
        {param: "replApplyBatchSize", val: 10},
        {param: "replIndexPrefetch", val: "none"},
        {param: "syncdelay", val: 30},
        {param: "traceExceptions", val: true},
        {param: "sslMode", val: "preferSSL"},
        {param: "clusterAuthMode", val: "sendX509"},
        {param: "userCacheInvalidationIntervalSecs", val: 300},
    ];
    params.forEach(function (p) {
        let cmd = {setParameter: 1};
        cmd[p.param] = p.val;
        assert.commandFailedWithCode(
            mongo.getDB("admin").runCommand(cmd),
            authorizeErrorCode,
            "setParameter: " + p.param,
        );
    });
    params.forEach(function (p) {
        let cmd = {getParameter: 1};
        cmd[p.param] = 1;
        assert.commandFailedWithCode(
            mongo.getDB("admin").runCommand(cmd),
            authorizeErrorCode,
            "getParameter: " + p.param,
        );
    });
};

let assertCanRunCommands = function (mongo, st) {
    print("============ ensuring that commands can be run.");

    // CRUD
    let test = mongo.getDB("test");

    // this will throw if it fails
    test.system.users.findOne();

    assert.commandWorked(test.foo.save({_id: 0}));
    assert.commandWorked(test.foo.update({_id: 0}, {$set: {x: 20}}));
    assert.commandWorked(test.foo.remove({_id: 0}));

    // Multi-shard
    test.foo.mapReduce(
        function () {
            emit(1, 1);
        },
        function (id, count) {
            return Array.sum(count);
        },
        {out: "other"},
    );

    // Config
    // this will throw if it fails
    mongo.getDB("config").shards.findOne();

    let to = findEmptyShard(st, "test.foo");
    let res = mongo.getDB("admin").runCommand({moveChunk: "test.foo", find: {_id: 1}, to: to});
    assert.commandWorked(res);
};

let authenticate = function (mongo) {
    print("============ authenticating user.");
    mongo.getDB("admin").auth(username, password);
};

let setupSharding = function (shardingTest) {
    let mongo = shardingTest.s;

    print("============ enabling sharding on test.foo.");
    mongo.getDB("admin").runCommand({enableSharding: "test", primaryShard: st.shard1.shardName});
    mongo.getDB("admin").runCommand({shardCollection: "test.foo", key: {_id: 1}});

    let test = mongo.getDB("test");
    for (let i = 1; i < 20; i++) {
        test.foo.insert({_id: i});
    }
};

let start = function () {
    return new ShardingTest({
        auth: "",
        shards: numShards,
        other: {
            keyFile: keyfile,
            chunkSize: 1,
            useHostname: false, // Must use localhost to take advantage of the localhost auth bypass
        },
    });
};

print("=====================");
print("starting shards");
print("=====================");
var st = start();
let host = st.s.host;
let extraShards = [];

let mongo = new Mongo(host);

assertCannotRunCommands(mongo, st);

extraShards.push(addShard(st, 1));
createUser(mongo);

authenticate(mongo);
authenticate(st.s);
setupSharding(st);

addUsersToEachShard(st);
st.printShardingStatus();

assertCanRunCommands(mongo, st);

print("===============================");
print("reconnecting with a new client.");
print("===============================");

mongo = new Mongo(host);

assertCannotRunCommands(mongo, st);
extraShards.push(addShard(mongo, 0));

authenticate(mongo);

assertCanRunCommands(mongo, st);
extraShards.push(addShard(mongo, 1));
st.printShardingStatus();

extraShards.forEach(function (rs) {
    rs.stopSet();
});
st.stop();
