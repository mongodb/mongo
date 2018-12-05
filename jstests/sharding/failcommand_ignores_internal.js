// Tests that the "failCommand" failpoint ignores commands from internal clients: SERVER-34943.
(function() {
    "use strict";

    const st = new ShardingTest({shards: 1});
    const mongosDB = st.s0.getDB("test_failcommand_ignores_internal");

    // Enough documents for three getMores.
    assert.commandWorked(mongosDB.collection.insertMany([{}, {}, {}]));
    const findReply = assert.commandWorked(mongosDB.runCommand({find: "collection", batchSize: 0}));
    const cursorId = findReply.cursor.id;

    // Test failing "getMore" twice with a particular error code.
    assert.commandWorked(mongosDB.adminCommand({
        configureFailPoint: "failCommand",
        mode: {times: 2},
        data: {errorCode: ErrorCodes.BadValue, failCommands: ["getMore"]}
    }));
    const getMore = {getMore: cursorId, collection: "collection", batchSize: 1};
    assert.commandFailedWithCode(mongosDB.runCommand(getMore), ErrorCodes.BadValue);
    assert.commandFailedWithCode(mongosDB.runCommand(getMore), ErrorCodes.BadValue);
    assert.commandWorked(mongosDB.runCommand(getMore));

    // Setting a failpoint for "distinct" on a shard has no effect on mongos.
    assert.commandWorked(st.shard0.getDB("admin").runCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {errorCode: ErrorCodes.BadValue, failCommands: ["distinct"]}
    }));
    const distinct = {distinct: "collection", key: "x"};
    assert.commandFailedWithCode(
        st.shard0.getDB("test_failcommand_ignores_internal").runCommand(distinct),
        ErrorCodes.BadValue);
    assert.commandWorked(mongosDB.runCommand(distinct));
    assert.commandWorked(
        st.shard0.getDB("admin").runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Setting a failpoint for "distinct" on a shard with failInternalCommands DOES affect mongos.
    assert.commandWorked(st.shard0.getDB("admin").runCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {
            errorCode: ErrorCodes.BadValue,
            failCommands: ["distinct"],
            failInternalCommands: true
        }
    }));
    assert.commandFailedWithCode(mongosDB.runCommand(distinct), ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        st.shard0.getDB("test_failcommand_ignores_internal").runCommand(distinct),
        ErrorCodes.BadValue);

    st.stop();
}());
