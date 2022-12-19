(function() {
'use strict';

load("jstests/libs/log.js");  // For findMatchingLogLine.

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
const profileLevelDB = mongo.getDB("keyfile_auth_ensure_correct_username_change");
const globalLog = assert.commandWorked(profileLevelDB.adminCommand({getLog: 'global'}));
const fieldMatcher = {
    msg: "Different user name was supplied to saslSupportedMechs"
};
assert(
    findMatchingLogLine(globalLog.log, fieldMatcher),
    "Did not find log line concerning \"Different user name was supplied to saslSupportedMechs\" when we expected to.");

MongoRunner.stopMongod(mongo);
})();
