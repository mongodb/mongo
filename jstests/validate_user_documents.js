// Ensure that inserts and updates of the system.users collection validate the schema of inserted
// documents.

function assertGLEOK(status) {
    assert(status.ok && status.err === null,
           "Expected OK status object; found " + tojson(status));
}

function assertGLENotOK(status) {
    assert(status.ok && status.err !== null,
           "Expected not-OK status object; found " + tojson(status));
}

db.dropDatabase();

//
// Tests of the insert path
//

// Valid compatibility document; insert should succeed.
db.system.users.insert({ user: "spencer", pwd: hex_md5("spencer:mongo:a"), readOnly: true });
assertGLEOK(db.getLastErrorObj());

// Invalid compatibility document; insert should fail.
db.system.users.insert({ user: "andy", readOnly: true });
assertGLENotOK(db.getLastErrorObj());

// Valid extended document; insert should succeed.
db.system.users.insert({ user: "spencer", userSource: "test2", roles: ["dbAdmin"] });
assertGLEOK(db.getLastErrorObj());

// Invalid extended document; insert should fail.
db.system.users.insert({ user: "andy", userSource: "test2", roles: ["dbAdmin", 15] });
assertGLENotOK(db.getLastErrorObj());


//
// Tests of the update path
//

// Update a document in a legal way, expect success.
db.system.users.update({user: "spencer", userSource: null}, { $set: {readOnly: false} });
assertGLEOK(db.getLastErrorObj());


// Update a document in a way that is illegal, expect failure.
db.system.users.update({user: "spencer", userSource: null}, { $unset: {pwd: 1} });
assertGLENotOK(db.getLastErrorObj());
