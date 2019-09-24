// Start a replica set with auth using SCRAM-SHA-256 exclusively,
// then connect via shell.

(function() {

const rsTest = new ReplSetTest({nodes: 3});
rsTest.startSet({
    oplogSize: 10,
    keyFile: 'jstests/libs/key1',
    setParameter: {authenticationMechanisms: 'SCRAM-SHA-256'}
});
rsTest.initiate();
rsTest.awaitSecondaryNodes();

// Setup initial data.
const primary = rsTest.getPrimary();
const admin = primary.getDB('admin');
admin.createUser({user: 'admin', pwd: 'password', roles: jsTest.adminUserRoles});
admin.auth('admin', 'password');
admin.logout();

// Fetch and rearrange connection string.
const connString = rsTest.getURL();
const slash = connString.indexOf('/');
const rsName = connString.substr(0, slash);
const rsHosts = connString.substr(slash + 1);

// Connect with shell using connString.
const csShell = runMongoProgram('./mongo',
                                '--host',
                                connString,
                                '-u',
                                'admin',
                                '--password',
                                'password',
                                '--authenticationDatabase',
                                'admin',
                                '--eval',
                                ';');
assert.eq(csShell, 0, 'Failed to connect using connection string');

// Connect with shell explicitly specifying mechanism.
const csShellMech = runMongoProgram('./mongo',
                                    '--host',
                                    connString,
                                    '-u',
                                    'admin',
                                    '--password',
                                    'password',
                                    '--authenticationDatabase',
                                    'admin',
                                    '--authenticationMechanism',
                                    'SCRAM-SHA-256',
                                    '--eval',
                                    ';');
assert.eq(csShellMech, 0, 'Failed to connect using connection string');

// Connect with shell using URI.
const uriString = 'mongodb://admin:password@' + rsHosts + '/admin?replicaSet=' + rsName;
const uriShell = runMongoProgram('./mongo', uriString, '--eval', ';');
assert.eq(uriShell, 0, 'Failed to connect using URI');

// Connect with shell using URI and explcit mechanism.
const uriShellMech =
    runMongoProgram('./mongo', uriString + '&authMechanism=SCRAM-SHA-256', '--eval', ';');
assert.eq(uriShellMech, 0, 'Failed to connect using URI');

rsTest.stopSet();
})();
