// SERVER-6591: Localhost authentication exception doesn't work right on sharded cluster
//
// This test is to ensure that localhost authentication works correctly against a sharded
// cluster whether they are hosted with "localhost" or a hostname.

// Checking UUID consistency, which occurs on ShardingTest.stop, involves using a merizos to read
// data on the config server, but this test uses a special shutdown function which stops the
// merizoses before calling ShardingTest.stop.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    'use strict';

    var replSetName = "replsets_server-6591";
    var keyfile = "jstests/libs/key1";
    var numShards = 2;
    var username = "foo";
    var password = "bar";

    var createUser = function(merizo) {
        print("============ adding a user.");
        merizo.getDB("admin").createUser(
            {user: username, pwd: password, roles: jsTest.adminUserRoles});
    };

    var addUsersToEachShard = function(st) {
        for (var i = 0; i < numShards; i++) {
            print("============ adding a user to shard " + i);
            var d = st["shard" + i];
            d.getDB("admin").createUser(
                {user: username, pwd: password, roles: jsTest.adminUserRoles});
        }
    };

    var addShard = function(st, shouldPass) {
        var m =
            MerizoRunner.runMerizod({auth: "", keyFile: keyfile, useHostname: false, 'shardsvr': ''});
        var res = st.getDB("admin").runCommand({addShard: m.host});
        if (shouldPass) {
            assert.commandWorked(res, "Add shard");
        } else {
            assert.commandFailed(res, "Add shard");
        }
        return m;
    };

    var findEmptyShard = function(st, ns) {
        var counts = st.chunkCounts("foo");

        for (var shard in counts) {
            if (counts[shard] == 0) {
                return shard;
            }
        }

        return null;
    };

    var assertCannotRunCommands = function(merizo, st) {
        print("============ ensuring that commands cannot be run.");

        // CRUD
        var test = merizo.getDB("test");
        assert.throws(function() {
            test.system.users.findOne();
        });
        assert.writeError(test.foo.save({_id: 0}));
        assert.throws(function() {
            test.foo.findOne({_id: 0});
        });
        assert.writeError(test.foo.update({_id: 0}, {$set: {x: 20}}));
        assert.writeError(test.foo.remove({_id: 0}));

        // Multi-shard
        assert.throws(function() {
            test.foo.mapReduce(
                function() {
                    emit(1, 1);
                },
                function(id, count) {
                    return Array.sum(count);
                },
                {out: "other"});
        });

        // Config
        assert.throws(function() {
            merizo.getDB("config").shards.findOne();
        });

        var authorizeErrorCode = 13;
        var res = merizo.getDB("admin").runCommand({
            moveChunk: "test.foo",
            find: {_id: 1},
            to: st.shard0.shardName  // Arbitrary shard.
        });
        assert.commandFailedWithCode(res, authorizeErrorCode, "moveChunk");
        // Create collection
        assert.commandFailedWithCode(
            merizo.getDB("test").createCollection("log", {capped: true, size: 5242880, max: 5000}),
            authorizeErrorCode,
            "createCollection");
        // Set/Get system parameters
        var params = [
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
            {param: "userCacheInvalidationIntervalSecs", val: 300}
        ];
        params.forEach(function(p) {
            var cmd = {setParameter: 1};
            cmd[p.param] = p.val;
            assert.commandFailedWithCode(merizo.getDB("admin").runCommand(cmd),
                                         authorizeErrorCode,
                                         "setParameter: " + p.param);
        });
        params.forEach(function(p) {
            var cmd = {getParameter: 1};
            cmd[p.param] = 1;
            assert.commandFailedWithCode(merizo.getDB("admin").runCommand(cmd),
                                         authorizeErrorCode,
                                         "getParameter: " + p.param);
        });
    };

    var assertCanRunCommands = function(merizo, st) {
        print("============ ensuring that commands can be run.");

        // CRUD
        var test = merizo.getDB("test");

        // this will throw if it fails
        test.system.users.findOne();

        assert.writeOK(test.foo.save({_id: 0}));
        assert.writeOK(test.foo.update({_id: 0}, {$set: {x: 20}}));
        assert.writeOK(test.foo.remove({_id: 0}));

        // Multi-shard
        test.foo.mapReduce(
            function() {
                emit(1, 1);
            },
            function(id, count) {
                return Array.sum(count);
            },
            {out: "other"});

        // Config
        // this will throw if it fails
        merizo.getDB("config").shards.findOne();

        var to = findEmptyShard(st, "test.foo");
        var res = merizo.getDB("admin").runCommand({moveChunk: "test.foo", find: {_id: 1}, to: to});
        assert.commandWorked(res);
    };

    var authenticate = function(merizo) {
        print("============ authenticating user.");
        merizo.getDB("admin").auth(username, password);
    };

    var setupSharding = function(shardingTest) {
        var merizo = shardingTest.s;

        print("============ enabling sharding on test.foo.");
        merizo.getDB("admin").runCommand({enableSharding: "test"});
        shardingTest.ensurePrimaryShard('test', st.shard1.shardName);
        merizo.getDB("admin").runCommand({shardCollection: "test.foo", key: {_id: 1}});

        var test = merizo.getDB("test");
        for (var i = 1; i < 20; i++) {
            test.foo.insert({_id: i});
        }
    };

    var start = function() {
        // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
        return new ShardingTest({
            auth: "",
            shards: numShards,
            other: {
                keyFile: keyfile,
                chunkSize: 1,
                useHostname:
                    false,  // Must use localhost to take advantage of the localhost auth bypass
                shardAsReplicaSet: false
            }
        });
    };

    var shutdown = function(st) {
        print("============ shutting down.");

        // SERVER-8445
        // Unlike MerizoRunner.stopMerizod and ReplSetTest.stopSet,
        // ShardingTest.stop does not have a way to provide auth
        // information.  Therefore, we'll do this manually for now.

        for (var i = 0; i < st._merizos.length; i++) {
            var conn = st["s" + i];
            MerizoRunner.stopMerizos(conn,
                                   /*signal*/ false,
                                   {auth: {user: username, pwd: password}});
        }

        for (var i = 0; i < st._connections.length; i++) {
            var conn = st["shard" + i];
            MerizoRunner.stopMerizod(conn,
                                   /*signal*/ false,
                                   {auth: {user: username, pwd: password}});
        }

        for (var i = 0; i < st._configServers.length; i++) {
            var conn = st["config" + i];
            MerizoRunner.stopMerizod(conn,
                                   /*signal*/ false,
                                   {auth: {user: username, pwd: password}});
        }

        st.stop();
    };

    print("=====================");
    print("starting shards");
    print("=====================");
    var st = start();
    var host = st.s.host;
    var extraShards = [];

    var merizo = new Merizo(host);

    assertCannotRunCommands(merizo, st);

    extraShards.push(addShard(st, 1));
    createUser(merizo);

    authenticate(merizo);
    authenticate(st.s);
    setupSharding(st);

    addUsersToEachShard(st);
    st.printShardingStatus();

    assertCanRunCommands(merizo, st);

    print("===============================");
    print("reconnecting with a new client.");
    print("===============================");

    merizo = new Merizo(host);

    assertCannotRunCommands(merizo, st);
    extraShards.push(addShard(merizo, 0));

    authenticate(merizo);

    assertCanRunCommands(merizo, st);
    extraShards.push(addShard(merizo, 1));
    st.printShardingStatus();

    shutdown(st);
    extraShards.forEach(function(sh) {
        MerizoRunner.stopMerizod(sh);
    });
})();
