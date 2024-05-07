/**
 * Tests that buildInfo command will fail if the connection is not authenticated.
 */

const conn = MongoRunner.runMongod({auth: ''});
const db = conn.getDB('admin');

assert.commandFailedWithCode(db.runCommand({buildInfo: 1}), ErrorCodes.Unauthorized);

// Create user without any roles, and authenticate. This should be enough to call buildInfo since it
// only checks the connection is authenticated.
db.createUser({user: 'admin', pwd: 'pwd', roles: []});
assert(conn.getDB('admin').auth('admin', 'pwd'));

assert.commandWorked(db.runCommand({buildInfo: 1}));

MongoRunner.stopMongod(conn);
