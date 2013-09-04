// Ensure that inserts and updates of the system.users collection validate the schema of inserted
// documents.

mydb = db.getSisterDB( "validate_user_documents" );

function assertGLEOK(status) {
    assert(status.ok && status.err === null,
           "Expected OK status object; found " + tojson(status));
}

function assertGLENotOK(status) {
    assert(status.ok && status.err !== null,
           "Expected not-OK status object; found " + tojson(status));
}

mydb.dropDatabase();
mydb.removeAllUsers();

//
// Tests of the insert path
//

// V0 user document document; insert should fail.
assert.commandFailed(mydb.runCommand({ createUser:1,
                                       user: "spencer",
                                       pwd: "password",
                                       readOnly: true }));

// V1 user document; insert should fail.
assert.commandFailed(mydb.runCommand({ createUser:1,
                                       user: "spencer",
                                       userSource: "test2",
                                       roles: ["dbAdmin"] }));

// Valid V2 user document; insert should succeed.
assert.commandWorked(mydb.runCommand({ createUser: "spencer",
                                       pwd: "password",
                                       roles: ["dbAdmin"] }));

// Valid V2 user document; insert should succeed.
assert.commandWorked(mydb.runCommand({ createUser: "andy",
                                       pwd: "password",
                                       roles: [{name: "dbAdmin",
                                                source: "validate_user_documents",
                                                hasRole: true,
                                                canDelegate: false}] }));

// Non-existent role; insert should fail
assert.commandFailed(mydb.runCommand({ createUser: "bob",
                                       pwd: "password",
                                       roles: ["fakeRole123"] }));

//
// Tests of the update path
//

/* Disabled per SERVER-10249.
// Update a document in a legal way, expect success.
mydb.system.users.update({user: "spencer", userSource: null}, { $set: {readOnly: false} });
assertGLEOK(mydb.getLastErrorObj());


// Update a document in a way that is illegal, expect failure.
mydb.system.users.update({user: "spencer", userSource: null}, { $unset: {pwd: 1} });
assertGLENotOK(mydb.getLastErrorObj());
*/

mydb.dropDatabase();
