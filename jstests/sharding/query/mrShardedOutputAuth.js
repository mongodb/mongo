/**
 * Test that a mapReduce job can write sharded output to a database
 * from a separate input database while authenticated to both.
 */
(function() {

"use strict";

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;
const st = new ShardingTest(
    {name: "mrShardedOutputAuth", shards: 1, mongos: 1, other: {keyFile: 'jstests/libs/key1'}});

// Setup the users to the input, output and admin databases
const authenticatedConn = st.s;
const adminDb = authenticatedConn.getDB('admin');
adminDb.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
assert(adminDb.auth('admin', 'pass'));

assert.commandWorked(adminDb.adminCommand({enablesharding: "output"}));

const configDb = authenticatedConn.getDB("config");
const inputDb = authenticatedConn.getDB("input");
const outputDb = authenticatedConn.getDB("output");
assert.commandWorked(adminDb.runCommand({enableSharding: outputDb.getName()}));

const nDocs = 50;
// Setup the input db
inputDb.numbers.drop();
for (let i = 0; i < nDocs; i++) {
    inputDb.numbers.insert({num: i});
}
assert.eq(inputDb.numbers.count(), nDocs);

function doMapReduce(connection, outputDb) {
    // clean output db and run m/r
    outputDb.numbers_out.drop();
    assert.commandWorked(
        adminDb.runCommand({shardCollection: outputDb.numbers_out.getFullName(), key: {_id: 1}}));

    return connection.getDB('input').runCommand({
        mapreduce: "numbers",
        map: function() {
            emit(this.num, {count: 1});
        },
        reduce: function(k, values) {
            const result = {};
            values.forEach(function(value) {
                result.count = 1;
            });
            return result;
        },
        out: {merge: "numbers_out", sharded: true, db: "output"},
        verbose: true,
        query: {}
    });
}

function assertSuccess(cmdResponse, configDb, outputDb) {
    assert.commandWorked(cmdResponse);
    assert.eq(outputDb.numbers_out.count(), nDocs, "map/reduce failed");
    assert.eq(1, configDb.collections.countDocuments({_id: "output.numbers_out"}));
}

function assertFailure(cmdResponse, configDb, outputDb) {
    assert.commandFailed(cmdResponse);
    assert.eq(outputDb.numbers_out.count(), 0, "map/reduce should not have succeeded");
}

function runTest(user, roles, assertion) {
    authenticatedConn.getDB(user.db).createUser({user: user.user, pwd: 'pass', roles: roles});

    const conn = new Mongo(st.s.host);
    assert(conn.getDB(user.db).auth(user.user, 'pass'));
    const response = doMapReduce(conn, outputDb);
    assertion(response, configDb, outputDb);
}

// Setup a connection authenticated to both input and output db
runTest({user: 'inout', db: 'admin'}, ['readWriteAnyDatabase'], assertSuccess);

// setup a connection authenticated to only input db
runTest({user: 'inonly', db: 'input'}, ['readWrite'], assertFailure);

// setup a connection authenticated to only output db
runTest({user: 'outOnly', db: 'output'}, ['readWrite'], assertFailure);

st.stop();
})();
