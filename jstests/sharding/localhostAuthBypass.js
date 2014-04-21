//SERVER-6591: Localhost authentication exception doesn't work right on sharded cluster
//
//This test is to ensure that localhost authentication works correctly against a sharded
//cluster whether they are hosted with "localhost" or a hostname.

var replSetName = "replsets_server-6591";
var keyfile = "jstests/libs/key1";
var numShards = 2;
var numConfigs = 3;
var username = "foo";
var password = "bar";

var createUser = function(mongo) {
    print("============ adding a user.");
    mongo.getDB("admin").createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
};

var addUsersToEachShard = function(st) {
    for(i = 0; i < numShards; i++) {
        print("============ adding a user to shard " + i);
        var d = st["shard" + i];
        d.getDB("admin").createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
    }
};

var findEmptyShard = function(st, ns) {
    var counts = st.chunkCounts( "foo" )

    for(shard in counts){
        if(counts[shard] == 0) {
            return shard;
        }
    }

    return null;
};

var assertCannotRunCommands = function(mongo, st) {
    print("============ ensuring that commands cannot be run.");

    // CRUD
    var test = mongo.getDB("test");
    assert.throws( function() { test.system.users.findOne(); });
    assert.writeError(test.foo.save({ _id: 0 }));
    assert.throws( function() { test.foo.findOne({_id:0}); });
    assert.writeError(test.foo.update({ _id: 0 }, { $set: { x: 20 }}));
    assert.writeError(test.foo.remove({ _id: 0 }));

    // Multi-shard
    assert.throws(function() { 
        test.foo.mapReduce(
            function() { emit(1, 1); }, 
            function(id, count) { return Array.sum(count); },
            { out: "other" });
    });

    // Config
    assert.throws(function() {
        mongo.getDB("config").shards.findOne();
    });

    var res = mongo.getDB("admin").runCommand({
        moveChunk: "test.foo",
        find: {_id: 1},
        to: "shard0000" // Arbitrary shard.
    });
    assert.commandFailedWithCode(res, 13 /* Authz code */);
};

var assertCanRunCommands = function(mongo, st) {
    print("============ ensuring that commands can be run.");

    // CRUD
    var test = mongo.getDB("test");

    // this will throw if it fails
    test.system.users.findOne();

    assert.writeOK(test.foo.save({ _id: 0 }));
    assert.writeOK(test.foo.update({ _id: 0 }, { $set: { x: 20 }}));
    assert.writeOK(test.foo.remove({ _id: 0 }));

    // Multi-shard
    test.foo.mapReduce(
        function() { emit(1, 1); }, 
        function(id, count) { return Array.sum(count); },
        { out: "other" }
    );

    // Config
    // this will throw if it fails
    mongo.getDB("config").shards.findOne();

    to = findEmptyShard(st, "test.foo");
    var res = mongo.getDB("admin").runCommand({
        moveChunk: "test.foo",
        find: {_id: 1},
        to: to
    });
    assert.commandWorked(res);
};

var authenticate = function(mongo) {
    print("============ authenticating user.");
    mongo.getDB("admin").auth(username, password);
};

var setupSharding = function(mongo) {
    print("============ enabling sharding on test.foo.");
    mongo.getDB("admin").runCommand({enableSharding : "test"});
    mongo.getDB("admin").runCommand({shardCollection : "test.foo", key : {_id : 1}});

    var test = mongo.getDB("test");
    for(i = 1; i < 20; i++) {
        test.foo.insert({_id: i});
    }
};

var start = function(useHostName) {
    return new ShardingTest({
        keyFile: keyfile, 
        shards: numShards, 
        chunksize: 1, 
        config: numConfigs, 
        separateConfig: true,
        other : { 
            nopreallocj: 1, 
            useHostName: useHostName 
        } 
    });
};

var shutdown = function(st) {
    print("============ shutting down.");

    // SERVER-8445
    // Unlike MongoRunner.stopMongod and ReplSetTest.stopSet,
    // ShardingTest.stop does not have a way to provide auth
    // information.  Therefore, we'll do this manually for now.

    for(i = 0; i < st._mongos.length; i++) {
        var port = st["s" + i].port;
        MongoRunner.stopMongos(
            port,
            /*signal*/false,
            { auth : { user: username, pwd: password }}
        );
    }

    for(i = 0; i < st._connections.length; i++) {
        var port = st["shard" + i].port;
        MongoRunner.stopMongod(
            port, 
            /*signal*/false, 
            { auth : { user: username, pwd: password }}
        );
    }

    for(i = 0; i < st._configServers.length; i++) {
        var c = st["config" + i].port;
        MongoRunner.stopMongod(
            port, 
            /*signal*/false, 
            { auth : { user: username, pwd: password }}
        );
    }

    st.stop();
};

var runTest = function(useHostName) {
    print("=====================");
    print("starting shards: useHostName=" + useHostName);
    print("=====================");
    var st = start(useHostName);
    var host = st.s.host;

    var mongo = new Mongo(host);

    assertCannotRunCommands(mongo, st);

    createUser(mongo);
    authenticate(mongo);
    setupSharding(mongo);

    addUsersToEachShard(st);
    authenticate(st.s);

    assertCanRunCommands(mongo, st);

    print("===============================");
    print("reconnecting with a new client.");
    print("===============================");

    mongo = new Mongo(host);

    assertCannotRunCommands(mongo, st);

    authenticate(mongo);

    assertCanRunCommands(mongo, st);

    shutdown(st);
}

runTest(false);
runTest(true);
