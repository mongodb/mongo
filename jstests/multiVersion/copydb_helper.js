// SERVER-36438 Ensure the 4.2 copyDatabase() shell helper still successfully executes the copyDB
// command on a 4.0 server, now that the copyDB command has been removed as of 4.2.
(function() {
"use strict";
const oldVersion = "4.0";

let runTest = function(useAuth) {
    let conn;
    if (useAuth) {
        conn = MongoRunner.runMongod({auth: "", binVersion: oldVersion});
    } else {
        conn = MongoRunner.runMongod({binVersion: oldVersion});
    }

    let fromDB = conn.getDB("copydb2-test-a");
    let toDB = conn.getDB("copydb2-test-b");
    let adminDB = conn.getDB("admin");

    if (useAuth) {
        adminDB.createUser({user: "root", pwd: "root", roles: ["root"]});
        adminDB.auth("root", "root");
        fromDB.createUser(
            {user: "chevy", pwd: "chase", roles: ["read", {role: "readWrite", db: toDB._name}]});
    }

    assert.commandWorked(fromDB.foo.insert({a: 1}));
    assert.commandWorked(fromDB.foo.createIndex({a: 1}));

    if (useAuth) {
        assert.commandWorked(toDB.getSiblingDB("admin").logout());
        fromDB.auth("chevy", "chase");
    }

    assert.eq(1, fromDB.foo.count());
    assert.eq(0, toDB.foo.count());

    assert.commandWorked(fromDB.copyDatabase(fromDB._name, toDB._name));
    assert.eq(1, fromDB.foo.count());
    assert.eq(1, toDB.foo.count());
    assert.eq(fromDB.foo.getIndexes().length, toDB.foo.getIndexes().length);
    MongoRunner.stopMongod(conn);
};

runTest(/*useAuth*/ false);

// Authenticating as multiple users on multiple databases results in an error.
if (!jsTest.options().auth) {
    runTest(/*useAuth*/ true);
}
})();
