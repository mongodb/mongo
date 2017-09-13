// Helpers for auth upgrade tests.

// Get a user document for username in db.
var getUserDoc = function(db, username) {
    return db.runCommand({'usersInfo': {user: username, db: db._name}, showCredentials: true})
        .users[0];
};

// Verify that the user document for username in db
// has MONGODB-CR credentials (or not) and SCRAM-SHA-1
// credentials (or not).
var verifyUserDoc = function(db, username, hasCR, hasSCRAM, hasExternal = false) {
    var userDoc = getUserDoc(db, username);
    assert.eq(hasCR, 'MONGODB-CR' in userDoc.credentials);
    assert.eq(hasSCRAM, 'SCRAM-SHA-1' in userDoc.credentials);
    assert.eq(hasExternal, 'external' in userDoc.credentials);
};

// Verify that that we can authenticate (or not) using MONGODB-CR
// and SCRAM-SHA-1 to db using username and password.
var verifyAuth = function(db, username, password, passCR, passSCRAM) {
    assert.eq(passCR, db.auth({mechanism: 'MONGODB-CR', user: username, pwd: password}));
    assert.eq(passSCRAM, db.auth({mechanism: 'SCRAM-SHA-1', user: username, pwd: password}));
    db.logout();
};
