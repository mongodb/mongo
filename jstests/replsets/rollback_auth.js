// Tests rollback of auth data in replica sets.
// This test creates a user and then does two different sets of updates to that user's privileges
// using the replSetTest command to trigger a rollback and verify that at the end the access control
// data is rolled back correctly and the user only has access to the expected collections.

var authzErrorCode = 13;

jsTestLog("Setting up replica set");

var replTest = new ReplSetTest({ name: 'rollbackAuth', nodes: 3, keyFile: 'jstests/libs/key1' });
var nodes = replTest.nodeList();
var conns = replTest.startSet();
replTest.initiate({ "_id": "rollbackAuth",
                    "members": [
                        { "_id": 0, "host": nodes[0] },
                        { "_id": 1, "host": nodes[1] },
                        { "_id": 2, "host": nodes[2], arbiterOnly: true}]
                  });

// Make sure we have a master
var master = replTest.getMaster();
var a_conn = conns[0];
var b_conn = conns[1];
a_conn.setSlaveOk();
b_conn.setSlaveOk();
var A = a_conn.getDB("admin");
var B = b_conn.getDB("admin");
var a = a_conn.getDB("test");
var b = b_conn.getDB("test");
assert(master == conns[0], "conns[0] assumed to be master");
assert(a_conn == master);

// Make sure we have an arbiter
assert.soon(function () {
                var res = conns[2].getDB("admin").runCommand({ replSetGetStatus: 1 });
                return res.myState == 7;
            }, "Arbiter failed to initialize.");


jsTestLog("Creating initial data");

// Create collections that will be used in test
A.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
A.auth('admin', 'pwd');
a.foo.insert({a:1});
a.bar.insert({a:1});
a.baz.insert({a:1});
a.foobar.insert({a:1});

// Set up user admin user

A.createUser({user: 'userAdmin', pwd: 'pwd', roles: ['userAdminAnyDatabase']});
A.auth('userAdmin', 'pwd'); // Logs out of admin@admin user
B.auth('userAdmin', 'pwd');

// Create a basic user and role
A.createRole({role: 'replStatusRole', // To make awaitReplication() work
              roles: [],
              privileges: [{resource: {cluster: true}, actions: ['replSetGetStatus']},
                           {resource: {db: 'local', collection: ''}, actions: ['find']},
                           {resource: {db: 'local', collection: 'system.replset'},
                            actions: ['find']}]});
a.createRole({role: 'myRole', roles: [], privileges: [{resource: {db: 'test', collection: ''},
                                                       actions: ['dbStats']}]});
a.createUser({user: 'spencer',
              pwd: 'pwd',
              roles: ['myRole', {role: 'replStatusRole', db: 'admin'}]});
assert(a.auth('spencer', 'pwd'));

// wait for secondary to get this data
assert.soon(function() {
                return b.auth('spencer', 'pwd');
            });

assert.commandWorked(a.runCommand({dbStats: 1}));
assert.commandFailedWithCode(a.runCommand({collStats: 'foo'}), authzErrorCode);
assert.commandFailedWithCode(a.runCommand({collStats: 'bar'}), authzErrorCode);
assert.commandFailedWithCode(a.runCommand({collStats: 'baz'}), authzErrorCode);
assert.commandFailedWithCode(a.runCommand({collStats: 'foobar'}), authzErrorCode);

assert.commandWorked(b.runCommand({dbStats: 1}));
assert.commandFailedWithCode(b.runCommand({collStats: 'foo'}), authzErrorCode);
assert.commandFailedWithCode(b.runCommand({collStats: 'bar'}), authzErrorCode);
assert.commandFailedWithCode(b.runCommand({collStats: 'baz'}), authzErrorCode);
assert.commandFailedWithCode(b.runCommand({collStats: 'foobar'}), authzErrorCode);


jsTestLog("Doing writes that will eventually be rolled back");

// Blind A
A.runCommand({ replSetTest: 1, blind: true });
reconnect(a);
reconnect(b);

// Wait for B to be master
replTest.waitForState(b_conn, replTest.PRIMARY);
printjson(b.adminCommand('replSetGetStatus'));


// Modify the the user and role in a way that will be rolled back.
b.grantPrivilegesToRole('myRole',
                        [{resource: {db: 'test', collection: 'foo'}, actions: ['collStats']}],
                        {}); // Default write concern will wait for majority, which will time out.
