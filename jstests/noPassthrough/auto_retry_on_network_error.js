/**
 * Tests that the auto_retry_on_network_error.js override automatically retries commands on network
 * errors for commands run under a session.
 */
(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    load("jstests/libs/override_methods/auto_retry_on_network_error.js");
    load("jstests/replsets/rslib.js");

    function stepDownPrimary(rst) {
        // Since we expect the mongo shell's connection to get severed as a result of running the
        // "replSetStepDown" command, we temporarily disable the retry on network error behavior.
        TestData.skipRetryOnNetworkError = true;
        try {
            const primary = rst.getPrimary();
            const error = assert.throws(function() {
                const res = primary.adminCommand({replSetStepDown: 1, force: true});
                print("replSetStepDown did not throw exception but returned: " + tojson(res));
            });
            assert(isNetworkError(error),
                   "replSetStepDown did not disconnect client; failed with " + tojson(error));

            // We use the reconnect() function to run a command against the former primary that
            // acquires the global lock to ensure that it has finished stepping down and has
            // therefore closed all of its client connections. This ensures commands sent on other
            // connections to the former primary trigger a network error rather than potentially
            // returning a "not master" error while the server is in the midst of closing client
            // connections.
            reconnect(primary);
        } finally {
            TestData.skipRetryOnNetworkError = false;
        }
    }

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const dbName = "test";
    const collName = "auto_retry";

    // The override requires the connection to be run under a session. Use the replica set URL to
    // allow automatic re-targeting of the primary on NotMaster errors.
    const db = new Mongo(rst.getURL()).startSession({retryWrites: true}).getDatabase(dbName);

    // Commands with no stepdowns should work as normal.
    assert.commandWorked(db.runCommand({ping: 1}));
    assert.commandWorked(db.runCommandWithMetadata({ping: 1}, {}).commandReply);

    // Read commands are automatically retried on network errors.
    stepDownPrimary(rst);
    assert.commandWorked(db.runCommand({find: collName}));

    stepDownPrimary(rst);
    assert.commandWorked(db.runCommandWithMetadata({find: collName}, {}).commandReply);

    // Retryable write commands that can be retried succeed.
    stepDownPrimary(rst);
    assert.writeOK(db[collName].insert({x: 1}));

    stepDownPrimary(rst);
    assert.commandWorked(db.runCommandWithMetadata({
                               insert: collName,
                               documents: [{x: 2}, {x: 3}],
                               txnNumber: NumberLong(10),
                               lsid: {id: UUID()}
                           },
                                                   {})
                             .commandReply);

    // Retryable write commands that cannot be retried (i.e. no transaction number, no session id,
    // or are unordered) throw.
    stepDownPrimary(rst);
    assert.throws(function() {
        db.runCommand({insert: collName, documents: [{x: 1}, {x: 2}], ordered: false});
    });

    // The previous command shouldn't have been retried, so run a command to successfully re-target
    // the primary, so the connection to it can be closed.
    assert.commandWorked(db.runCommandWithMetadata({ping: 1}, {}).commandReply);

    stepDownPrimary(rst);
    assert.throws(function() {
        db.runCommandWithMetadata({insert: collName, documents: [{x: 1}, {x: 2}], ordered: false},
                                  {});
    });

    // getMore commands can't be retried because we won't know whether the cursor was advanced or
    // not.
    let cursorId = assert.commandWorked(db.runCommand({find: collName, batchSize: 0})).cursor.id;
    stepDownPrimary(rst);
    assert.throws(function() {
        db.runCommand({getMore: cursorId, collection: collName});
    });

    cursorId = assert.commandWorked(db.runCommand({find: collName, batchSize: 0})).cursor.id;
    stepDownPrimary(rst);
    assert.throws(function() {
        db.runCommandWithMetadata({getMore: cursorId, collection: collName}, {});
    });

    rst.stopSet();
})();
