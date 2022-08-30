/**
 * This tests using DB commands with authentication enabled when sharded.
 */
(function() {
'use strict';

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

load("jstests/replsets/rslib.js");
load("jstests/sharding/libs/find_chunks_util.js");

// Replica set nodes started with --shardsvr do not enable key generation until they are added
// to a sharded cluster and reject commands with gossiped clusterTime from users without the
// advanceClusterTime privilege. This causes ShardingTest setup to fail because the shell
// briefly authenticates as __system and receives clusterTime metadata then will fail trying to
// gossip that time later in setup.
//

var st = new ShardingTest({
    shards: 2,
    rs: {oplogSize: 10, useHostname: false},
    other: {keyFile: 'jstests/libs/key1', useHostname: false, chunkSize: 2},
});

var mongos = st.s;
var adminDB = mongos.getDB('admin');
var configDB = mongos.getDB('config');
var testDB = mongos.getDB('test');

jsTestLog('Setting up initial users');
var rwUser = 'rwUser';
var roUser = 'roUser';
var password = 'password';
var expectedDocs = 1000;

adminDB.createUser({user: rwUser, pwd: password, roles: jsTest.adminUserRoles});

assert(adminDB.auth(rwUser, password));

// Secondaries should be up here, since we awaitReplication in the ShardingTest, but we *don't*
// wait for the mongos to explicitly detect them.
awaitRSClientHosts(mongos, st.rs0.getSecondaries(), {ok: true, secondary: true});
awaitRSClientHosts(mongos, st.rs1.getSecondaries(), {ok: true, secondary: true});

testDB.createUser({user: rwUser, pwd: password, roles: jsTest.basicUserRoles});
testDB.createUser({user: roUser, pwd: password, roles: jsTest.readOnlyUserRoles});

var authenticatedConn = new Mongo(mongos.host);
authenticatedConn.getDB('admin').auth(rwUser, password);

// Add user to shards to prevent localhost connections from having automatic full access
st.rs0.getPrimary().getDB('admin').createUser(
    {user: 'user', pwd: 'password', roles: jsTest.basicUserRoles}, {w: 3, wtimeout: 30000});
st.rs1.getPrimary().getDB('admin').createUser(
    {user: 'user', pwd: 'password', roles: jsTest.basicUserRoles}, {w: 3, wtimeout: 30000});

jsTestLog('Creating initial data');

st.adminCommand({enablesharding: "test"});
st.ensurePrimaryShard('test', st.shard0.shardName);
st.adminCommand({shardcollection: "test.foo", key: {i: 1, j: 1}});

// Balancer is stopped by default, so no moveChunks will interfere with the splits we're testing

var str = 'a';
while (str.length < 8000) {
    str += str;
}

for (var i = 0; i < 100; i++) {
    var bulk = testDB.foo.initializeUnorderedBulkOp();
    for (var j = 0; j < 10; j++) {
        bulk.insert({i: i, j: j, str: str});
    }
    assert.commandWorked(bulk.execute({w: "majority"}));
    // Split the chunk we just inserted so that we have something to balance.
    assert.commandWorked(st.splitFind("test.foo", {i: i, j: 0}));
}

assert.eq(expectedDocs, testDB.foo.count());

// Wait for the balancer to start back up
assert.commandWorked(
    configDB.settings.update({_id: 'balancer'}, {$set: {_waitForDelete: true}}, true));
st.startBalancer();

// Make sure we've done at least some splitting, so the balancer will work
assert.gt(findChunksUtil.findChunksByNs(configDB, 'test.foo').count(), 2);

// Make sure we eventually balance the 'test.foo' collection
st.awaitBalance('foo', 'test', 60 * 5 * 1000);

var map = function() {
    emit(this.i, this.j);
};

var reduce = function(key, values) {
    var jCount = 0;
    values.forEach(function(j) {
        jCount += j;
    });
    return jCount;
};

var checkCommandSucceeded = function(db, cmdObj) {
    print("Running command that should succeed: " + tojson(cmdObj));
    var resultObj = assert.commandWorked(db.runCommand(cmdObj));
    printjson(resultObj);
    return resultObj;
};

var checkCommandFailed = function(db, cmdObj) {
    print("Running command that should fail: " + tojson(cmdObj));
    var resultObj = assert.commandFailed(db.runCommand(cmdObj));
    printjson(resultObj);
    return resultObj;
};

var checkReadOps = function(hasReadAuth) {
    if (hasReadAuth) {
        print("Checking read operations, should work");
        assert.eq(expectedDocs, testDB.foo.find().itcount());
        assert.eq(expectedDocs, testDB.foo.count());

        checkCommandSucceeded(testDB, {dbstats: 1});
        checkCommandSucceeded(testDB, {collstats: 'foo'});

        // inline map-reduce works read-only
        var res = checkCommandSucceeded(
            testDB, {mapreduce: 'foo', map: map, reduce: reduce, out: {inline: 1}});
        assert.eq(100, res.results.length);
        assert.eq(45, res.results[0].value);

        res = checkCommandSucceeded(testDB, {
            aggregate: 'foo',
            pipeline: [{$project: {j: 1}}, {$group: {_id: 'j', sum: {$sum: '$j'}}}],
            cursor: {}
        });
        assert.eq(4500, res.cursor.firstBatch[0].sum);
    } else {
        print("Checking read operations, should fail");
        assert.throws(function() {
            testDB.foo.find().itcount();
        });
        checkCommandFailed(testDB, {dbstats: 1});
        checkCommandFailed(testDB, {collstats: 'foo'});
        checkCommandFailed(testDB, {mapreduce: 'foo', map: map, reduce: reduce, out: {inline: 1}});
        checkCommandFailed(testDB, {
            aggregate: 'foo',
            pipeline: [{$project: {j: 1}}, {$group: {_id: 'j', sum: {$sum: '$j'}}}],
            cursor: {}
        });
    }
};

var checkWriteOps = function(hasWriteAuth) {
    if (hasWriteAuth) {
        print("Checking write operations, should work");
        testDB.foo.insert({a: 1, i: 1, j: 1});
        var res = checkCommandSucceeded(
            testDB, {findAndModify: "foo", query: {a: 1, i: 1, j: 1}, update: {$set: {b: 1}}});
        assert.eq(1, res.value.a);
        assert.eq(null, res.value.b);
        assert.eq(1, testDB.foo.findOne({a: 1}).b);
        testDB.foo.remove({a: 1});
        assert.eq(null, testDB.runCommand({getlasterror: 1}).err);
        checkCommandSucceeded(testDB,
                              {mapreduce: 'foo', map: map, reduce: reduce, out: 'mrOutput'});
        assert.eq(100, testDB.mrOutput.count());
        assert.eq(45, testDB.mrOutput.findOne().value);

        checkCommandSucceeded(testDB, {drop: 'foo'});
        assert.eq(0, testDB.foo.count());
        testDB.foo.insert({a: 1});
        assert.eq(1, testDB.foo.count());
        checkCommandSucceeded(testDB, {dropDatabase: 1});
        assert.eq(0, testDB.foo.count());
        checkCommandSucceeded(testDB, {create: 'baz'});
    } else {
        print("Checking write operations, should fail");
        testDB.foo.insert({a: 1, i: 1, j: 1});
        assert.eq(0, authenticatedConn.getDB('test').foo.count({a: 1, i: 1, j: 1}));
        checkCommandFailed(
            testDB, {findAndModify: "foo", query: {a: 1, i: 1, j: 1}, update: {$set: {b: 1}}});
        checkCommandFailed(testDB, {mapreduce: 'foo', map: map, reduce: reduce, out: 'mrOutput'});
        checkCommandFailed(testDB, {drop: 'foo'});
        checkCommandFailed(testDB, {dropDatabase: 1});
        var passed = true;
        try {
            // For some reason when create fails it throws an exception instead of just
            // returning ok:0
            var res = testDB.runCommand({create: 'baz'});
            if (!res.ok) {
                passed = false;
            }
        } catch (e) {
            // expected
            printjson(e);
            passed = false;
        }
        assert(!passed);
    }
};

var checkAdminOps = function(hasAuth) {
    if (hasAuth) {
        checkCommandSucceeded(adminDB, {getCmdLineOpts: 1});
        checkCommandSucceeded(adminDB, {serverStatus: 1});
        checkCommandSucceeded(adminDB, {listShards: 1});
        checkCommandSucceeded(adminDB, {whatsmyuri: 1});
        checkCommandSucceeded(adminDB, {isdbgrid: 1});
        checkCommandSucceeded(adminDB, {ismaster: 1});
        checkCommandSucceeded(adminDB, {hello: 1});
        checkCommandSucceeded(adminDB, {split: 'test.foo', find: {i: 1, j: 1}});
        var chunk = findChunksUtil.findOneChunkByNs(configDB, 'test.foo', {shard: st.rs0.name});
        checkCommandSucceeded(
            adminDB,
            {moveChunk: 'test.foo', find: chunk.min, to: st.rs1.name, _waitForDelete: true});
    } else {
        checkCommandFailed(adminDB, {getCmdLineOpts: 1});
        checkCommandFailed(adminDB, {serverStatus: 1});
        checkCommandFailed(adminDB, {listShards: 1});
        // whatsmyuri, isdbgrid, ismaster, and hello don't require any auth
        checkCommandSucceeded(adminDB, {whatsmyuri: 1});
        checkCommandSucceeded(adminDB, {isdbgrid: 1});
        checkCommandSucceeded(adminDB, {ismaster: 1});
        checkCommandSucceeded(adminDB, {hello: 1});
        checkCommandFailed(adminDB, {split: 'test.foo', find: {i: 1, j: 1}});
        var chunkKey = {i: {$minKey: 1}, j: {$minKey: 1}};
        checkCommandFailed(
            adminDB,
            {moveChunk: 'test.foo', find: chunkKey, to: st.rs1.name, _waitForDelete: true});
    }
};

var checkRemoveShard = function(hasWriteAuth) {
    if (hasWriteAuth) {
        // start draining
        checkCommandSucceeded(adminDB, {removeshard: st.rs1.name});
        // Wait for shard to be completely removed
        checkRemoveShard = function() {
            var res = checkCommandSucceeded(adminDB, {removeshard: st.rs1.name});
            return res.msg == 'removeshard completed successfully';
        };
        assert.soon(checkRemoveShard, "failed to remove shard");
    } else {
        checkCommandFailed(adminDB, {removeshard: st.rs1.name});
    }
};

var checkAddShard = function(hasWriteAuth) {
    if (hasWriteAuth) {
        checkCommandSucceeded(adminDB, {addshard: st.rs1.getURL()});
    } else {
        checkCommandFailed(adminDB, {addshard: st.rs1.getURL()});
    }
};

st.stopBalancer();

jsTestLog("Checking admin commands with admin auth credentials");
checkAdminOps(true);
assert(adminDB.logout().ok);

jsTestLog("Checking admin commands with no auth credentials");
checkAdminOps(false);

jsTestLog("Checking commands with no auth credentials");
checkReadOps(false);
checkWriteOps(false);

// Authenticate as read-only user
jsTestLog("Checking commands with read-only auth credentials");
assert(testDB.auth(roUser, password));
checkReadOps(true);
checkWriteOps(false);

// Authenticate as read-write user
jsTestLog("Checking commands with read-write auth credentials");
assert(testDB.logout().ok);
assert(testDB.auth(rwUser, password));
checkReadOps(true);
checkWriteOps(true);

jsTestLog("Check drainging/removing a shard");
assert(testDB.logout().ok);
checkRemoveShard(false);
assert(adminDB.auth(rwUser, password));
assert(testDB.dropDatabase().ok);
checkRemoveShard(true);
st.printShardingStatus();

jsTestLog("Check adding a shard");
assert(adminDB.logout().ok);
checkAddShard(false);
assert(adminDB.auth(rwUser, password));
checkAddShard(true);
st.printShardingStatus();

st.stop();
})();