b.createRole({role: 'temporaryRole',
              roles: [],
              privileges: [{resource: {db: 'test', collection: 'bar'}, actions: ['collStats']}]},
             {}); // Default write concern will wait for majority, which will time out.
b.grantRolesToUser('spencer',
                   ['temporaryRole'],
                   {}); // Default write concern will wait for majority, which will time out.


assert.commandWorked(b.runCommand({dbStats: 1}));
assert.commandWorked(b.runCommand({collStats: 'foo'}));
assert.commandWorked(b.runCommand({collStats: 'bar'}));
assert.commandFailedWithCode(b.runCommand({collStats: 'baz'}), authzErrorCode);
assert.commandFailedWithCode(b.runCommand({collStats: 'foobar'}), authzErrorCode);

// a should not have the new data as it was in blind state.
assert.commandWorked(a.runCommand({dbStats: 1}));
assert.commandFailedWithCode(a.runCommand({collStats: 'foo'}), authzErrorCode);
assert.commandFailedWithCode(a.runCommand({collStats: 'bar'}), authzErrorCode);
assert.commandFailedWithCode(a.runCommand({collStats: 'baz'}), authzErrorCode);
assert.commandFailedWithCode(a.runCommand({collStats: 'foobar'}), authzErrorCode);

// Now blind B instead of A
B.runCommand({ replSetTest: 1, blind: true });
reconnect(a);
reconnect(b);

A.runCommand({ replSetTest: 1, blind: false });
reconnect(a);
reconnect(b);

replTest.waitForState(a_conn, replTest.PRIMARY, 60000);


jsTestLog("Doing writes that should persist after the rollback");

// Modify the user and role in a way that will persist.
a.grantPrivilegesToRole('myRole',
                        [{resource: {db: 'test', collection: 'baz'}, actions: ['collStats']}],
                        {}); // Default write concern will wait for majority, which will time out.
a.createRole({role: 'persistentRole',
              roles: [],
              privileges: [{resource: {db: 'test', collection: 'foobar'}, actions: ['collStats']}]},
             {}); // Default write concern will wait for majority, which will time out.
a.grantRolesToUser('spencer',
                   ['persistentRole'],
                   {}); // Default write concern will wait for majority, which will time out.

// A has the data we just wrote, but not what B wrote before
assert.commandWorked(a.runCommand({dbStats: 1}));
assert.commandFailedWithCode(a.runCommand({collStats: 'foo'}), authzErrorCode);
assert.commandFailedWithCode(a.runCommand({collStats: 'bar'}), authzErrorCode);
assert.commandWorked(a.runCommand({collStats: 'baz'}));
assert.commandWorked(a.runCommand({collStats: 'foobar'}));

// B has what it wrote before, but not what A just wrote, since it's still blind
assert.commandWorked(b.runCommand({dbStats: 1}));
assert.commandWorked(b.runCommand({collStats: 'foo'}));
assert.commandWorked(b.runCommand({collStats: 'bar'}));
assert.commandFailedWithCode(b.runCommand({collStats: 'baz'}), authzErrorCode);
assert.commandFailedWithCode(b.runCommand({collStats: 'foobar'}), authzErrorCode);


jsTestLog("Triggering rollback");

// bring B back online
// as A is primary, B will roll back and then catch up
B.runCommand({ replSetTest: 1, blind: false });
reconnect(a);
reconnect(b);
replTest.awaitReplication();
replTest.waitForState(a_conn, replTest.PRIMARY);
replTest.waitForState(b_conn, replTest.SECONDARY);

// Now both A and B should agree
assert.commandWorked(a.runCommand({dbStats: 1}));
assert.commandFailedWithCode(a.runCommand({collStats: 'foo'}), authzErrorCode);
assert.commandFailedWithCode(a.runCommand({collStats: 'bar'}), authzErrorCode);
assert.commandWorked(a.runCommand({collStats: 'baz'}));
assert.commandWorked(a.runCommand({collStats: 'foobar'}));

assert.commandWorked(b.runCommand({dbStats: 1}));
assert.commandFailedWithCode(b.runCommand({collStats: 'foo'}), authzErrorCode);
assert.commandFailedWithCode(b.runCommand({collStats: 'bar'}), authzErrorCode);
assert.commandWorked(b.runCommand({collStats: 'baz'}));
assert.commandWorked(b.runCommand({collStats: 'foobar'}));

replTest.stopSet();