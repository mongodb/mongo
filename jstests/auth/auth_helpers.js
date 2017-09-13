// Test the db.auth() shell helper.
//
// This test requires users to persist across a restart.
// @tags: [requires_persistence]

var conn = MongoRunner.runMongod({smallfiles: ""});

var mechanisms, hasCR, hasCramMd5;

var admin = conn.getDB('admin');
// In order to test MONGODB-CR we need to "reset" the authSchemaVersion to
// 26Final "3" or else the user won't get MONGODB-CR credentials.
admin.system.version.save({"_id": "authSchema", "currentVersion": 3});
admin.createUser({user: 'andy', pwd: 'a', roles: jsTest.adminUserRoles});
admin.auth({user: 'andy', pwd: 'a'});

// Attempt to start with CRAM-MD5 enabled
// If this fails the build only supports default auth mechanisms
MongoRunner.stopMongod(conn);
var restartedConn = MongoRunner.runMongod({
    auth: "",
    restart: conn,
    setParameter: "authenticationMechanisms=SCRAM-SHA-1,MONGODB-CR,CRAM-MD5"
});
if (restartedConn != null) {
    mechanisms = ["SCRAM-SHA-1", "MONGODB-CR", "CRAM-MD5"];
    hasCR = true;
    hasCramMd5 = true;
    print("test info: Enabling non-default authentication mechanisms.");
} else {
    restartedConn = MongoRunner.runMongod({restart: conn});
    mechanisms = ["SCRAM-SHA-1", "MONGODB-CR"];
    hasCR = true;
    hasCramMd5 = false;
    print("test info: Using only default password authentication mechanisms.");
}

admin = restartedConn.getDB('admin');
var testedSomething = false;

// Try all the ways to call db.auth that uses SCRAM-SHA-1 or MONGODB-CR.
if (hasCR) {
    testedSomething = true;
    assert(admin.auth('andy', 'a'));
    admin.logout();
    assert(admin.auth({user: 'andy', pwd: 'a'}));
    admin.logout();
    assert(admin.auth({mechanism: 'SCRAM-SHA-1', user: 'andy', pwd: 'a'}));
    admin.logout();
    assert(admin.auth({mechanism: 'MONGODB-CR', user: 'andy', pwd: 'a'}));
    admin.logout();
}

// If the server supports CRAM-MD5, try it out.
if (hasCramMd5) {
    testedSomething = true;
    assert(admin.auth({mechanism: 'CRAM-MD5', user: 'andy', pwd: 'a'}));
    admin.logout();
}

// Sanity check that we tested at least one of MONGODB-CR and CRAM-MD5.
assert(testedSomething, "No candidate authentication mechanisms matched.");

// Invalid mechanisms shouldn't lead to authentication, but also shouldn't crash.
assert(!admin.auth({mechanism: 'this-mechanism-is-fake', user: 'andy', pwd: 'a'}));
