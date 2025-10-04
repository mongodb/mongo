/*
 * Tests that resource pattern matching rules work as expected.
 * @tags: [requires_replication, requires_sharding]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test logs in users on the admin database, but doesn't log them out, which can fail with
// implicit sessions and ReplSetTest when the fixture attempts to verify data hashes at shutdown by
// authenticating as the __system user.

TestData.disableImplicitSessions = true;

function setup_users(granter) {
    const admin = granter.getSiblingDB("admin");
    assert.commandWorked(
        admin.runCommand({
            createUser: "admin",
            pwd: "admin",
            roles: ["userAdminAnyDatabase", "dbAdminAnyDatabase", "clusterAdmin", "readWriteAnyDatabase"],
        }),
    );

    assert(admin.auth("admin", "admin"));
    printjson(admin.runCommand({createRole: "test_role", privileges: [], roles: []}));
    printjson(admin.runCommand({createUser: "test_user", pwd: "password", roles: ["test_role"]}));
    admin.logout();
}

function setup_dbs_and_cols(db) {
    const admin = db.getSiblingDB("admin");
    const test_db_a = db.getSiblingDB("a");
    const test_db_b = db.getSiblingDB("b");

    assert(admin.auth("admin", "admin"));
    assert.commandWorked(test_db_a.dropDatabase({w: "majority"}));
    assert.commandWorked(test_db_b.dropDatabase({w: "majority"}));

    assert.commandWorked(test_db_a.createCollection("a", {writeConcern: {w: "majority"}}));
    assert.commandWorked(test_db_a.createCollection("b", {writeConcern: {w: "majority"}}));

    assert.commandWorked(test_db_b.createCollection("a", {writeConcern: {w: "majority"}}));
    assert.commandWorked(test_db_b.createCollection("b", {writeConcern: {w: "majority"}}));
    admin.logout();
}

function grant_privileges(granter, privileges) {
    const admin = granter.getSiblingDB("admin");

    assert(admin.auth("admin", "admin"));
    const result = admin.runCommand({
        grantPrivilegesToRole: "test_role",
        privileges: privileges,
        writeConcern: {w: "majority"},
    });
    admin.logout();
    return result;
}

function revoke_privileges(granter, privileges) {
    const admin = granter.getSiblingDB("admin");

    assert(admin.auth("admin", "admin"));
    const result = admin.runCommand({
        revokePrivilegesFromRole: "test_role",
        privileges: privileges,
        writeConcern: {w: "majority"},
    });
    admin.logout();
    return result;
}

function invalidateUserCache(verifier) {
    const admin = verifier.getSiblingDB("admin");
    assert(admin.auth("admin", "admin"));
    assert.commandWorked(admin.runCommand("invalidateUserCache"));
    admin.logout();
}

function run_test(name, granter, verifier, privileges, collections, rst) {
    print("\n=== testing " + name + "() ===\n");

    grant_privileges(granter, privileges);
    invalidateUserCache(verifier);
    if (rst) {
        rst.awaitReplication();
    }
    const verifierDB = verifier.getSiblingDB("admin");
    assert(verifierDB.auth("test_user", "password"));

    for (let key in collections) {
        const parts = key.split(".");
        const testdb = verifier.getSiblingDB(parts[0]);
        const col = testdb.getCollection(parts[1]);

        const cb = collections[key];

        cb(testdb, col);
    }
    verifierDB.logout();

    revoke_privileges(granter, privileges);
}

function run_test_bad_resource(name, granter, resource) {
    print("\n=== testing resource fail " + name + "() ===\n");
    assert.commandFailed(grant_privileges(granter, [{resource: resource, actions: ["find"]}]));
}

function should_insert(testdb, testcol) {
    assert.doesNotThrow(function () {
        testcol.insert({a: "b"});
    });
}

function should_find(testdb, testcol) {
    assert.doesNotThrow(function () {
        testcol.findOne();
    });
}

function should_fail_find(testdb, testcol) {
    assert.throws(function () {
        testcol.findOne();
    });
}

function run_tests(granter, verifier, rst) {
    setup_users(granter);
    setup_dbs_and_cols(granter);

    run_test(
        "specific",
        granter,
        verifier,
        [{resource: {db: "a", collection: "a"}, actions: ["find"]}],
        {
            "a.a": should_find,
            "a.b": should_fail_find,
            "b.a": should_fail_find,
            "b.b": should_fail_find,
        },
        rst,
    );

    run_test(
        "glob_collection",
        granter,
        verifier,
        [{resource: {db: "a", collection: ""}, actions: ["find"]}],
        {"a.a": should_find, "a.b": should_find, "b.a": should_fail_find, "b.b": should_fail_find},
        rst,
    );

    run_test(
        "glob_database",
        granter,
        verifier,
        [{resource: {db: "", collection: "a"}, actions: ["find"]}],
        {"a.a": should_find, "a.b": should_fail_find, "b.a": should_find, "b.b": should_fail_find},
        rst,
    );

    run_test(
        "glob_all",
        granter,
        verifier,
        [{resource: {db: "", collection: ""}, actions: ["find"]}],
        {"a.a": should_find, "a.b": should_find, "b.a": should_find, "b.b": should_find},
        rst,
    );

    run_test(
        "any_resource",
        granter,
        verifier,
        [{resource: {anyResource: true}, actions: ["find"]}],
        {
            "a.a": should_find,
            "a.b": should_find,
            "b.a": should_find,
            "b.b": should_find,
            "c.a": should_find,
        },
        rst,
    );

    run_test(
        "no_global_access",
        granter,
        verifier,
        [{resource: {db: "$", collection: "cmd"}, actions: ["find"]}],
        {
            "a.a": function (testdb, testcol) {
                let r = testdb.stats();

                if (r["ok"]) throw "db.$.cmd shouldn't give a.stats()";
            },
        },
        rst,
    );

    run_test_bad_resource("empty_resource", granter, {});
    run_test_bad_resource("users_collection_any_db", granter, {collection: "users"});
    run_test_bad_resource("bad_key", granter, {myResource: "users"});
    run_test_bad_resource("extra_key", granter, {db: "test", collection: "users", cluster: true});
    run_test_bad_resource("bad_value_type", granter, {cluster: "false"});
    run_test_bad_resource("bad_collection", granter, {db: "test", collection: "$$$$"});

    run_test(
        "mixed_find_write",
        granter,
        verifier,
        [
            {resource: {db: "a", collection: "a"}, actions: ["find"]},
            {resource: {db: "", collection: ""}, actions: ["insert"]},
        ],
        {
            "a.a": function (testdb, testcol) {
                should_insert(testdb, testcol);
                should_find(testdb, testcol);
            },
            "a.b": function (testdb, testcol) {
                should_insert(testdb, testcol);
                should_fail_find(testdb, testcol);
            },
            "b.a": function (testdb, testcol) {
                should_insert(testdb, testcol);
                should_fail_find(testdb, testcol);
            },
            "b.b": function (testdb, testcol) {
                should_insert(testdb, testcol);
                should_fail_find(testdb, testcol);
            },
        },
        rst,
    );
}

const keyfile = "jstests/libs/key1";

{
    print("--- standalone node test ---");
    const conn = MongoRunner.runMongod({auth: null, keyFile: keyfile});
    run_tests(conn.getDB("test"), conn.getDB("test"));
    MongoRunner.stopMongod(conn);
    print("--- done standalone node test ---");
}

{
    print("--- replica set test ---");
    const rst = new ReplSetTest({name: "testset", nodes: 2, nodeOptions: {"auth": null}, keyFile: keyfile});

    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary().getDB("admin");
    rst.awaitSecondaryNodes();
    const secondary = rst.getSecondaries()[0].getDB("admin");
    run_tests(primary, secondary, rst);
    rst.stopSet();
    print("--- done with the rs tests ---");
}

{
    print("--- sharding test ---");
    const st = new ShardingTest({
        mongos: 2,
        shard: 1,
        keyFile: keyfile,
        other: {
            mongosOptions: {"auth": null},
            configOptions: {"auth": null},
            rsOptions: {"auth": null},
        },
    });
    run_tests(st.s0.getDB("admin"), st.s1.getDB("admin"));
    st.stop();
    print("--- sharding test done ---");
}
