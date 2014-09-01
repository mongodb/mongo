// Test the db.auth() shell helper.

var conn = MongoRunner.runMongod({ smallfiles: "", auth: "" });

var mechanisms, hasMongoCR, hasCramMd5;

var admin = conn.getDB('admin');
admin.createUser({user:'andy', pwd: 'a', roles: jsTest.adminUserRoles});
admin.auth({user: 'andy', pwd: 'a'});

// Attempt to start with CRAM-MD5 enabled
// If this fails the build only supports default auth mechanisms
MongoRunner.stopMongod(conn);
var restartedConn = MongoRunner.runMongod({ restart: conn,
                               setParameter: "authenticationMechanisms=MONGODB-CR,CRAM-MD5" });
if (restartedConn != null) {
    mechanisms = [ "MONGODB-CR", "CRAM-MD5" ];
    hasMongoCR = true;
    hasCramMd5 = true;
    print("test info: Enabling non-default authentication mechanisms.");
}
else {
    restartedConn = MongoRunner.runMongod({ restart: conn });
    mechanisms = [ "MONGODB-CR" ];
    hasMongoCR = true;
    hasCramMd5 = false;
    print("test info: Using only default authentication mechanism, MONGODB-CR.");
}

admin = restartedConn.getDB('admin');
var testedSomething = false;

// If the server supports them MONGODB-CR, try all the ways to call db.auth that use MONGODB-CR.
if (hasMongoCR) {
    testedSomething = true;
    assert(admin.auth('andy', 'a'));
    admin.logout();
    assert(admin.auth({user: 'andy', pwd: 'a'}));
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
