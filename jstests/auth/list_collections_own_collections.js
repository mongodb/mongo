/**
 * Test nameOnly option of listCollections
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */

(function() {
"use strict";

const dbName = "list_collections_own_collections";

const nameSort = (a, b) => a.name > b.name;
const resFoo = {
    "name": "foo",
    "type": "collection"
};
const resBar = {
    "name": "bar",
    "type": "view"
};
const resOther =
    [{"name": "otherCollection", "type": "collection"}, {"name": "otherView", "type": "view"}];
const resSystemViews = {
    "name": "system.views",
    "type": "collection"
};
const resFooTS = {
    "name": "foo",
    "type": "timeseries"
};
const resBarTS = {
    "name": "bar",
    "type": "timeseries"
};
const resSBFoo = {
    "name": "system.buckets.foo",
    "type": "collection"
};
const resSBBar = {
    "name": "system.buckets.bar",
    "type": "collection"
};

function createTestRoleAndUser(db, roleName, privs) {
    const admin = db.getSiblingDB("admin");
    assert.commandWorked(admin.runCommand({createRole: roleName, roles: [], privileges: privs}));

    const userName = "user|" + roleName;
    assert.commandWorked(
        db.runCommand({createUser: userName, pwd: "pwd", roles: [{role: roleName, db: "admin"}]}));
}

function runTestOnRole(db, roleName, expectedColls) {
    jsTestLog(roleName);
    const userName = "user|" + roleName;
    assert(db.auth(userName, "pwd"));

    let res;

    res = db.runCommand({listCollections: 1});
    assert.commandFailed(res);
    res = db.runCommand({listCollections: 1, nameOnly: true});
    assert.commandFailed(res);
    res = db.runCommand({listCollections: 1, authorizedCollections: true});
    assert.commandFailed(res);

    res = db.runCommand({listCollections: 1, nameOnly: true, authorizedCollections: true});
    assert.commandWorked(res);
    assert.eq(expectedColls.sort(nameSort), res.cursor.firstBatch.sort(nameSort));

    res = db.runCommand(
        {listCollections: 1, nameOnly: true, authorizedCollections: true, filter: {"name": "foo"}});
    assert.commandWorked(res);
    if (roleName.indexOf("Buckets") != -1) {
        assert.eq([resFooTS], res.cursor.firstBatch);
    } else {
        assert.eq([resFoo], res.cursor.firstBatch);
    }

    db.logout();
}

function runTestOnConnection(conn) {
    const admin = conn.getDB("admin");
    const db = conn.getDB(dbName);

    assert.commandWorked(admin.runCommand({createUser: "root", pwd: "root", roles: ["root"]}));
    assert(admin.auth("root", "root"));

    createTestRoleAndUser(db, "roleWithExactNamespacePrivileges", [
        {resource: {db: dbName, collection: "foo"}, actions: ["find"]},
        {resource: {db: dbName, collection: "bar"}, actions: ["find"]}
    ]);

    createTestRoleAndUser(db, "roleWithExactNamespaceAndSystemPrivileges", [
        {resource: {db: dbName, collection: "foo"}, actions: ["find"]},
        {resource: {db: dbName, collection: "bar"}, actions: ["find"]},
        {resource: {db: dbName, collection: "system.views"}, actions: ["find"]}
    ]);

    createTestRoleAndUser(db, "roleWithCollectionPrivileges", [
        {resource: {db: "", collection: "foo"}, actions: ["find"]},
        {resource: {db: "", collection: "bar"}, actions: ["find"]}
    ]);

    createTestRoleAndUser(db, "roleWithCollectionAndSystemPrivileges", [
        {resource: {db: "", collection: "foo"}, actions: ["find"]},
        {resource: {db: "", collection: "bar"}, actions: ["find"]},
        {resource: {db: "", collection: "system.views"}, actions: ["find"]}
    ]);

    createTestRoleAndUser(db, "roleWithDatabasePrivileges", [
        {resource: {db: dbName, collection: ""}, actions: ["find"]},
    ]);

    createTestRoleAndUser(db, "roleWithDatabaseAndSystemPrivileges", [
        {resource: {db: dbName, collection: ""}, actions: ["find"]},
        {resource: {db: dbName, collection: "system.views"}, actions: ["find"]}
    ]);

    createTestRoleAndUser(db, "roleWithAnyNormalResourcePrivileges", [
        {resource: {db: "", collection: ""}, actions: ["find"]},
    ]);

    createTestRoleAndUser(db, "roleWithAnyNormalResourceAndSystemPrivileges", [
        {resource: {db: "", collection: ""}, actions: ["find"]},
        {resource: {db: "", collection: "system.views"}, actions: ["find"]}
    ]);

    // Create the collection and view used by the tests.
    assert.commandWorked(db.dropDatabase());
    assert.commandWorked(db.createCollection("foo"));
    assert.commandWorked(db.createView("bar", "foo", []));

    // Create a collection and view that are never granted specific permissions, to ensure
    // they're only returned by listCollections when the role has access to the whole db/server.
    assert.commandWorked(db.createCollection("otherCollection"));
    assert.commandWorked(db.createView("otherView", "otherCollection", []));

    admin.logout();

    runTestOnRole(db, "roleWithExactNamespacePrivileges", [resFoo, resBar]);
    runTestOnRole(
        db, "roleWithExactNamespaceAndSystemPrivileges", [resFoo, resBar, resSystemViews]);

    runTestOnRole(db, "roleWithCollectionPrivileges", [resFoo, resBar]);
    runTestOnRole(db, "roleWithCollectionAndSystemPrivileges", [resFoo, resBar, resSystemViews]);

    runTestOnRole(db, "roleWithDatabasePrivileges", [resFoo, resBar, ...resOther]);
    runTestOnRole(
        db, "roleWithDatabaseAndSystemPrivileges", [resFoo, resBar, ...resOther, resSystemViews]);

    runTestOnRole(db, "roleWithAnyNormalResourcePrivileges", [resFoo, resBar, ...resOther]);
    runTestOnRole(db,
                  "roleWithAnyNormalResourceAndSystemPrivileges",
                  [resFoo, resBar, ...resOther, resSystemViews]);
}

function runSystemsBucketsTestOnConnection(conn, isMongod) {
    const admin = conn.getDB("admin");
    const db = conn.getDB(dbName);

    assert(admin.auth("root", "root"));

    createTestRoleAndUser(db, "roleWithExactNamespacePrivilegesBuckets", [
        {resource: {db: dbName, collection: "foo"}, actions: ["find"]},
    ]);

    createTestRoleAndUser(db, "roleWithExactNamespaceAndSystemPrivilegesBuckets", [
        {resource: {db: dbName, collection: "foo"}, actions: ["find"]},
        {resource: {db: dbName, collection: "bar"}, actions: ["find"]},
        {resource: {db: dbName, collection: "system.buckets.foo"}, actions: ["find"]}
    ]);

    createTestRoleAndUser(db, "roleWithSystemBucketsInAnyDB", [
        {resource: {db: "", collection: "foo"}, actions: ["find"]},
        {resource: {db: "", system_buckets: "foo"}, actions: ["find"]},
        {resource: {db: "", collection: "bar"}, actions: ["find"]}
    ]);

    createTestRoleAndUser(db, "roleWithAnySystemBucketsInDB", [
        {resource: {db: dbName, collection: "foo"}, actions: ["find"]},
        {resource: {db: dbName, system_buckets: ""}, actions: ["find"]},
        {resource: {db: dbName, collection: "bar"}, actions: ["find"]},
    ]);

    createTestRoleAndUser(db, "roleWithAnySystemBuckets", [
        {resource: {db: dbName, collection: "foo"}, actions: ["find"]},
        {resource: {db: "", system_buckets: ""}, actions: ["find"]},
        {resource: {db: dbName, collection: "bar"}, actions: ["find"]},
    ]);

    // Create the collection and view used by the tests.
    assert.commandWorked(db.dropDatabase());
    assert.commandWorked(db.createCollection("foo", {timeseries: {timeField: "date"}}));
    assert.commandWorked(db.createCollection("bar", {timeseries: {timeField: "date"}}));

    // Create a collection and view that are never granted specific permissions, to ensure
    // they're only returned by listCollections when the role has access to the whole db/server.
    assert.commandWorked(db.createCollection("otherCollection"));
    assert.commandWorked(db.createView("otherView", "otherCollection", []));

    admin.logout();

    runTestOnRole(db, "roleWithExactNamespacePrivilegesBuckets", [resFooTS]);
    runTestOnRole(
        db, "roleWithExactNamespaceAndSystemPrivilegesBuckets", [resFooTS, resBarTS, resSBFoo]);

    runTestOnRole(db, "roleWithSystemBucketsInAnyDB", [resFooTS, resBarTS, resSBFoo]);

    runTestOnRole(db, "roleWithAnySystemBucketsInDB", [resFooTS, resBarTS, resSBFoo, resSBBar]);
    runTestOnRole(db, "roleWithAnySystemBuckets", [resFooTS, resBarTS, resSBFoo, resSBBar]);
}

function runNoAuthTestOnConnection(conn) {
    const admin = conn.getDB("admin");
    const db = conn.getDB(dbName);

    assert.commandWorked(db.dropDatabase());
    assert.commandWorked(db.createCollection("foo"));
    assert.commandWorked(db.createView("bar", "foo", []));
    assert.commandWorked(db.createCollection("ts_foo", {timeseries: {timeField: "date"}}));

    var resFull = db.runCommand({listCollections: 1});
    assert.commandWorked(resFull);
    var resAuthColls = db.runCommand({listCollections: 1, authorizedCollections: true});
    assert.commandWorked(resAuthColls);
    assert.eq(resFull.cursor.firstBatch.sort(nameSort),
              resAuthColls.cursor.firstBatch.sort(nameSort));

    var resNameOnly = db.runCommand({listCollections: 1, nameOnly: true});
    assert.commandWorked(resNameOnly);
    var resNameOnlyAuthColls =
        db.runCommand({listCollections: 1, nameOnly: true, authorizedCollections: true});
    assert.commandWorked(resNameOnlyAuthColls);
    assert.eq(resNameOnly.cursor.firstBatch.sort(nameSort),
              resNameOnlyAuthColls.cursor.firstBatch.sort(nameSort));

    var resWithFilter = db.runCommand(
        {listCollections: 1, nameOnly: true, authorizedCollections: true, filter: {"name": "foo"}});
    assert.commandWorked(resWithFilter);
    assert.eq([{"name": "foo", "type": "collection"}], resWithFilter.cursor.firstBatch);
}

const mongod = MongoRunner.runMongod({auth: ''});
runTestOnConnection(mongod);
runSystemsBucketsTestOnConnection(mongod, true);
MongoRunner.stopMongod(mongod);

const st =
    new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
runTestOnConnection(st.s0);
runSystemsBucketsTestOnConnection(st.s0, false);
st.stop();

const mongodNoAuth = MongoRunner.runMongod();
runNoAuthTestOnConnection(mongodNoAuth);
MongoRunner.stopMongod(mongodNoAuth);

const stNoAuth = new ShardingTest({shards: 1, mongos: 1, config: 1});
runNoAuthTestOnConnection(stNoAuth.s0);
stNoAuth.stop();
}());
