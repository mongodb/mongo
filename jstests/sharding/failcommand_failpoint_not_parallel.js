(function() {
    "use strict";

    const st = new ShardingTest({shards: 3, mongos: 1});
    const db = st.s.getDB("test_failcommand_noparallel");

    // Test times when closing connection.
    assert.commandWorked(st.s.adminCommand({
        configureFailPoint: "failCommand",
        mode: {times: 2},
        data: {
            closeConnection: true,
            failCommands: ["find"],
        }
    }));
    assert.throws(() => db.runCommand({find: "c"}));
    assert.throws(() => db.runCommand({find: "c"}));
    assert.commandWorked(db.runCommand({find: "c"}));
    assert.commandWorked(st.s.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    st.stop();
}());
