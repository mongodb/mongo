// Tests that a user can only run a getMore on a cursor that they created.
(function() {
    "use strict";

    function runTest(conn) {
        let adminDB = conn.getDB("admin");
        let isMaster = adminDB.runCommand("ismaster");
        assert.commandWorked(isMaster);
        const isMongos = (isMaster.msg === "isdbgrid");

        // Create the admin user.
        assert.commandWorked(
            adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
        assert.eq(1, adminDB.auth("admin", "admin"));

        let ismmap = false;
        if (!isMongos) {
            ismmap = assert.commandWorked(adminDB.serverStatus()).storageEngine.name == "mmapv1";
        }

        // Set up the test database.
        const testDBName = "auth_getMore";
        let testDB = adminDB.getSiblingDB(testDBName);
        testDB.dropDatabase();
        assert.writeOK(testDB.foo.insert({_id: 0}));
        assert.writeOK(testDB.foo.insert({_id: 1}));
        assert.writeOK(testDB.foo.insert({_id: 2}));

        //
        // Test that a user can only run a getMore on a cursor that they created.
        //

        // Create two users, "Alice" and "Mallory".
        assert.commandWorked(
            testDB.runCommand({createUser: "Alice", pwd: "pwd", roles: ["readWrite"]}));
        assert.commandWorked(
            testDB.runCommand({createUser: "Mallory", pwd: "pwd", roles: ["readWrite"]}));
        adminDB.logout();

        // Test that "Mallory" cannot use a find cursor created by "Alice".
        assert.eq(1, testDB.auth("Alice", "pwd"));
        let res = assert.commandWorked(testDB.runCommand({find: "foo", batchSize: 0}));
        let cursorId = res.cursor.id;
        testDB.logout();
        assert.eq(1, testDB.auth("Mallory", "pwd"));
        assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "foo"}),
                                     ErrorCodes.Unauthorized,
                                     "read from another user's find cursor");
        testDB.logout();

        // Test that "Mallory" cannot use a legacy find cursor created by "Alice".
        testDB.getMongo().forceReadMode("legacy");
        assert.eq(1, testDB.auth("Alice", "pwd"));
        let cursor = testDB.foo.find().batchSize(2);
        cursor.next();
        cursor.next();
        testDB.logout();
        assert.eq(1, testDB.auth("Mallory", "pwd"));
        assert.throws(function() {
            cursor.next();
        }, [], "read from another user's legacy find cursor");
        testDB.logout();
        testDB.getMongo().forceReadMode("commands");

        // Test that "Mallory" cannot use an aggregation cursor created by "Alice".
        assert.eq(1, testDB.auth("Alice", "pwd"));
        res = assert.commandWorked(
            testDB.runCommand({aggregate: "foo", pipeline: [], cursor: {batchSize: 0}}));
        cursorId = res.cursor.id;
        testDB.logout();
        assert.eq(1, testDB.auth("Mallory", "pwd"));
        assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "foo"}),
                                     ErrorCodes.Unauthorized,
                                     "read from another user's aggregate cursor");
        testDB.logout();

        // Test that "Mallory" cannot use a listCollections cursor created by "Alice".
        assert.eq(1, testDB.auth("Alice", "pwd"));
        res = assert.commandWorked(testDB.runCommand({listCollections: 1, cursor: {batchSize: 0}}));
        cursorId = res.cursor.id;
        testDB.logout();
        assert.eq(1, testDB.auth("Mallory", "pwd"));
        assert.commandFailedWithCode(
            testDB.runCommand({getMore: cursorId, collection: "$cmd.listCollections"}),
            ErrorCodes.Unauthorized,
            "read from another user's listCollections cursor");
        testDB.logout();

        // Test that "Mallory" cannot use a listIndexes cursor created by "Alice".
        assert.eq(1, testDB.auth("Alice", "pwd"));
        res = assert.commandWorked(testDB.runCommand({listIndexes: "foo", cursor: {batchSize: 0}}));
        cursorId = res.cursor.id;
        testDB.logout();
        assert.eq(1, testDB.auth("Mallory", "pwd"));
        assert.commandFailedWithCode(
            testDB.runCommand({getMore: cursorId, collection: "$cmd.listIndexes.foo"}),
            ErrorCodes.Unauthorized,
            "read from another user's listIndexes cursor");
        testDB.logout();

        // Test that "Mallory" cannot use a parallelCollectionScan cursor created by "Alice".
        if (!isMongos) {
            assert.eq(1, testDB.auth("Alice", "pwd"));
            res = assert.commandWorked(
                testDB.runCommand({parallelCollectionScan: "foo", numCursors: 1}));
            assert.eq(res.cursors.length, 1, tojson(res));
            cursorId = res.cursors[0].cursor.id;
            testDB.logout();
            assert.eq(1, testDB.auth("Mallory", "pwd"));
            assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "foo"}),
                                         ErrorCodes.Unauthorized,
                                         "read from another user's parallelCollectionScan cursor");
            testDB.logout();
        }

        // Test that "Mallory" cannot use a repairCursor cursor created by "Alice".
        if (!isMongos && ismmap) {
            assert.eq(1, testDB.auth("Alice", "pwd"));
            res = assert.commandWorked(testDB.runCommand({repairCursor: "foo"}));
            cursorId = res.cursor.id;
            testDB.logout();
            assert.eq(1, testDB.auth("Mallory", "pwd"));
            assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "foo"}),
                                         ErrorCodes.Unauthorized,
                                         "read from another user's repairCursor cursor");
            testDB.logout();
        }

        //
        // Test that a user can run a getMore on an aggregate cursor they created, even if some
        // privileges required for the pipeline have been revoked in the meantime.
        //

        assert.eq(1, testDB.auth("Alice", "pwd"));
        res = assert.commandWorked(testDB.runCommand({
            aggregate: "foo",
            pipeline: [{$match: {_id: 0}}, {$out: "out"}],
            cursor: {batchSize: 0}
        }));
        cursorId = res.cursor.id;
        testDB.logout();
        assert.eq(1, adminDB.auth("admin", "admin"));
        testDB.revokeRolesFromUser("Alice", ["readWrite"]);
        testDB.grantRolesToUser("Alice", ["read"]);
        adminDB.logout();
        assert.eq(1, testDB.auth("Alice", "pwd"));
        assert.commandFailedWithCode(
            testDB.runCommand(
                {aggregate: "foo", pipeline: [{$match: {_id: 0}}, {$out: "out"}], cursor: {}}),
            ErrorCodes.Unauthorized,
            "user should no longer have write privileges");
        res = assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: "foo"}));
        assert.eq(1, testDB.out.find().itcount());

        //
        // Test that if there were multiple users authenticated when the cursor was created, then at
        // least one of them must be authenticated in order to run getMore on the cursor.
        //

        assert.eq(1, adminDB.auth("admin", "admin"));
        assert.writeOK(testDB.bar.insert({_id: 0}));

        // Create a user "fooUser" on the test database that can read the "foo" collection.
        assert.commandWorked(testDB.runCommand({
            createRole: "readFoo",
            privileges: [{resource: {db: testDBName, collection: "foo"}, actions: ["find"]}],
            roles: []
        }));
        assert.commandWorked(
            testDB.runCommand({createUser: "fooUser", pwd: "pwd", roles: ["readFoo"]}));

        // Create a user "fooBarUser" on the admin database that can read the "foo" and "bar"
        // collections.
        assert.commandWorked(adminDB.runCommand({
            createRole: "readFooBar",
            privileges: [
                {resource: {db: testDBName, collection: "foo"}, actions: ["find"]},
                {resource: {db: testDBName, collection: "bar"}, actions: ["find"]}
            ],
            roles: []
        }));
        assert.commandWorked(
            adminDB.runCommand({createUser: "fooBarUser", pwd: "pwd", roles: ["readFooBar"]}));

        adminDB.logout();

        // Test that a cursor created by "fooUser" and "fooBarUser" can be used by "fooUser".
        assert.eq(1, testDB.auth("fooUser", "pwd"));
        assert.eq(1, adminDB.auth("fooBarUser", "pwd"));
        res = assert.commandWorked(testDB.runCommand({find: "foo", batchSize: 0}));
        cursorId = res.cursor.id;
        adminDB.logout();
        assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: "foo"}));
        testDB.logout();

        // Test that a cursor created by "fooUser" and "fooBarUser" cannot be used by "fooUser" if
        // "fooUser" does not have the privilege to read the collection.
        assert.eq(1, testDB.auth("fooUser", "pwd"));
        assert.eq(1, adminDB.auth("fooBarUser", "pwd"));
        res = assert.commandWorked(testDB.runCommand({find: "bar", batchSize: 0}));
        cursorId = res.cursor.id;
        adminDB.logout();
        assert.commandFailedWithCode(testDB.runCommand({getMore: cursorId, collection: "bar"}),
                                     ErrorCodes.Unauthorized,
                                     "'fooUser' should not be able to read 'bar' collection");
        testDB.logout();

        // Test that an aggregate cursor created by "fooUser" and "fooBarUser" can be used by
        // "fooUser", even if "fooUser" does not have all privileges required by the pipeline. This
        // is not desirable behavior, but it will be resolved when we require that only one user be
        // authenticated at a time.
        assert.eq(1, testDB.auth("fooUser", "pwd"));
        assert.eq(1, adminDB.auth("fooBarUser", "pwd"));
        res = assert.commandWorked(testDB.runCommand({
            aggregate: "foo",
            pipeline: [
                {$match: {_id: 0}},
                {$lookup: {from: "bar", localField: "_id", foreignField: "_id", as: "bar"}}
            ],
            cursor: {batchSize: 0}
        }));
        cursorId = res.cursor.id;
        adminDB.logout();
        res = assert.commandWorked(testDB.runCommand({getMore: cursorId, collection: "foo"}));
        assert.eq(res.cursor.nextBatch, [{_id: 0, bar: [{_id: 0}]}], tojson(res));
        testDB.logout();
    }

    // Run the test on a standalone.
    let conn = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
    runTest(conn);
    MongoRunner.stopMongod(conn);

    // Run the test on a sharded cluster.
    let cluster = new ShardingTest(
        {shards: 1, mongos: 1, keyFile: "jstests/libs/key1", other: {shardOptions: {auth: ""}}});
    runTest(cluster);
    cluster.stop();
}());
