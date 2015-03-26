// Test that initial sync aborts when it encounters auth data from unsupported
// auth schemas (see: SERVER-17671)

function testInitialSyncAbortsWithUnsupportedAuthSchema(schema) {
    'use strict';

    var rst = new ReplSetTest({nodes: 1});
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
    rst.reInitiate();

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

    var rst = new ReplSetTest({nodes: 1});
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
    rst.reInitiate();

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
