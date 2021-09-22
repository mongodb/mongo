/**
 * Tests that retryable write errors in MongoS return a top level error message.
 */

(function() {
    "use strict";

    load('jstests/libs/parallelTester.js');  // for ScopedThread.

    const dbName = "test";
    const collName = "retryable_mongos_write_errors";
    const ns = dbName + "." + collName;

    const st = new ShardingTest({config: 1, mongos: 1, shards: 1});
    const shard0Primary = st.rs0.getPrimary();

    // Creates a new connection, uses it to get the database from the parameter name and inserts
    // multiple documents to the provided collection.
    function insertHandler(host, dbName, collName, testData) {
        try {
            TestData = testData;
            const conn = new Mongo(host);
            const database = conn.getDB(dbName);
            // creates an array with 10 documents
            const docs = Array.from(Array(10).keys()).map((i) => ({a: i, b: "retryable"}));
            const commandResponse = database.runCommand({insert: collName, documents: docs});
            // assert that retryableInsertRes failed with the HostUnreachableError or
            // InterruptedAtShutdown error code
            assert.commandFailedWithCode(commandResponse, ErrorCodes.InterruptedAtShutdown);
            jsTest.log("Command Response: " + tojson(commandResponse) + "." + commandResponse.code);
            return {ok: 1};
        } catch (e) {
            if (!isNetworkError(e)) {
                return {ok: 0, error: e.toString(), stack: e.stack};
            }

            return {ok: 1};
        }
    }

    const failpointName = 'hangAfterCollectionInserts';
    const executeFailPointCommand = (mode) => {
        assert.commandWorked(shard0Primary.adminCommand(
            {configureFailPoint: failpointName, mode, data: {collectionNS: ns}}));
    };

    executeFailPointCommand("alwaysOn");

    const insertThread = new ScopedThread(insertHandler, st.s.host, dbName, collName, TestData);
    jsTest.log("Starting To Insert Documents");
    insertThread.start();

    checkLog.contains(shard0Primary, `${failpointName} fail point enabled`);
    jsTest.log("Starting to shutdown MongoS.");
    MongoRunner.stopMongos(st.s);

    try {
        assert.commandWorked(insertThread.returnData());
    } finally {
        jsTest.log("Finished Assertions, Turning Off Failpoint");
        executeFailPointCommand("off");
    }

    st.s = MongoRunner.runMongos(st.s);

    jsTest.log('Shutting down sharding test');
    st.stop();
}());