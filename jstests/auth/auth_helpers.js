// Test the db.auth() shell helper.

var conn = MongoRunner.runMongod({ smallfiles: "", auth: "" });

var mechanisms, hasMongoCR, hasCramMd5;

// Find out if this build supports the authenticationMechanisms startup parameter.  If it does,
// restart with MONGO-CR and CRAM-MD5 mechanisms enabled.
var cmdOut = conn.getDB('admin').runCommand({getParameter: 1, authenticationMechanisms: 1})
if (cmdOut.ok) {
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({ restart: conn,
                                   setParameter: "authenticationMechanisms=MONGO-CR,CRAM-MD5" });
    mechanisms = [ "MONGO-CR", "CRAM-MD5" ];
    hasMongoCR = true;
    hasCramMd5 = true;
    print("test info: Enabling non-default authentication mechanisms.");
}
else {
    mechanisms = [ "MONGO-CR" ];
    hasMongoCR = true;
    hasCramMd5 = false;
    print("test info: Using only default authentication mechanism, MONGO-CR.");
}

var admin = conn.getDB('admin');

var testedSomething = false;

admin.addUser('andy', 'a');

// If the server supports them MONGO-CR, try all the ways to call db.auth that use MONGO-CR.
if (hasMongoCR) {
    testedSomething = true;
    assert(admin.auth('andy', 'a'));
    admin.logout();
    assert(admin.auth({user: 'andy', pwd: 'a'}));
    admin.logout();
    assert(admin.auth({mechanism: 'MONGO-CR', user: 'andy', pwd: 'a'}));
    admin.logout();
}

// If the server supports CRAM-MD5 and the shell supports sasl, try it out.
if (hasCramMd5 && conn.saslAuthenticate) {
    testedSomething = true;
    assert(admin.auth({mechanism: 'CRAM-MD5', user: 'andy', pwd: 'a'}));
    admin.logout();
}

// Sanity check that we tested at least one of MONGO-CR and CRAM-MD5.
assert(testedSomething, "No candidate authentication mechanisms matched.");

// If the shell doesn't support sasl authentication, it shouldn't be able to do CRAM-MD5,
// but shouldn't crash.
if (!conn.saslAuthenticate) {
    assert(!admin.auth({mechanism: 'CRAM-MD5', user: 'andy', pwd: 'a'}));
}
