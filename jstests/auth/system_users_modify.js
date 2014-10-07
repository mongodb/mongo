// SERVER-15500
// Test various auth scenarios, when directly modifying system.users

// Try to authenticate user
// Value determines if auth should be expected to pass or fail
function authUser(value, conn, user, pwd) {
    var message = "Auth for user "+user+" on connection "+tojson(conn);
    assert.eq(conn.auth(user, pwd), value, message);
}

// Test steps
//  - createRole, if required
//  - createUser
//  - Update system.users for specified user, if required
//  - auth user
function runTest(test) {

    jsTest.log("Test:"+tojson(test));
    authUser(authSucceed, admin, "admin", "admin");
    if (test.role) {
        assert.doesNotThrow(
            function () {test.userdb.createRole(test.role);},
            [],
            "Create Role "+tojson(test.role));
    }
    // Create user
    assert.doesNotThrow(
        function () {test.userdb.createUser(test.user);},
        [],
        "Create User "+tojson(test.user));

    // Try to authenticate - should always succeed
    authUser(authSucceed, test.userdb, test.user.user, test.user.pwd);

    // Not all test cases will have updates to user doc
    for (var i=0; i< test.updates.length; i++) {
        // Directly update the user doc
        test.admin.system.users.update({user: test.user.user}, test.updates[i]);
        // Try to authenticate - test defines success
        authUser(test.result, test.userdb, test.user.user, test.user.pwd);
    }
}

var conn = MongoRunner.runMongod({auth : ""});
var dbName = "server15500";
var admin = conn.getDB("admin");
var userdb = conn.getDB(dbName);
var adminUser = {user: "admin", pwd: "admin", roles: ["__system"]};
var authSucceed = 1;
var authFail = 0;

// Tests to run
var tests = [
    {name: "Valid user",
     role: {role: "userRole",
            privileges: [{resource: {db: dbName, collection: ""},
                          actions: ["find", "update", "insert"]}],
            roles: [{role: "readWrite", db: dbName}]},
     user: {user: "user1", pwd: "user1", roles: [{role: "userRole", db: dbName}]},
     updates: [{$set: {roles: [{role: "readWrite", db: dbName}]}}],
     result: authSucceed,
     admin: admin,
     userdb: userdb
    },
    {name: "User without roles",
     user: {user: "noroles", pwd: "noroles", roles: ["readWrite"]},
     updates: [{$unset: {roles: ""}}],
     result: authFail,
     admin: admin,
     userdb: userdb
    },
    {name: "User with invalid roles",
     user: {user: "badroles", pwd: "badroles", roles: ["readWrite"]},
     updates: [{$set: {roles: "non-array"}},
               {$set: {roles: 34}},
               {$set: {roles: {}}}],
     result: authFail,
     admin: admin,
     userdb: userdb
    },
    {name: "User without credentials",
     user: {user: "nocred", pwd: "nocred", roles: ["readWrite"]},
     updates: [{$unset: {credentials: ""}}],
     result: authFail,
     admin: admin,
     userdb: userdb
    },
    {name: "User invalid credentials",
     user: {user: "badcred", pwd: "badcred", roles: ["readWrite"]},
     updates: [{$set: {credentials: {"MONGODB-CR": "badcred"}}},
               {$set: {credentials: {"MONGODB": "badcred"}}}],
     result: authFail,
     admin: admin,
     userdb: userdb
    },
    {name: "User without db",
     user: {user: "nodb", pwd: "nodb", roles: ["readWrite"]},
     updates: [{$unset: {db: ""}}],
     result: authFail,
     admin: admin,
     userdb: userdb
    },
    {name: "User invalid db",
     user: {user: "baddb", pwd: "baddb", roles: ["readWrite"]},
     updates: [{$set: {db: null}},
               {$set: {db: {}}},
               {$set: {db: []}},
               {$set: {db: 34}}],
     result: authFail,
     admin: admin,
     userdb: userdb
    },
];

admin.createUser(adminUser);

// Execute all tests
tests.forEach(function(test) {
    authUser(authSucceed, admin, "admin", "admin");
    runTest(test);
});

// Make sure that we can still auth with a valid user
authUser(authSucceed, userdb, "user1", "user1");

// Remove custom roles
userdb.dropAllRoles();

// Remove all users
admin.system.users.remove({});
