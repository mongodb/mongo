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
mydb.system.users.insert({ user: "spencer", pwd: hex_md5("spencer:mongo:a"), readOnly: true });
assertGLEOK(mydb.getLastErrorObj());

// Invalid compatibility document; insert should fail.
mydb.system.users.insert({ user: "andy", readOnly: true });
assertGLENotOK(mydb.getLastErrorObj());

// Valid extended document; insert should succeed.
mydb.system.users.insert({ user: "spencer", userSource: "test2", roles: ["dbAdmin"] });
assertGLEOK(mydb.getLastErrorObj());

// Invalid extended document; insert should fail.
mydb.system.users.insert({ user: "andy", userSource: "test2", roles: ["dbAdmin", 15] });
assertGLENotOK(mydb.getLastErrorObj());


//
// Tests of the update path
//

// Update a document in a legal way, expect success.
mydb.system.users.update({user: "spencer", userSource: null}, { $set: {readOnly: false} });
assertGLEOK(mydb.getLastErrorObj());


// Update a document in a way that is illegal, expect failure.
mydb.system.users.update({user: "spencer", userSource: null}, { $unset: {pwd: 1} });
assertGLENotOK(mydb.getLastErrorObj());

mydb.dropDatabase();
