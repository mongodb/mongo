// Test creating and authenticating users with special characters.

let conn = MongoRunner.runMongod({auth: ""});

let adminDB = conn.getDB("admin");
adminDB.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});

let testUserSpecialCharacters = function () {
    // Create a user with special characters, make sure it can auth.
    assert(adminDB.auth("admin", "pass"));
    adminDB.createUser({user: '~`!@#$%^&*()-_+={}[]||;:",.//><', pwd: "pass", roles: jsTest.adminUserRoles});
    assert(adminDB.logout());

    assert(adminDB.auth({user: '~`!@#$%^&*()-_+={}[]||;:",.//><', pwd: "pass"}));
    assert(adminDB.logout());
};
testUserSpecialCharacters();

let testUserAndDatabaseAtSymbolConflation = function () {
    // Create a pair of users and databases such that their string representations are
    // identical.
    assert(adminDB.auth("admin", "pass"));

    let bcDB = conn.getDB("b@c");
    bcDB.createUser({user: "a", pwd: "pass2", roles: [{role: "readWrite", db: "b@c"}]});

    let cDB = conn.getDB("c");
    cDB.createUser({user: "a@b", pwd: "pass1", roles: [{role: "readWrite", db: "c"}]});

    assert(adminDB.logout());

    // Ensure they cannot authenticate to the wrong database.
    assert(!bcDB.auth("a@b", "pass1"));
    assert(!bcDB.auth("a@b", "pass2"));
    assert(!cDB.auth("a", "pass1"));
    assert(!cDB.auth("a", "pass2"));

    // Ensure that they can both successfully authenticate to their correct database.
    assert(cDB.auth("a@b", "pass1"));
    assert.commandWorked(cDB.col.insert({data: 1}));
    assert.writeError(bcDB.col.insert({data: 2}));
    assert(cDB.logout());

    assert(bcDB.auth("a", "pass2"));
    assert.commandWorked(bcDB.col.insert({data: 3}));
    assert.writeError(cDB.col.insert({data: 4}));
    assert(bcDB.logout());
};
testUserAndDatabaseAtSymbolConflation();

MongoRunner.stopMongod(conn);
