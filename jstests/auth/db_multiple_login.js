// Test that only one user may be authenticated against the database at a time.

(function() {
'use strict';

const conn = MongoRunner.runMongod({auth: ''});
const admin = conn.getDB('admin');
const test = conn.getDB('test');

admin.createUser({user: 'admin', pwd: 'pwd', roles: jsTest.adminUserRoles});
assert(admin.auth('admin', 'pwd'));
test.createUser({user: 'user', pwd: 'pwd', roles: ['readWrite']});

jsTest.log('Testing multi-auth');
assert(!test.auth('user', 'pwd'));

jsTest.log('Testing re-auth after logout');
admin.logout();
assert(test.auth('user', 'pwd'));
test.logout();

jsTest.log('Shutting down');
assert(admin.auth('admin', 'pwd'));
MongoRunner.stopMongod(conn);
})();
