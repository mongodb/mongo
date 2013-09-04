/*
 * Regression test for SECURITY-27.
 *
 * Verifies that creating a user named "__system" in any database does not get internal system
 * privileges.
 *
 * Operates by creating an "admin" user for set-up, then creating __system users in the "test",
 * "admin" and "local" databases.  Then, it verifies that the __system@local user is shadowed for
 * password and privilege purposes by the keyfile.  It then procedes to verify that the
 * __system@test and __system@admin users are _not_ shadowed in any way by the keyfile user.
 */

(function() {

    "use strict";

    // Runs the "count" command on a database in a way that returns the result document, for easier
    // inspection of the errmsg.
    function runCountCommand(conn, dbName, collectionName) {
        return conn.getDB(dbName).runCommand({ count: collectionName });
    }

    // Asserts that on the given "conn", "dbName"."collectionName".count() fails as unauthorized.
    function assertCountUnauthorized(conn, dbName, collectionName) {
        assert.eq(runCountCommand(conn, dbName, collectionName).code, 13,
                  "On " + dbName + "." + collectionName);
    }

    var conn = MongoRunner.runMongod({ smallfiles: "", auth: "" });

    var admin = conn.getDB('admin');
    var test = conn.getDB('test');
    var local = conn.getDB('local');

    //
    // Preliminary set up.
    //
    admin.addUser('admin', 'a', jsTest.adminUserRoles);
    admin.auth('admin', 'a');

    //
    // Add users named "__system" with no privileges on "test" and "admin", and make sure you can't
    // add one on "local"
    //

    test.addUser({user: '__system', pwd: 'a', roles: []});
    admin.addUser({user: '__system', pwd: 'a', roles: []});
    assert.throws(function() {
        local.addUser({user: '__system', pwd: 'a', roles: []});
    });

    //
    // Add some data to count.
    //

    admin.foo.insert({_id: 1});
    test.foo.insert({_id: 2});
    local.foo.insert({_id: 3});


    admin.logout();
    assertCountUnauthorized(conn, "admin", "foo");
    assertCountUnauthorized(conn, "local", "foo");
    assertCountUnauthorized(conn, "test", "foo");

    //
    // Validate that you cannot even log in as __system@local with the supplied password; you _must_
    // use the password from the keyfile.
    //
    assert(!local.auth('__system', 'a'))
    assertCountUnauthorized(conn, "admin", "foo");
    assertCountUnauthorized(conn, "local", "foo");
    assertCountUnauthorized(conn, "test", "foo");

    //
    // Validate that __system@test is not shadowed by the keyfile __system user.
    //
    test.auth('__system', 'a');
    assertCountUnauthorized(conn, "admin", "foo");
    assertCountUnauthorized(conn, "local", "foo");
    assertCountUnauthorized(conn, "test", "foo");

    test.logout();
    assertCountUnauthorized(conn, "admin", "foo");
    assertCountUnauthorized(conn, "local", "foo");
    assertCountUnauthorized(conn, "test", "foo");

    //
    // Validate that __system@admin is not shadowed by the keyfile __system user.
    //
    admin.auth('__system', 'a');
    assertCountUnauthorized(conn, "admin", "foo");
    assertCountUnauthorized(conn, "local", "foo");
    assertCountUnauthorized(conn, "test", "foo");

    admin.logout();
    assertCountUnauthorized(conn, "admin", "foo");
    assertCountUnauthorized(conn, "local", "foo");
    assertCountUnauthorized(conn, "test", "foo");

})();

