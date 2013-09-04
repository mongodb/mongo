// Test the db.auth() shell helper.

var conn = MongoRunner.runMongod({ smallfiles: "", auth: "" });

var mechanisms, hasMongoCR, hasCramMd5;

// Find out if this build supports the authenticationMechanisms startup parameter.  If it does,
// restart with MONGODB-CR and CRAM-MD5 mechanisms enabled.
var cmdOut = conn.getDB('admin').runCommand({getParameter: 1, authenticationMechanisms: 1})
if (cmdOut.ok) {
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({ restart: conn,
                                   setParameter: "authenticationMechanisms=MONGODB-CR,CRAM-MD5" });
    mechanisms = [ "MONGODB-CR", "CRAM-MD5" ];
    hasMongoCR = true;
    hasCramMd5 = true;
    print("test info: Enabling non-default authentication mechanisms.");
}
else {
    mechanisms = [ "MONGODB-CR" ];
    hasMongoCR = true;
    hasCramMd5 = false;
    print("test info: Using only default authentication mechanism, MONGODB-CR.");
}

var admin = conn.getDB('admin');

var testedSomething = false;

admin.addUser('andy', 'a', jsTest.adminUserRoles);

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
