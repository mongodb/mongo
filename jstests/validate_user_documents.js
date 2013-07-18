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

//
// Tests of the insert path
//

// Valid compatibility document; insert should succeed.
assert.commandWorked(mydb.runCommand({ createUser:1,
                                       user: "spencer",
                                       pwd: "spencer",
                                       readOnly: true }));

// Invalid compatibility document; insert should fail.
assert.commandFailed(mydb.runCommand({ createUser:1, user: "andy", readOnly: true }));

// Valid extended document; insert should succeed.
assert.commandWorked(mydb.runCommand({ createUser:1,
                                       user: "spencer",
                                       userSource: "test2",
                                       roles: ["dbAdmin"] }));

// Invalid extended document; insert should fail.
assert.commandFailed(mydb.runCommand({ createUser:1,
                                       user: "andy",
                                       userSource: "test2",
                                       roles: ["dbAdmin", 15] }));


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
