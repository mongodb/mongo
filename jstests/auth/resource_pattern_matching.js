/*
 * Tests that resource pattern matching rules work as expected.
 */

function setup_users(granter) {
    var admindb = granter.getSiblingDB("admin");
    admindb.runCommand({
        createUser: "admin",
        pwd: "admin",
        roles: [
            "userAdminAnyDatabase",
            "dbAdminAnyDatabase",
            "clusterAdmin",
            "readWriteAnyDatabase"
        ]
    });

    admindb.auth("admin", "admin");

    printjson(admindb.runCommand({createRole: "test_role", privileges: [], roles: []}));

    printjson(admindb.runCommand({createUser: "test_user", pwd: "password", roles: ["test_role"]}));
}

function setup_dbs_and_cols(db) {
    var test_db_a = db.getSiblingDB("a");
    var test_db_b = db.getSiblingDB("b");

    test_db_a.dropDatabase();
    test_db_b.dropDatabase();

    test_db_a.createCollection("a");
    test_db_a.createCollection("b");

    test_db_b.createCollection("a");
    test_db_b.createCollection("b");

    db.getLastError('majority');
}

function grant_privileges(granter, privileges) {
    var admindb = granter.getSiblingDB("admin");

    admindb.auth("admin", "admin");

    var result = admindb.runCommand({
        grantPrivilegesToRole: "test_role",
        privileges: privileges,
        writeConcern: {w: 'majority'}
    });

    admindb.logout();

    return result;
}

function revoke_privileges(granter, privileges) {
    var admindb = granter.getSiblingDB("admin");

    admindb.auth("admin", "admin");

    var result = admindb.runCommand({
        revokePrivilegesFromRole: "test_role",
        privileges: privileges,
        writeConcern: {w: 'majority'}
    });

    admindb.logout();

    return result;
}

function invalidateUserCache(verifier) {
    var admindb = verifier.getSiblingDB("admin");
    admindb.auth('admin', 'admin');
    admindb.runCommand("invalidateUserCache");
    admindb.logout();
}

function run_test(name, granter, verifier, privileges, collections) {
    print("\n=== testing " + name + "() ===\n");

    grant_privileges(granter, privileges);
    invalidateUserCache(verifier);

    verifier.getSiblingDB('admin').auth("test_user", "password");

    for (var key in collections) {
        var parts = key.split(".");
        var testdb = verifier.getSiblingDB(parts[0]);
        var col = testdb.getCollection(parts[1]);

        var cb = collections[key];

        cb(testdb, col);
    }

    revoke_privileges(granter, privileges);
}

function run_test_bad_resource(name, granter, resource) {
    print("\n=== testing resource fail " + name + "() ===\n");
    var admindb = granter.getSiblingDB("admin");
    assert.commandFailed(grant_privileges(granter, [{resource: resource, actions: ["find"]}]));
}

function should_insert(testdb, testcol) {
    assert.doesNotThrow(function() {
        testcol.insert({a: "b"});
    });
}

function should_fail_insert(testdb, testcol) {
    assert.throws(function() {
        testcol.insert({a: "b"});
    });
}

function should_find(testdb, testcol) {
    assert.doesNotThrow(function() {
        testcol.findOne();
    });
}

function should_fail_find(testdb, testcol) {
    assert.throws(function() {
        testcol.findOne();
    });
}

function run_tests(granter, verifier) {
    setup_users(granter);
    setup_dbs_and_cols(granter);

    run_test("specific",
             granter,
             verifier,
             [{resource: {db: "a", collection: "a"}, actions: ["find"]}],
             {
               "a.a": should_find,
               "a.b": should_fail_find,
               "b.a": should_fail_find,
               "b.b": should_fail_find
             });

    run_test(
        "glob_collection",
        granter,
        verifier,
        [{resource: {db: "a", collection: ""}, actions: ["find"]}],
        {"a.a": should_find, "a.b": should_find, "b.a": should_fail_find, "b.b": should_fail_find});

    run_test(
        "glob_database",
        granter,
        verifier,
        [{resource: {db: "", collection: "a"}, actions: ["find"]}],
        {"a.a": should_find, "a.b": should_fail_find, "b.a": should_find, "b.b": should_fail_find});

    run_test("glob_all",
             granter,
             verifier,
             [{resource: {db: "", collection: ""}, actions: ["find"]}],
             {"a.a": should_find, "a.b": should_find, "b.a": should_find, "b.b": should_find});

    run_test(
        "any_resource", granter, verifier, [{resource: {anyResource: true}, actions: ["find"]}], {
            "a.a": should_find,
            "a.b": should_find,
            "b.a": should_find,
            "b.b": should_find,
            "c.a": should_find
        });

    run_test("no_global_access",
             granter,
             verifier,
             [{resource: {db: "$", collection: "cmd"}, actions: ["find"]}],
             {
               "a.a": function(testdb, testcol) {
                   var r = testdb.stats();

                   if (r["ok"])
                       throw("db.$.cmd shouldn't give a.stats()");
               }
             });

    run_test_bad_resource("empty_resource", granter, {});
    run_test_bad_resource("users_collection_any_db", granter, {collection: "users"});
    run_test_bad_resource("bad_key", granter, {myResource: "users"});
    run_test_bad_resource("extra_key", granter, {db: "test", collection: "users", cluster: true});
    run_test_bad_resource("bad_value_type", granter, {cluster: "false"});
    run_test_bad_resource("bad_collection", granter, {db: "test", collection: "$$$$"});

    run_test("mixed_find_write",
             granter,
             verifier,
             [
               {resource: {db: "a", collection: "a"}, actions: ["find"]},
               {resource: {db: "", collection: ""}, actions: ["insert"]}
             ],
             {
               "a.a": function(testdb, testcol) {
                   should_insert(testdb, testcol);
                   should_find(testdb, testcol);
               },
               "a.b": function(testdb, testcol) {
                   should_insert(testdb, testcol);
                   should_fail_find(testdb, testcol);
               },
               "b.a": function(testdb, testcol) {
                   should_insert(testdb, testcol);
                   should_fail_find(testdb, testcol);
               },
               "b.b": function(testdb, testcol) {
                   should_insert(testdb, testcol);
                   should_fail_find(testdb, testcol);
               },
             });
}

var keyfile = "jstests/libs/key1";

print('--- standalone node test ---');
var conn = MongoRunner.runMongod({auth: null, keyFile: keyfile});
run_tests(conn.getDB('test'), conn.getDB('test'));
MongoRunner.stopMongod(conn);
print('--- done standalone node test ---');

print('--- replica set test ---');
var rst = new ReplSetTest({
    name: 'testset',
    nodes: 2,
    nodeOptions: {'auth': null, 'httpinterface': null, 'keyFile': keyfile}
});

rst.startSet();
rst.initiate();
var primary = rst.getPrimary().getDB('admin');
rst.awaitSecondaryNodes();
var secondary = rst.getSecondaries()[0].getDB('admin');
run_tests(primary, secondary);
rst.stopSet();
print('--- done with the rs tests ---');

print('--- sharding test ---');
var st = new ShardingTest({
    mongos: 2,
    shard: 1,
    keyFile: keyfile,
    other: {
        mongosOptions: {'auth': null, 'httpinterface': null},
        configOptions: {'auth': null, 'httpinterface': null},
        shardOptions: {'auth': null, 'httpinterface': null}
    }
});
run_tests(st.s0.getDB('admin'), st.s1.getDB('admin'));
st.stop();
print('--- sharding test done ---');
