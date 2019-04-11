// Test listCollections with unauthorized views.
(function() {
    "use strict";

    const dbName = "list_collections_filter_views";

    function runTestOnConnection(conn) {
        const admin = conn.getDB("admin");
        const db = conn.getDB("test");

        assert.commandWorked(admin.runCommand({createUser: "root", pwd: "root", roles: ["root"]}));
        assert(admin.auth("root", "root"));

        assert.commandWorked(db.foo.insert({x: 123}));
        assert.commandWorked(db.createView("bar", "foo", []));
        assert.commandWorked(db.createView("baz", "foo", []));

        assert.commandWorked(db.runCommand({
            createRole: "role",
            roles: [],
            privileges: [
                {resource: {db: "test", collection: "foo"}, actions: ["find"]},
                {resource: {db: "test", collection: "bar"}, actions: ["find"]}
            ]
        }));

        assert.commandWorked(
            db.runCommand({createUser: "user", pwd: "pwd", roles: [{role: "role", db: "test"}]}));
        admin.logout();

        assert(db.auth("user", "pwd"));

        const res = assert.commandWorked(
            db.runCommand({listCollections: 1, nameOnly: true, authorizedCollections: true}));
        assert.eq(2, res.cursor.firstBatch.length, tojson(res.cursor.firstBatch));

        function nameSort(a, b) {
            return a.name > b.name;
        }
        assert.eq(
            [{"name": "bar", "type": "view"}, {"name": "foo", "type": "collection"}].sort(nameSort),
            res.cursor.firstBatch.sort(nameSort));
    }

    const mongod = MongoRunner.runMongod({auth: ''});
    runTestOnConnection(mongod);
    MongoRunner.stopMongod(mongod);

    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        other: {keyFile: 'jstests/libs/key1', shardAsReplicaSet: false},
    });
    runTestOnConnection(st.s0);
    st.stop();

}());
