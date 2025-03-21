// Tests that the internal generic request arguments are rejected for non-internal clients
const mongod = MongoRunner.runMongod({auth: ""});
const db = mongod.getDB("test");

// Verify that those arguments are rejected for admins, as they require the internal role
const admin = mongod.getDB('admin');
admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
assert(admin.auth('admin', 'pass'));

assert.commandFailedWithCode(db.runCommand({listCollections: 1, mayBypassWriteBlocking: true}),
                             6317500);
assert.commandFailedWithCode(db.runCommand({listCollections: 1, versionContext: {OFCV: latestFCV}}),
                             9955800);

// Verify that the same commands are allowed with the internal __system role
admin.createUser({user: 'system', pwd: 'pass', roles: ["root", "__system"]});
admin.logout();
assert(admin.auth('system', 'pass'));

assert.commandWorked(db.runCommand({listCollections: 1, mayBypassWriteBlocking: true}));
assert.commandWorked(db.runCommand({listCollections: 1, versionContext: {OFCV: latestFCV}}));

MongoRunner.stopMongod(mongod);
