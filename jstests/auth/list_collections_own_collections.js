// Test nameOnly option of listCollections
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

function runTestOnConnection(conn) {
    const admin = conn.getDB("admin");
    const db = conn.getDB(dbName);

    assert.commandWorked(admin.runCommand({createUser: "root", pwd: "root", roles: ["root"]}));
    assert(admin.auth("root", "root"));

    function createTestRoleAndUser(roleName, privs) {
        assert.commandWorked(
            admin.runCommand({createRole: roleName, roles: [], privileges: privs}));

        const userName = "user|" + roleName;
        assert.commandWorked(db.runCommand(
            {createUser: userName, pwd: "pwd", roles: [{role: roleName, db: "admin"}]}));
    }

    createTestRoleAndUser("roleWithExactNamespacePrivileges", [
        {resource: {db: dbName, collection: "foo"}, actions: ["find"]},
        {resource: {db: dbName, collection: "bar"}, actions: ["find"]}
    ]);

    createTestRoleAndUser("roleWithExactNamespaceAndSystemPrivileges", [
        {resource: {db: dbName, collection: "foo"}, actions: ["find"]},
        {resource: {db: dbName, collection: "bar"}, actions: ["find"]},
        {resource: {db: dbName, collection: "system.views"}, actions: ["find"]}
    ]);

    createTestRoleAndUser("roleWithCollectionPrivileges", [
        {resource: {db: "", collection: "foo"}, actions: ["find"]},
        {resource: {db: "", collection: "bar"}, actions: ["find"]}
    ]);

    createTestRoleAndUser("roleWithCollectionAndSystemPrivileges", [
        {resource: {db: "", collection: "foo"}, actions: ["find"]},
        {resource: {db: "", collection: "bar"}, actions: ["find"]},
        {resource: {db: "", collection: "system.views"}, actions: ["find"]}
    ]);

    createTestRoleAndUser("roleWithDatabasePrivileges", [
        {resource: {db: dbName, collection: ""}, actions: ["find"]},
    ]);

    createTestRoleAndUser("roleWithDatabaseAndSystemPrivileges", [
        {resource: {db: dbName, collection: ""}, actions: ["find"]},
        {resource: {db: dbName, collection: "system.views"}, actions: ["find"]}
    ]);

    createTestRoleAndUser("roleWithAnyNormalResourcePrivileges", [
        {resource: {db: "", collection: ""}, actions: ["find"]},
    ]);

    createTestRoleAndUser("roleWithAnyNormalResourceAndSystemPrivileges", [
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

    function runTestOnRole(roleName, expectedColls) {
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

        res = db.runCommand({
            listCollections: 1,
            nameOnly: true,
            authorizedCollections: true,
            filter: {"name": "foo"}
        });
        assert.commandWorked(res);
        assert.eq([resFoo], res.cursor.firstBatch);

        db.logout();
    }

    runTestOnRole("roleWithExactNamespacePrivileges", [resFoo, resBar]);
    runTestOnRole("roleWithExactNamespaceAndSystemPrivileges", [resFoo, resBar, resSystemViews]);

    runTestOnRole("roleWithCollectionPrivileges", [resFoo, resBar]);
    runTestOnRole("roleWithCollectionAndSystemPrivileges", [resFoo, resBar, resSystemViews]);

    runTestOnRole("roleWithDatabasePrivileges", [resFoo, resBar, ...resOther]);
    runTestOnRole("roleWithDatabaseAndSystemPrivileges",
                  [resFoo, resBar, ...resOther, resSystemViews]);

    runTestOnRole("roleWithAnyNormalResourcePrivileges", [resFoo, resBar, ...resOther]);
    runTestOnRole("roleWithAnyNormalResourceAndSystemPrivileges",
                  [resFoo, resBar, ...resOther, resSystemViews]);
}

function runNoAuthTestOnConnection(conn) {
    const admin = conn.getDB("admin");
    const db = conn.getDB(dbName);

    assert.commandWorked(db.dropDatabase());
    assert.commandWorked(db.createCollection("foo"));
    assert.commandWorked(db.createView("bar", "foo", []));

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
MongoRunner.stopMongod(mongod);

const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    config: 1,
    other: {keyFile: 'jstests/libs/key1', shardAsReplicaSet: false}
});
runTestOnConnection(st.s0);
st.stop();

const mongodNoAuth = MongoRunner.runMongod();
runNoAuthTestOnConnection(mongodNoAuth);
MongoRunner.stopMongod(mongodNoAuth);

const stNoAuth =
    new ShardingTest({shards: 1, mongos: 1, config: 1, other: {shardAsReplicaSet: false}});
runNoAuthTestOnConnection(stNoAuth.s0);
stNoAuth.stop();
}());
