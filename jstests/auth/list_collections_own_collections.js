// Test nameOnly option of listCollections
(function() {
    "use strict";

    const dbName = "list_collections_own_collections";

    function runTestOnConnection(conn) {
        const admin = conn.getDB("admin");
        const db = conn.getDB(dbName);

        assert.commandWorked(admin.runCommand({createUser: "root", pwd: "root", roles: ["root"]}));
        assert(admin.auth("root", "root"));

        assert.commandWorked(admin.runCommand({
            createRole: "roleWithExactNamespacePrivileges",
            roles: [],
            privileges: [
                {resource: {db: dbName, collection: "foo"}, actions: ["createCollection"]},
                {resource: {db: dbName, collection: "bar"}, actions: ["createCollection"]}
            ]
        }));

        assert.commandWorked(admin.runCommand({
            createRole: "roleWithExactNamespaceAndSystemPrivileges",
            roles: [],
            privileges: [
                {resource: {db: dbName, collection: "foo"}, actions: ["createCollection"]},
                {resource: {db: dbName, collection: "bar"}, actions: ["createCollection"]},
                {
                  resource: {db: dbName, collection: "system.views"},
                  actions: ["createCollection"]
                }
            ]
        }));

        assert.commandWorked(admin.runCommand({
            createRole: "roleWithCollectionPrivileges",
            roles: [],
            privileges: [
                {resource: {db: "", collection: "foo"}, actions: ["createCollection"]},
                {resource: {db: "", collection: "bar"}, actions: ["createCollection"]}
            ]
        }));

        assert.commandWorked(admin.runCommand({
            createRole: "roleWithDatabasePrivileges",
            roles: [],
            privileges: [
                {resource: {db: dbName, collection: ""}, actions: ["createCollection"]},
            ]
        }));

        assert.commandWorked(admin.runCommand({
            createRole: "roleWithAnyNormalResourcePrivileges",
            roles: [],
            privileges: [
                {resource: {db: "", collection: ""}, actions: ["createCollection"]},
            ]
        }));
        admin.logout();

        function runTestOnRole(roleName) {
            assert(admin.auth("root", "root"));
            assert.commandWorked(db.dropDatabase());

            const userName = "user|" + roleName;
            assert.commandWorked(db.runCommand(
                {createUser: userName, pwd: "pwd", roles: [{role: roleName, db: "admin"}]}));
            admin.logout();

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
            assert.eq(0, res.cursor.firstBatch.length);

            assert.commandWorked(db.createCollection("foo"));

            res = db.runCommand({listCollections: 1, nameOnly: true, authorizedCollections: true});
            assert.commandWorked(res);
            assert.eq(1, res.cursor.firstBatch.length);
            assert.eq([{"name": "foo", "type": "collection"}], res.cursor.firstBatch);

            assert.commandWorked(db.createView("bar", "foo", []));

            res = db.runCommand({listCollections: 1, nameOnly: true, authorizedCollections: true});
            assert.commandWorked(res);
            assert.eq(2, res.cursor.firstBatch.length, tojson(res.cursor.firstBatch));
            const sortFun = function(a, b) {
                return a.name > b.name;
            };
            assert.eq([{"name": "bar", "type": "view"}, {"name": "foo", "type": "collection"}].sort(
                          sortFun),
                      res.cursor.firstBatch.sort(sortFun));

            res = db.runCommand({
                listCollections: 1,
                nameOnly: true,
                authorizedCollections: true,
                filter: {"name": "foo"}
            });
            assert.commandWorked(res);
            assert.eq(1, res.cursor.firstBatch.length);
            assert.eq([{"name": "foo", "type": "collection"}], res.cursor.firstBatch);

            db.logout();
        }

        runTestOnRole("roleWithExactNamespacePrivileges");
        runTestOnRole("roleWithExactNamespaceAndSystemPrivileges");
        runTestOnRole("roleWithCollectionPrivileges");
        runTestOnRole("roleWithDatabasePrivileges");
        runTestOnRole("roleWithAnyNormalResourcePrivileges");
    }

    const mongod = MongoRunner.runMongod({auth: ''});
    runTestOnConnection(mongod);
    MongoRunner.stopMongod(mongod);

    // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        other: {keyFile: 'jstests/libs/key1', shardAsReplicaSet: false}
    });
    runTestOnConnection(st.s0);
    st.stop();

}());
