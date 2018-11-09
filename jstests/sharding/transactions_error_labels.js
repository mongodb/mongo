// Test TransientTransactionErrors error label in mongos write commands.
// @tags: [uses_transactions, uses_single_shard_transaction]
(function() {
    "use strict";

    let st = new ShardingTest({shards: 1});

    let session = st.s.startSession();
    let sessionDB = session.getDatabase("test");

    st.rs0.nodes.forEach(function(node) {
        assert.commandWorked(node.getDB("admin").runCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {errorCode: ErrorCodes.WriteConflict, failCommands: ["insert"]}
        }));
    });

    session.startTransaction();

    let res = sessionDB.runCommand({
        insert: "foo",
        documents: [{_id: "insert-1"}],
        readConcern: {level: "snapshot"},
    });

    assert.commandFailedWithCode(res, ErrorCodes.WriteConflict);
    assert.eq(res.errorLabels, ["TransientTransactionError"]);

    st.rs0.nodes.forEach(function(node) {
        assert.commandWorked(
            node.getDB("admin").runCommand({configureFailPoint: "failCommand", mode: "off"}));
    });

    st.stop();
}());
