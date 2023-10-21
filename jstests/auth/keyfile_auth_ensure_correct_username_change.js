import {findMatchingLogLine} from "jstests/libs/log.js";

function assertLog(mongo, shouldExist) {
    const profileLevelDB = mongo.getDB("keyfile_auth_ensure_correct_username_change");
    const globalLog = assert.commandWorked(profileLevelDB.adminCommand({getLog: 'global'}));
    const fieldMatcher = {msg: "Different user name was supplied to saslSupportedMechs"};

    if (shouldExist) {
        assert(
            findMatchingLogLine(globalLog.log, fieldMatcher),
            "Did not find log line concerning \"Different user name was supplied to saslSupportedMechs\" when we expected to.");
    } else {
        assert.eq(
            null,
            findMatchingLogLine(globalLog.log, fieldMatcher),
            "Unexpectedly found log line concerning \"Different user name was supplied to saslSupportedMechs\".");
    }
}

// Switching from __system to another user should result in a warning.
function runSaslSupportedMechsDiffUserTest() {
    const mongo = MongoRunner.runMongod({auth: "", keyFile: "jstests/libs/key1"});
    const admin = mongo.getDB("admin");

    admin.createUser({user: 'user', pwd: 'pass', roles: jsTest.adminUserRoles});

    // The username should be set to __system@local here because of the saslSupportedMechs field..
    assert.commandWorked(admin.runCommand({
        hello: 1,
        saslSupportedMechs: "local.__system",
    }));

    // During this authentication, the username should be set to user@admin.
    assert(admin.auth({user: 'user', pwd: 'pass', mechanism: 'SCRAM-SHA-256'}));

    // We expect to see a log detailing that the username changed during authentication.
    assertLog(mongo, true /* shouldExist */);

    MongoRunner.stopMongod(mongo);
}

// Authenticating directly as __system without saslSupportedMechs should not result in a log.
function runSystemDirectAuthTest() {
    const mongo = MongoRunner.runMongod({auth: "", keyFile: "jstests/libs/key1"});
    const local = mongo.getDB("local");
    assert(local.auth({user: '__system', pwd: 'foopdedoop'}));

    // We don't expect to see a log detailing that the username changed during authentication.
    assertLog(mongo, false /* shouldExist */);

    MongoRunner.stopMongod(mongo);
}

// Authenticating directly as user@admin should not result in a username switch warning.
function runRegularUserDirectAuthTest() {
    const mongo = MongoRunner.runMongod({auth: "", keyFile: "jstests/libs/key1"});
    const admin = mongo.getDB("admin");

    admin.createUser({user: 'user', pwd: 'pass', roles: jsTest.adminUserRoles});
    assert(admin.auth({user: 'user', pwd: 'pass'}));

    // We don't expect to see a log detailing that the username changed during authentication.
    assertLog(mongo, false /* shouldExist */);
    MongoRunner.stopMongod(mongo);
}

runSaslSupportedMechsDiffUserTest();
runSystemDirectAuthTest();
runRegularUserDirectAuthTest();
