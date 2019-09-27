/**
 * Test that a mapReduce job can write sharded output to a database
 * from a separate input database while authenticated to both.
 */
(function() {

"use strict";

// TODO SERVER-35447: Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;
const st = new ShardingTest(
    {name: "mrShardedOutputAuth", shards: 1, mongos: 1, other: {keyFile: 'jstests/libs/key1'}});

// Setup the users to the input, output and admin databases
const mongos = st.s;
let adminDb = mongos.getDB("admin");
adminDb.createUser({user: "user", pwd: "pass", roles: jsTest.adminUserRoles});

const authenticatedConn = new Mongo(mongos.host);
authenticatedConn.getDB('admin').auth("user", "pass");
adminDb = authenticatedConn.getDB("admin");

const configDb = authenticatedConn.getDB("config");

const inputDb = authenticatedConn.getDB("input");
inputDb.createUser({user: "user", pwd: "pass", roles: jsTest.basicUserRoles});

const outputDb = authenticatedConn.getDB("output");
outputDb.createUser({user: "user", pwd: "pass", roles: jsTest.basicUserRoles});

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
    assert(!configDb.collections.findOne().dropped, "no sharded collections");
}

function assertFailure(cmdResponse, configDb, outputDb) {
    assert.commandFailed(cmdResponse);
    assert.eq(outputDb.numbers_out.count(), 0, "map/reduce should not have succeeded");
}

// Setup a connection authenticated to both input and output db
const inputOutputAuthConn = new Mongo(mongos.host);
inputOutputAuthConn.getDB('input').auth("user", "pass");
inputOutputAuthConn.getDB('output').auth("user", "pass");
let cmdResponse = doMapReduce(inputOutputAuthConn, outputDb);
assertSuccess(cmdResponse, configDb, outputDb);

// setup a connection authenticated to only input db
const inputAuthConn = new Mongo(mongos.host);
inputAuthConn.getDB('input').auth("user", "pass");
cmdResponse = doMapReduce(inputAuthConn, outputDb);
assertFailure(cmdResponse, configDb, outputDb);

// setup a connection authenticated to only output db
const outputAuthConn = new Mongo(mongos.host);
outputAuthConn.getDB('output').auth("user", "pass");
cmdResponse = doMapReduce(outputAuthConn, outputDb);
assertFailure(cmdResponse, configDb, outputDb);

st.stop();
})();
