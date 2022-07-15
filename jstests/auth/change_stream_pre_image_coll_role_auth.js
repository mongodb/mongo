/**
 * Tests that when RBAC (Role-Based Access Control) is enabled on the cluster, then the user of the
 * root role can issue 'find' and 'remove' commands on the 'config.system.preimages' collections.
 * Verify that if pre-image for the collection has been deleted by the 'root' user.
 * @tags: [
 *  requires_fcv_60,
 *  uses_change_streams,
 *  assumes_read_preference_unchanged,
 *  requires_replication,
 * ]
 */
(function() {
'use strict';

const password = "password";
const keyFile = "jstests/libs/key1";

// A dictionary of users against which authorization of pre-image collection will be verified.
// The format of dictionary is:
//   <key>: user-name,
//   <value>:
//       'db': authentication db for the user
//       'roles': roles assigned to the user
const users = {
    admin: {db: "admin", roles: [{role: "userAdminAnyDatabase", db: "admin"}]},
    clusterAdmin: {db: "admin", roles: [{"role": "clusterAdmin", "db": "admin"}]},
    test: {
        db: "test",
        roles: [{"role": "readWrite", "db": "test"}, {"role": "readWrite", "db": "config"}]
    },
    root: {db: "admin", roles: ["root"]}
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

        // Logout if the current authenticated user is 'admin'
        if (adminDb !== null) {
            adminDb.logout();
        }

        // Verify that the user has been created by logging-in and then logging out.
        const db = login(conn, userName);
        db.logout();
    }

    // Create the list of users on the specified connection.
    createUser("admin");
    createUser("clusterAdmin");
    createUser("test");
    createUser("root");
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

// Helper to verify if the logged-in user is authorized to issue 'find' command.
// The parameter 'authDb' is the authentication db for the user, and 'numDocs' determines the
// expected number of documents to be retrieved.
function findPreImage(authDb, numDocs = 1) {
    const db = authDb.getSiblingDB("config");
    const result = db.getCollection("system.preimages").find().toArray();
    assert.eq(result.length, numDocs, result);
}

// Helper to verify if the logged-in user is authorized to issue 'remove' command.
// The parameter 'authDb' is the authentication db for the user.
function removePreImage(authDb) {
    const db = authDb.getSiblingDB("config");
    assert.commandWorked(db.getCollection("system.preimages").remove({"preImage._id": 0}));
    findPreImage(authDb, 0 /* numDocs */);
}

// Verify that the expected number of change stream events are observed using the supplied resume
// token.
function verifyChangeStreamEvents(authDb, resumeToken) {
    const csCursor = authDb.getSiblingDB("test").watch([], {
        resumeAfter: resumeToken,
        fullDocument: "whenAvailable",
        fullDocumentBeforeChange: "whenAvailable"
    });

    assert.soon(() => csCursor.hasNext());
    const event1 = csCursor.next();
    assert.eq(event1.documentKey._id, 0, event1);
    assert.eq(event1.fullDocument, {_id: 0, annotation: "inserted"}, event1);

    assert.soon(() => csCursor.hasNext());
    const event2 = csCursor.next();
    assert.eq(event2.documentKey._id, 0, event2);

    assert.eq(event2.fullDocument, {_id: 0, annotation: "updated"}, event2);
    assert.eq(event2.fullDocumentBeforeChange, {_id: 0, annotation: "inserted"}, event2);
}

// Start a replica-set test with one-node and authentication enabled. Connect to the primary node
// and create users.
const replSetTest =
    new ReplSetTest({name: "shard", nodes: 1, useHostName: true, waitForKeys: false});
replSetTest.startSet({keyFile: keyFile});
replSetTest.initiate();
const primary = replSetTest.getPrimary();
createUsers(primary);

// Connect to the 'test' user.
let testPrimary = login(primary, "test");

// Get the resume token of the change streams.
const csResumeToken = testPrimary.watch([]).getResumeToken();

// Create a collection, insert a document and then update to create pre-image.
testPrimary.createCollection("testColl", {changeStreamPreAndPostImages: {enabled: true}});
assert.commandWorked(testPrimary.testColl.insert({_id: 0, annotation: "inserted"}));
assert.commandWorked(testPrimary.testColl.update({_id: 0}, {$set: {annotation: "updated"}}));

// Verify that with RBAC enabled, the change streams observes the events with pre-and-post images.
// Verify that the 'test' user is not authorized to issue 'find' command on the pre-image
// collection.
assertActionAuthorized(verifyChangeStreamEvents.bind(null, testPrimary, csResumeToken), true);
assertActionAuthorized(findPreImage.bind(null, testPrimary), false);
testPrimary.logout();

// User 'clusterAdmin' should not be authorized to find the pre-images and open change-streams with
// pre-and-post images.
let clusterAdminPrimary = login(primary, "clusterAdmin");
assertActionAuthorized(verifyChangeStreamEvents.bind(null, clusterAdminPrimary, csResumeToken),
                       false);
assertActionAuthorized(findPreImage.bind(null, clusterAdminPrimary), false);
clusterAdminPrimary.logout();

// User 'admin' should not be authorized to find the pre-images and open change-streams with
// pre-and-post images.
let adminPrimary = login(primary, "admin");
assertActionAuthorized(verifyChangeStreamEvents.bind(null, clusterAdminPrimary, csResumeToken),
                       false);
assertActionAuthorized(findPreImage.bind(null, adminPrimary), false);
adminPrimary.logout();

// User 'root' should be authorized to find the pre-images and open change-streams with pre-and-post
// images.
let rootPrimary = login(primary, "root");
assertActionAuthorized(verifyChangeStreamEvents.bind(null, clusterAdminPrimary, csResumeToken),
                       true);
assertActionAuthorized(findPreImage.bind(null, rootPrimary), true);
rootPrimary.logout();

// User 'test' should not be authorized to remove pre-images.
testPrimary = login(primary, "test");
assertActionAuthorized(removePreImage.bind(null, testPrimary), false);
testPrimary.logout();

// User 'clusterAdmin' should not be authorized to remove pre-images.
clusterAdminPrimary = login(primary, "clusterAdmin");
assertActionAuthorized(removePreImage.bind(null, clusterAdminPrimary), false);
clusterAdminPrimary.logout();

// User 'admin' should not be authorized to remove pre-images.
adminPrimary = login(primary, "admin");
assertActionAuthorized(removePreImage.bind(null, adminPrimary), false);
adminPrimary.logout();

// User 'root' should be authorized to remove pre-images.
rootPrimary = login(primary, "root");
assertActionAuthorized(removePreImage.bind(null, rootPrimary), true);
rootPrimary.logout();

replSetTest.stopSet();
})();