/**
 * Tests that retryable write errors in MongoS return a top level error message.
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallelTester.js');  // for ScopedThread.

// Creates a new connection, uses it to get the database from the parameter name and inserts
// multiple documents to the provided collection.
function insertHandler(host, databaseName, collectionName) {
    const conn = new Mongo(host);
    const database = conn.getDB(databaseName);
    // creates an array with 10 documents
    const docs = Array.from(Array(10).keys()).map((i) => ({a: i, b: "retryable"}));
    return database.runCommand({insert: collectionName, documents: docs});
}

const dbName = "test";
const collName = "retryable_mongos_write_errors";
const ns = dbName + "." + collName;

const st = new ShardingTest({config: 1, mongos: 1, shards: 1});
const shard0Primary = st.rs0.getPrimary();

const insertFailPoint =
    configureFailPoint(shard0Primary, "hangAfterCollectionInserts", {collectionNS: ns});

const insertThread = new Thread(insertHandler, st.s.host, dbName, collName);
jsTest.log("Starting To Insert Documents");
insertThread.start();
insertFailPoint.wait();
MongoRunner.stopMongos(st.s);

try {
    const commandResponse = insertThread.returnData();
    jsTest.log("Command Response: " + tojson(commandResponse) + "." + commandResponse.code);
    // assert that retryableInsertRes failed with the HostUnreachableError or
    // InterruptedAtShutdown error code
    assert.eq(commandResponse.code, ErrorCodes.InterruptedAtShutdown, tojson(commandResponse));
} catch (e) {
    jsTest.log("Error ocurred: " + e);
    if (!isNetworkError(e)) {
        throw e;
    }
}

jsTest.log("Finished Assertions, Turning Off Failpoint");

insertFailPoint.off();
st.s = MongoRunner.runMongos(st.s);

jsTest.log('Shutting down sharding test');
st.stop();
}());