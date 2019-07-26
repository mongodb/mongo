/**
 * Tests that when a user who lacks the listCollections privilege types 'db.<tab>' in the shell,
 * autocompletion shows the collections on which she has permissions.
 *
 * @tags: [
 *   assumes_superuser_permissions,
 *   assumes_write_concern_unchanged,
 *   creates_and_authenticates_user,
 *   requires_auth,
 *   requires_non_retryable_commands,
 * ]
 */

// Get shell's global scope.
const self = this;

(function() {
'use strict';

const testName = jsTest.name();
const conn = MongoRunner.runMongod({auth: ''});
const admin = conn.getDB('admin');
admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
assert(admin.auth('admin', 'pass'));

admin.getSiblingDB(testName).createRole({
    role: 'coachTicket',
    privileges: [{resource: {db: testName, collection: 'coachClass'}, actions: ['find']}],
    roles: []
});

admin.getSiblingDB(testName).createUser(
    {user: 'coachPassenger', pwd: 'password', roles: ['coachTicket']});

const testDB = conn.getDB(testName);
testDB.coachClass.insertOne({});
testDB.businessClass.insertOne({});

// Must use 'db' to test autocompletion.
self.db = new Mongo(conn.host).getDB(testName);
assert(db.auth('coachPassenger', 'password'));
const authzErrorCode = 13;
assert.commandFailedWithCode(db.runCommand({listCollections: 1}), authzErrorCode);
assert.commandWorked(db.runCommand({find: 'coachClass'}));
assert.commandFailedWithCode(db.runCommand({find: 'businessClass'}), authzErrorCode);
shellAutocomplete('db.');
assert(__autocomplete__.includes('db.coachClass'),
       `Completions should include 'coachClass': ${__autocomplete__}`);
assert(!__autocomplete__.includes('db.businessClass'),
       `Completions should NOT include 'businessClass': ${__autocomplete__}`);
MongoRunner.stopMongod(conn);
})();
