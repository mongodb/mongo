// Test that initial sync aborts when it encounters auth data from unsupported
// auth schemas (see: SERVER-17671)

function testInitialSyncAbortsWithUnsupportedAuthSchema(schema) {
    'use strict';

    // Create a replica set with one data-bearing node and one arbiter to
    // ensure availability when the added node fasserts later in the test
    var rst = new ReplSetTest({nodes: {n0: {}, arbiter: {}}});
    rst.startSet();
    rst.initiate();

    // Simulate unsupported auth data by setting the auth schema version to an
    // invalid or outdated version
    var versionColl = rst.getPrimary().getDB('admin').system.version;
    var res = versionColl.insert(schema);
    assert.writeOK(res);

    // Add another node to the replica set to allow an initial sync to occur
    rst.add();

    clearRawMongoProgramOutput();
    try {
        rst.reInitiate();
    }
    catch (e) {
        // reInitiate can throw because it tries to run an ismaster command on
        // all secondaries, including the new one that may have already aborted
        if (tojson(e).indexOf('error doing query: failed') === -1) {
            throw e;
        }
    }

    var msg;
    if (schema.hasOwnProperty('currentVersion')) {
        msg = new RegExp('During initial sync, found auth schema version ' + schema.currentVersion);
    } else {
        msg = /During initial sync, found malformed auth schema version/;
    }

    var assertFn = function() {
        return rawMongoProgramOutput().match(msg);
    };
    assert.soon(assertFn, 'Initial sync should have aborted due to an invalid or unsupported' +
                          ' authSchema version: ' + tojson(schema), 60000);

    rst.stopSet();
}

function testInitialSyncAbortsWithExistingUserAndNoAuthSchema() {
    'use strict';

    // Create a replica set with one data-bearing node and one arbiter to
    // ensure availability when the added node fasserts later in the test
    var rst = new ReplSetTest({nodes: {n0: {}, arbiter: {}}});
    rst.startSet();
    rst.initiate();

    // Simulate unsupported auth data by inserting a user document without inserting
    // a corresponding auth schema
    var userColl = rst.getPrimary().getDB('admin').system.users;
    var res = userColl.insert({});
    assert.writeOK(res);

    // Add another node to the replica set to allow an initial sync to occur
    rst.add();

    clearRawMongoProgramOutput();
    try {
        rst.reInitiate();
    }
    catch (e) {
        // reInitiate can throw because it tries to run an ismaster command on
        // all secondaries, including the new one that may have already aborted
        if (tojson(e).indexOf('error doing query: failed') === -1) {
            throw e;
        }
    }

    var msg = /During initial sync, found documents in admin\.system\.users/;
    var assertFn = function() {
        return rawMongoProgramOutput().match(msg);
    };

    assert.soon(assertFn, 'Initial sync should have aborted due to an existing user document and' +
                          ' a missing auth schema', 60000);

    rst.stopSet();
}

testInitialSyncAbortsWithUnsupportedAuthSchema({_id: 'authSchema'});
testInitialSyncAbortsWithUnsupportedAuthSchema({_id: 'authSchema', currentVersion: 1});
testInitialSyncAbortsWithUnsupportedAuthSchema({_id: 'authSchema', currentVersion: 2});
testInitialSyncAbortsWithExistingUserAndNoAuthSchema();
