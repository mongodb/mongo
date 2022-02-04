// Tests rollback of auth data in replica sets.
// This test creates a user and then does two different sets of updates to that user's privileges
// using the replSetTest command to trigger a rollback and verify that at the end the access control
// data is rolled back correctly and the user only has access to the expected collections.
//
// If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
// not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
// scenario, none of the members will have any data, and upon restart will each look for a member to
// inital sync from, so no primary will be elected. This test induces such a scenario, so cannot be
// run on ephemeral storage engines.
// @tags: [requires_persistence]

(function() {
"use strict";

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

// helper function for verifying contents at the end of the test
function checkFinalResults(db) {
    assert.commandWorked(db.runCommand({dbStats: 1}));
    assert.commandFailedWithCode(db.runCommand({collStats: 'foo'}), authzErrorCode);
    assert.commandFailedWithCode(db.runCommand({collStats: 'bar'}), authzErrorCode);
    assert.commandWorked(db.runCommand({collStats: 'baz'}));
    assert.commandWorked(db.runCommand({collStats: 'foobar'}));
}

const authzErrorCode = 13;

jsTestLog("Setting up replica set");

const name = "rollbackAuth";
const replTest = new ReplSetTest({name: name, nodes: 3, keyFile: 'jstests/libs/key1'});
const nodes = replTest.nodeList();
const conns = replTest.startSet();
replTest.initiate({
    "_id": "rollbackAuth",
    "members": [
        {"_id": 0, "host": nodes[0], "priority": 3},
        {"_id": 1, "host": nodes[1]},
        {"_id": 2, "host": nodes[2], arbiterOnly: true}
    ]
});

// Make sure we have a primary
replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
const primary = replTest.getPrimary();
const a_conn = conns[0];
const b_conn = conns[1];
a_conn.setSecondaryOk();
b_conn.setSecondaryOk();
const A_admin = a_conn.getDB("admin");
const B_admin = b_conn.getDB("admin");
const A_test = a_conn.getDB("test");
const B_test = b_conn.getDB("test");
assert.eq(primary, conns[0], "conns[0] assumed to be primary");
assert.eq(a_conn, primary);

// Make sure we have an arbiter
assert.soon(function() {
    const res = conns[2].getDB("admin").runCommand({replSetGetStatus: 1});
    return res.myState == 7;
}, "Arbiter failed to initialize.");

jsTestLog("Creating initial data");

// Create collections that will be used in test
A_admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
A_admin.auth('admin', 'pwd');

// Set up user admin user
A_admin.createUser({user: 'userAdmin', pwd: 'pwd', roles: ['userAdminAnyDatabase']});

A_test.foo.insert({a: 1});
A_test.bar.insert({a: 1});
A_test.baz.insert({a: 1});
A_test.foobar.insert({a: 1});
A_admin.logout();

assert(A_admin.auth('userAdmin', 'pwd'));

// Give replication time to catch up.
assert.soon(function() {
    return B_admin.auth('userAdmin', 'pwd');
});

// Create a basic user and role
A_admin.createRole({
    role: 'replStatusRole',  // To make awaitReplication() work
    roles: [],
    privileges: [
        {resource: {cluster: true}, actions: ['replSetGetStatus']},
        {resource: {db: 'local', collection: ''}, actions: ['find']},
        {resource: {db: 'local', collection: 'system.replset'}, actions: ['find']}
    ]
});
A_test.createRole({
    role: 'myRole',
    roles: [],
    privileges: [{resource: {db: 'test', collection: ''}, actions: ['dbStats']}]
});
A_test.createUser(
    {user: 'spencer', pwd: 'pwd', roles: ['myRole', {role: 'replStatusRole', db: 'admin'}]});

A_admin.logout();
B_admin.logout();

assert(A_test.auth('spencer', 'pwd'));

// wait for secondary to get this data
assert.soon(function() {
    return B_test.auth('spencer', 'pwd');
});

assert.commandWorked(A_test.runCommand({dbStats: 1}));
assert.commandFailedWithCode(A_test.runCommand({collStats: 'foo'}), authzErrorCode);
assert.commandFailedWithCode(A_test.runCommand({collStats: 'bar'}), authzErrorCode);
assert.commandFailedWithCode(A_test.runCommand({collStats: 'baz'}), authzErrorCode);
assert.commandFailedWithCode(A_test.runCommand({collStats: 'foobar'}), authzErrorCode);

assert.commandWorked(B_test.runCommand({dbStats: 1}));
assert.commandFailedWithCode(B_test.runCommand({collStats: 'foo'}), authzErrorCode);
assert.commandFailedWithCode(B_test.runCommand({collStats: 'bar'}), authzErrorCode);
assert.commandFailedWithCode(B_test.runCommand({collStats: 'baz'}), authzErrorCode);
assert.commandFailedWithCode(B_test.runCommand({collStats: 'foobar'}), authzErrorCode);

jsTestLog("Doing writes that will eventually be rolled back");

// down A and wait for B to become primary
A_test.logout();
replTest.stop(0);
assert.soon(function() {
    try {
        return B_admin.hello().isWritablePrimary;
    } catch (e) {
        return false;
    }
}, "B didn't become primary");
printjson(assert.commandWorked(B_test.adminCommand('replSetGetStatus')));
B_test.logout();

// Modify the the user and role in a way that will be rolled back.
assert(B_admin.auth('admin', 'pwd'));
B_test.grantPrivilegesToRole(
    'myRole',
    [{resource: {db: 'test', collection: 'foo'}, actions: ['collStats']}],
    {});  // Default write concern will wait for majority, which will time out.
B_test.createRole({
    role: 'temporaryRole',
    roles: [],
    privileges: [{resource: {db: 'test', collection: 'bar'}, actions: ['collStats']}]
},
                  {});  // Default write concern will wait for majority, which will time out.
B_test.grantRolesToUser('spencer',
                        ['temporaryRole'],
                        {});  // Default write concern will wait for majority, which will time out.
B_admin.logout();

assert(B_test.auth('spencer', 'pwd'));
assert.commandWorked(B_test.runCommand({dbStats: 1}));
assert.commandWorked(B_test.runCommand({collStats: 'foo'}));
assert.commandWorked(B_test.runCommand({collStats: 'bar'}));
assert.commandFailedWithCode(B_test.runCommand({collStats: 'baz'}), authzErrorCode);
assert.commandFailedWithCode(B_test.runCommand({collStats: 'foobar'}), authzErrorCode);
B_test.logout();

// down B, bring A back up, then wait for A to become primary
// insert new data into A so that B will need to rollback when it reconnects to A
replTest.stop(1);

replTest.restart(0);
assert.soon(function() {
    try {
        return A_admin.hello().isWritablePrimary;
    } catch (e) {
        return false;
    }
}, "A didn't become primary");

// A should not have the new data as it was down
assert(A_test.auth('spencer', 'pwd'));
assert.commandWorked(A_test.runCommand({dbStats: 1}));
assert.commandFailedWithCode(A_test.runCommand({collStats: 'foo'}), authzErrorCode);
assert.commandFailedWithCode(A_test.runCommand({collStats: 'bar'}), authzErrorCode);
assert.commandFailedWithCode(A_test.runCommand({collStats: 'baz'}), authzErrorCode);
assert.commandFailedWithCode(A_test.runCommand({collStats: 'foobar'}), authzErrorCode);
A_test.logout();

jsTestLog("Doing writes that should persist after the rollback");
// Modify the user and role in a way that will persist.
A_admin.auth('userAdmin', 'pwd');
// Default write concern will wait for majority, which would time out
// so we override it with an empty write concern
A_test.grantPrivilegesToRole(
    'myRole', [{resource: {db: 'test', collection: 'baz'}, actions: ['collStats']}], {});

A_test.createRole({
    role: 'persistentRole',
    roles: [],
    privileges: [{resource: {db: 'test', collection: 'foobar'}, actions: ['collStats']}]
},
                  {});
A_test.grantRolesToUser('spencer', ['persistentRole'], {});
A_admin.logout();

A_test.auth('spencer', 'pwd');

// A has the data we just wrote, but not what B wrote before
checkFinalResults(A_test);

jsTestLog("Triggering rollback");

// bring B back in contact with A
// as A is primary, B will roll back and then catch up
replTest.restart(1);
assert.soonNoExcept(function() {
    authutil.asCluster(replTest.nodes, 'jstests/libs/key1', function() {
        replTest.awaitReplication();
    });

    return B_test.auth('spencer', 'pwd');
});
// Now both A and B should agree
checkFinalResults(A_test);
checkFinalResults(B_test);

A_test.logout();

// Verify data consistency between nodes.
authutil.asCluster(replTest.nodes, 'jstests/libs/key1', function() {
    replTest.checkOplogs();
});

// DB hash check is done in stopSet.
replTest.stopSet();
}());
