// Verify that a user with read access to database "test" cannot access database "test2" via a where
// clause.

import {ShardingTest} from "jstests/libs/shardingtest.js";

//
// User document declarations.  All users in this test are added to the admin database.
//

let adminUser = {
    user: "admin",
    pwd: "a",
    roles: ["readWriteAnyDatabase", "dbAdminAnyDatabase", "userAdminAnyDatabase", "clusterAdmin"],
};

let test1Reader = {
    user: "test",
    pwd: "a",
    roles: [{role: "read", db: "test1", hasRole: true, canDelegate: false}],
};

function assertRemove(collection, pattern) {
    assert.commandWorked(collection.remove(pattern));
}

function assertInsert(collection, obj) {
    assert.commandWorked(collection.insert(obj));
}

let cluster = new ShardingTest({name: "authwhere", shards: 1, mongos: 1, other: {keyFile: "jstests/libs/key1"}});

// Set up the test data.
(function () {
    let adminDB = cluster.getDB("admin");
    let test1DB = adminDB.getSiblingDB("test1");
    let test2DB = adminDB.getSiblingDB("test2");
    let ex;
    try {
        adminDB.createUser(adminUser);
        assert(adminDB.auth(adminUser.user, adminUser.pwd));

        adminDB.dropUser(test1Reader.user);
        adminDB.createUser(test1Reader);

        assertInsert(test1DB.foo, {a: 1});
        assertInsert(test2DB.foo, {x: 1});
    } finally {
        adminDB.logout();
    }
})();

(function () {
    let adminDB = cluster.getDB("admin");
    let test1DB;
    let test2DB;
    assert(adminDB.auth(test1Reader.user, test1Reader.pwd));
    try {
        test1DB = adminDB.getSiblingDB("test1");
        test2DB = adminDB.getSiblingDB("test2");

        // Sanity check.  test1Reader can count (read) test1, but not test2.
        assert.eq(test1DB.foo.count(), 1);
        assert.throws(function () {
            test2DB.foo.count();
        });

        // Cannot examine second database from a where clause.
        assert.throws(function () {
            test1DB.foo.count("db.getSiblingDB('test2').foo.count() == 1");
        });

        // Cannot write test1 via tricky where clause.
        assert.throws(function () {
            test1DB.foo.count("db.foo.insert({b: 1})");
        });
        assert.eq(test1DB.foo.count(), 1);
    } finally {
        adminDB.logout();
    }
})();

cluster.stop();
