// Tests that users are not authorized to use the internal collMod parameter used during FCV upgrade
// to remove the legacy timeseries bucketing parameters have changed flag
const mongod = MongoRunner.runMongod({auth: ""});
const admin = mongod.getDB('admin');

admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
assert(admin.auth('admin', 'pass'));

const db = mongod.getDB("test");
assert.commandWorked(db.createCollection("dummy"));
assert.commandFailedWithCode(
    db.runCommand({collMod: "dummy", _removeLegacyTimeseriesBucketingParametersHaveChanged: true}),
    ErrorCodes.Unauthorized);

MongoRunner.stopMongod(mongod);
