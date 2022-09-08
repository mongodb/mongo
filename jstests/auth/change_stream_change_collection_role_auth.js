/**
 * Tests that when RBAC (Role-Based Access Control) is enabled on the cluster, then the user of the
 * root role can issue 'find', 'insert', 'update', 'remove' commands on the
 * 'config.system.change_collection' collection.
 * @tags: [
 *  assumes_read_preference_unchanged,
 *  requires_replication,
 *  requires_fcv_62,
 * ]
 */
(function() {
'use strict';

const password = "password";
const keyFile = "jstests/libs/key1";

// A dictionary of users against which authorization of change collection will be verified.
// The format of dictionary is:
//   <key>: user-name,
//   <value>:
//       'db': authentication db for the user
//       'roles': roles assigned to the user
//       'privileges': allowed operations of the user
const users = {
    admin: {
        db: "admin",
        roles: [{role: "userAdminAnyDatabase", db: "admin"}],
        privileges: {
            insert: false,
            find: false,
            update: false,
            remove: false,
        },
    },
    clusterAdmin: {
        db: "admin",
        roles: [{"role": "clusterAdmin", "db": "admin"}],
        privileges: {
            insert: false,
            find: false,
            update: false,
            remove: false,
        },
    },
    test: {
        db: "test",
        roles: [{"role": "readWrite", "db": "test"}, {"role": "readWrite", "db": "config"}],
        privileges: {
            insert: false,
            find: false,
            update: false,
            remove: false,
        },
    },
    root: {
        db: "admin",
        roles: ["root"],
        privileges: {
            insert: true,
            find: true,
            update: true,
            remove: true,
        }
    },
};

// Helper to authenticate and login the user on the provided connection.
function login(conn, userName) {
    const user = users[userName];
    const db = conn.getDB(user.db);
    assert(db.auth(userName, password));
    return db;
}

// Helper to create users from the 'users' dictionary on the provided connection.
function createUsers(conn) {
    function createUser(userName) {
        // A user-creation will happen by 'admin' user. If 'userName' is not 'admin', then login
        // first as an 'admin'.
        const adminDb = userName !== "admin" ? login(conn, "admin") : null;

        // Get the details of user to create and issue create command.
        const user = users[userName];
        conn.getDB(user.db).createUser({user: userName, pwd: password, roles: user.roles});

        // Logout if the current authenticated user is 'admin'.
        if (adminDb !== null) {
            adminDb.logout();
        }

        // Verify that the user has been created by logging-in and then logging out.
        const db = login(conn, userName);
        db.logout();
    }

    // Create the list of users on the specified connection.
    for (const userName of Object.keys(users)) {
        createUser(userName);
    }
}

// Helper to verify if the logged-in user is authorized to invoke 'actionFunc'.
// The parameter 'isAuthorized' determines if the authorization should pass or fail.
function assertActionAuthorized(actionFunc, isAuthorized) {
    try {
        actionFunc();

        // Verify if the authorization is expected to pass.
        assert.eq(isAuthorized, true, "authorization passed unexpectedly");
    } catch (ex) {
        // Verify if the authorization is expected to fail.
        assert.eq(isAuthorized, false, "authorization failed unexpectedly, details: " + ex);

        // Verify that the authorization failed with the expected error code.
        const unauthorized = 13;
        assert.eq(ex.code,
                  unauthorized,
                  "expected operation should fail with code: " + unauthorized +
                      ", found: " + ex.code + ", details: " + ex);
    }
}

const changeCollectionName = "system.change_collection";

// Helper to verify if the logged-in user is authorized to issue 'find' command.
// The parameter 'authDb' is the authentication db for the user, and 'numDocs' determines the
// least number of documents to be retrieved.
function findChangeCollectionDoc(authDb, numDocs = 1) {
    const db = authDb.getSiblingDB("config");
    const result = db.getCollection(changeCollectionName).find({_id: 0}).toArray();
    assert.eq(result.length, numDocs, result);
}

// Helper to verify if the logged-in user is authorized to issue 'insert' command.
// The parameter 'authDb' is the authentication db for the user.
function insertChangeCollectionDoc(authDb) {
    const db = authDb.getSiblingDB("config");
    assert.commandWorked(db.getCollection(changeCollectionName).insert({_id: 0}));
    findChangeCollectionDoc(authDb, 1 /* numDocs */);
}

// Helper to verify if the logged-in user is authorized to issue 'update' command.
// The parameter 'authDb' is the authentication db for the user.
function updateChangeCollectionDoc(authDb) {
    const db = authDb.getSiblingDB("config");
    assert.commandWorked(db.getCollection(changeCollectionName).update({_id: 0}, {$set: {x: 0}}));
    findChangeCollectionDoc(authDb, 1 /* numDocs */);
}

// Helper to verify if the logged-in user is authorized to issue 'remove' command.
// The parameter 'authDb' is the authentication db for the user.
function removeChangeCollectionDoc(authDb) {
    const db = authDb.getSiblingDB("config");
    assert.commandWorked(db.getCollection(changeCollectionName).remove({"_id": 0}));
    findChangeCollectionDoc(authDb, 0 /* numDocs */);
}

// Start a replica-set test with one-node and authentication enabled. Connect to the primary node
// and create users.
const replSetTest =
    new ReplSetTest({name: "shard", nodes: 1, useHostName: true, waitForKeys: false});

// TODO SERVER-67267: Add 'featureFlagServerlessChangeStreams', 'multitenancySupport' and
// 'serverless' flags and remove 'failpoint.forceEnableChangeCollectionsMode'.
replSetTest.startSet({
    keyFile: keyFile,
    setParameter: {"failpoint.forceEnableChangeCollectionsMode": tojson({mode: "alwaysOn"})}
});
replSetTest.initiate();
const primary = replSetTest.getPrimary();
const adminDb = primary.getDB('admin');
createUsers(primary);

// Connect to the 'root' user.
let testPrimary = login(primary, "root");

// Enable change streams to ensure the creation of change collections.
assert.commandWorked(adminDb.runCommand({setChangeStreamState: 1, enabled: true}));

// Create a collection, insert a document and logout.
assert.commandWorked(testPrimary.createCollection("testColl"));
testPrimary.logout();

// Test the privileges for every user and operation in the 'privilegeMap.
for (const userName of Object.keys(users)) {
    const user = login(primary, userName);

    for (const [op, isAuthorized] of Object.entries(users[userName]['privileges'])) {
        let opFunc;
        switch (op) {
            case 'insert':
                opFunc = insertChangeCollectionDoc.bind(null, user);
                break;
            case 'find':
                opFunc = findChangeCollectionDoc.bind(null, user);
                break;
            case 'update':
                opFunc = updateChangeCollectionDoc.bind(null, user);
                break;
            case 'remove':
                opFunc = removeChangeCollectionDoc.bind(null, user);
                break;
            default:
                assert(false);
        }

        jsTestLog(`${userName} is performing ${op} on ${changeCollectionName}`);
        assertActionAuthorized(opFunc, isAuthorized);
    }

    user.logout();
}

replSetTest.stopSet();
})();
