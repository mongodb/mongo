// Test TransientTransactionErrors error label in mongos write commands.
// @tags: [uses_transactions]
(function() {
    "use strict";

    let st = new ShardingTest({shards: 1});

    let session = st.s.startSession();
    let sessionDB = session.getDatabase("test");

    st.rs0.nodes.forEach(function(node) {
        // Sharding tests require failInternalCommands: true, since the mongos appears to mongod to
        // be an internal client.
        assert.commandWorked(node.getDB("admin").runCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                errorCode: ErrorCodes.WriteConflict,
                failCommands: ["insert"],
                failInternalCommands: true
            }
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
