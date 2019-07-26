// Authenticate to a mongod from the shell via command line.

(function() {
'use strict';

const port = allocatePort();
const mongod = MongoRunner.runMongod({auth: '', port: port});
const admin = mongod.getDB('admin');

admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});

// Connect via shell round-trip in order to verify handling of mongodb:// uri with password.
const uri = 'mongodb://admin:pass@localhost:' + port + '/admin';
// Be sure to actually do something requiring authentication.
const mongo = runMongoProgram('mongo', uri, '--eval', 'db.system.users.find({});');
assert.eq(mongo, 0, "Failed connecting to mongod via shell+mongodb uri");

MongoRunner.stopMongod(mongod);
})();
