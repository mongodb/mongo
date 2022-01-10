/**
 * Tests that a standalone node started with --verbose, --profile, and --keyfile can
 * successfully run the replSetInitiate command.
 */
(function() {
let options = {verbose: 1, profile: 1, keyFile: 'jstests/libs/key1', replSet: "rs0", port: 27017};

const conn = MongoRunner.runMongod(options);

assert.commandWorked(conn.getDB('admin').runCommand({
    replSetInitiate: {
        "_id": "rs0",
        "members": [{"_id": 0, "host": "127.0.0.1:27017"}],
        writeConcernMajorityJournalDefault: false
    }
}));

assert.soon(function() {
    const res = assert.commandWorked(conn.adminCommand({hello: 1}));
    return res.isWritablePrimary;
});

const admin = conn.getDB('admin');
admin.createUser({user: 'foo', pwd: 'bar', roles: jsTest.adminUserRoles});
assert(admin.auth({user: 'foo', pwd: 'bar'}));
assert.commandWorked(admin.test.insert({x: 1}));
MongoRunner.stopMongod(conn);
})();
