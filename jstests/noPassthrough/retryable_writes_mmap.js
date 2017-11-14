/*
 * Verify that retryable writes aren't allowed on mmapv1, because it doesn't have document-level
 * locking.
 */
(function() {
    "use strict";

    if (jsTest.options().storageEngine !== "mmapv1") {
        jsTestLog("Storage engine is not mmapv1, skipping test");
        return;
    }

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    let testDB = rst.getPrimary().startSession({retryWrites: true}).getDatabase("test");

    assert.writeErrorWithCode(
        testDB.foo.insert({x: 1}),
        ErrorCodes.IllegalOperation,
        "expected command with txnNumber to fail without document-level locking");

    rst.stopSet();

    const st = new ShardingTest({shards: {rs0: {nodes: 1}}});

    testDB = st.s.startSession({retryWrites: true}).getDatabase("test");

    assert.writeErrorWithCode(
        testDB.foo.insert({x: 1}),
        ErrorCodes.IllegalOperation,
        "expected command with txnNumber to fail without document-level locking");

    st.stop();
}());
